/**
 * useEventStream — singleton SSE connection.
 *
 * Exports:
 *  - EventBusProvider   — mount once after login
 *  - useEventStream()   — subscribe to filtered live events
 *  - useEventStreamHistory() — read ring-buffer for EventsPage (§ 18.2)
 *
 * Transport is fetch + ReadableStream (see api/sseClient.ts) — EventSource
 * cannot send the Authorization header, so we run SSE through fetch to
 * keep a single auth path with the REST API.
 */
import React from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { useAuthStore } from '@/stores/auth';
import { useUiStore } from '@/stores/ui';
import type { SseEvent } from '@/api/types';
import { runSseLoop } from '@/api/sseClient';

const BUFFER_SIZE = 256;

// ─── Module-level ring buffer ─────────────────────────────────────────────────
const eventBuffer: SseEvent[] = [];
type Listener = (event: SseEvent) => void;
const listeners = new Set<Listener>();

// Subscribers that want the full ring-buffer snapshot (EventsPage)
type HistoryListener = (events: SseEvent[]) => void;
const historyListeners = new Set<HistoryListener>();

function emit(event: SseEvent) {
  eventBuffer.push(event);
  if (eventBuffer.length > BUFFER_SIZE) eventBuffer.shift();
  listeners.forEach((fn) => fn(event));
  // Notify history subscribers with an immutable copy
  const snapshot = [...eventBuffer];
  historyListeners.forEach((fn) => fn(snapshot));
}

// ─── EventBusProvider ─────────────────────────────────────────────────────────

export const EventBusContext = React.createContext<{
  connected: boolean;
  overflowed: boolean;
}>({ connected: false, overflowed: false });

export function EventBusProvider({ children }: { children: React.ReactNode }) {
  const qc = useQueryClient();
  const token = useAuthStore((s) => s.accessToken);
  const setSseConnected  = useUiStore((s) => s.setSseConnected);
  const setSseOverflowed = useUiStore((s) => s.setSseOverflowed);
  const addToast         = useUiStore((s) => s.addToast);
  const [connected,  setConnected]  = React.useState(false);
  const [overflowed, setOverflowed] = React.useState(false);

  React.useEffect(() => {
    if (!token) return;

    const ac = new AbortController();

    runSseLoop({
      url:      new URL('/api/events/stream', window.location.origin).toString(),
      signal:   ac.signal,
      getToken: () => useAuthStore.getState().accessToken,

      refreshToken: async () => {
        const rt = useAuthStore.getState().refreshToken;
        if (!rt) throw new Error('no refresh token');
        const res = await fetch('/api/auth/refresh', {
          method:  'POST',
          headers: { 'Content-Type': 'application/json' },
          body:    JSON.stringify({ refresh_token: rt }),
        });
        if (!res.ok) throw new Error(`refresh ${res.status}`);
        const data = await res.json();
        useAuthStore.getState().setTokens(
          data.access_token, data.refresh_token, data.expires_in);
      },
      onAuthFailed: () => {
        useAuthStore.getState().logout();
      },

      onOpen: () => {
        setConnected(true); setSseConnected(true);
        setOverflowed(false); setSseOverflowed(false);
      },
      onDisconnect: () => {
        setConnected(false); setSseConnected(false);
      },

      onMessage: (msg) => {
        // Server-emitted error frames carry `event: error` and an
        // {"error":"…"} payload. The normal-frames branch below assumes
        // `data` is an SseEvent JSON object.
        if (msg.event === 'error') {
          try {
            const parsed = JSON.parse(msg.data);
            if (parsed?.error === 'event_bus_overflow') {
              setOverflowed(true); setSseOverflowed(true);
              addToast('SSE stream overflow — refetching state…', 'warning');
              qc.invalidateQueries();
            }
          } catch { /* malformed error frame — ignore */ }
          return;
        }

        let event: SseEvent;
        try { event = JSON.parse(msg.data) as SseEvent; }
        catch { return; }

        emit(event);

        // ─── Targeted query invalidation ─────────────────────────────────
        const chId = event.channel_id;
        switch (event.type) {
          case 'channel_state_change':
          case 'clip_change':
          case 'channel_health_change':
            qc.invalidateQueries({ queryKey: ['channels', chId] });
            qc.invalidateQueries({ queryKey: ['channels'] });
            break;
          case 'output_state_change':
            if (chId) qc.invalidateQueries({ queryKey: ['channels', chId, 'outputs'] });
            break;
          case 'schedule_active_change':
            if (chId) {
              qc.invalidateQueries({ queryKey: ['channels', chId, 'schedule', 'active'] });
              qc.invalidateQueries({ queryKey: ['channels', chId, 'schedule', 'upcoming'] });
            }
            break;
          case 'watcher_status_change':
            if (chId) qc.invalidateQueries({ queryKey: ['channels', chId, 'watcher'] });
            break;
          case 'stress_run_started':
          case 'stress_run_finished':
            qc.invalidateQueries({ queryKey: ['stress'] });
            break;
          case 'plugin_install':
          case 'plugin_uninstall':
            qc.invalidateQueries({ queryKey: ['plugins'] });
            break;
          // § 3 — Q3 in answers1.md: invalidate audit query on auth events
          case 'auth_audit':
            qc.invalidateQueries({ queryKey: ['audit'] });
            break;
          // Enterprise audit trail — one signal per row written to
          // state/audit.db. Payload is a compact snapshot; the trail
          // page refetches for the full row (mac, id, chain-position).
          case 'audit_event':
            qc.invalidateQueries({ queryKey: ['audit-trail'] });
            break;
          // fix33 D1 — gateway lifecycle: invalidate gateway list and the
          // affected gateway's status. Payload carries `id` (not channel_id).
          case 'gateway_state_change': {
            qc.invalidateQueries({ queryKey: ['gateways'] });
            const gwId = (event as { payload?: { id?: number } }).payload?.id;
            if (typeof gwId === 'number') {
              qc.invalidateQueries({ queryKey: ['gateways', gwId] });
            }
            break;
          }
        }
      },
    });

    return () => {
      ac.abort();
      setConnected(false); setSseConnected(false);
    };
  }, [token]); // eslint-disable-line react-hooks/exhaustive-deps

  const ctx = React.useMemo(() => ({ connected, overflowed }), [connected, overflowed]);
  return <EventBusContext.Provider value={ctx}>{children}</EventBusContext.Provider>;
}

// ─── useEventStream — filtered live subscriber ─────────────────────────────────

export function useEventStream(opts?: { types?: string[]; channelId?: number }) {
  const ctx = React.useContext(EventBusContext);
  const [localEvents, setLocalEvents] = React.useState<SseEvent[]>([]);

  React.useEffect(() => {
    const listener: Listener = (ev) => {
      if (opts?.types && !opts.types.includes(ev.type)) return;
      if (opts?.channelId !== undefined && ev.channel_id !== opts.channelId) return;
      setLocalEvents((prev) => [ev, ...prev].slice(0, 200));
    };
    listeners.add(listener);
    return () => { listeners.delete(listener); };
  }, [opts?.types?.join(','), opts?.channelId]); // eslint-disable-line

  return { events: localEvents, connected: ctx.connected, overflowed: ctx.overflowed };
}

// ─── useEventStreamHistory — ring-buffer for EventsPage (§ 18.2) ───────────────

export function useEventStreamHistory() {
  const ctx = React.useContext(EventBusContext);
  const [events, setEvents] = React.useState<SseEvent[]>([...eventBuffer]);

  React.useEffect(() => {
    // Prime with current buffer
    setEvents([...eventBuffer]);

    const listener: HistoryListener = (snapshot) => setEvents(snapshot);
    historyListeners.add(listener);
    return () => { historyListeners.delete(listener); };
  }, []);

  return { events, connected: ctx.connected, overflowed: ctx.overflowed };
}
