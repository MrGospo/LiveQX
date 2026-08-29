/**
 * useClipChangeInvalidation — per-LogTab SSE subscription for clip_change.
 *
 * The singleton EventBusProvider connects without ?types= and therefore
 * uses the backend's default subscription set, from which clip_change is
 * excluded (SseFilter::sseEventInDefaultSubscription). That keeps loud
 * per-clip traffic out of the general /observability/events page.
 *
 * The channel Log tab still wants live updates. This hook opens a
 * dedicated stream with ?types=clip_change while the tab is mounted and
 * invalidates the playback-log query for the given channel whenever an
 * event for it arrives. Backend then serves fresh rows from the sink on
 * the next fetch — no polling, no client-side reconciliation logic.
 */
import React from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { runSseLoop } from '@/api/sseClient';
import { useAuthStore } from '@/stores/auth';
import type { SseEvent } from '@/api/types';

export function useClipChangeInvalidation(channelId: number) {
  const qc    = useQueryClient();
  const token = useAuthStore(s => s.accessToken);

  React.useEffect(() => {
    if (!token || channelId <= 0) return;
    const ac = new AbortController();

    runSseLoop({
      url: new URL('/api/events/stream?types=clip_change',
                   window.location.origin).toString(),
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
        const j = await res.json();
        useAuthStore.getState().setTokens(
          j.access_token, j.refresh_token, j.expires_in);
      },
      onAuthFailed: () => { useAuthStore.getState().logout(); },

      onMessage: (msg) => {
        if (msg.event === 'error') return;
        let ev: SseEvent;
        try { ev = JSON.parse(msg.data) as SseEvent; } catch { return; }
        if (ev.type !== 'clip_change') return;
        if (ev.channel_id !== channelId) return;
        qc.invalidateQueries({ queryKey: ['channels', channelId, 'playback-log'] });
        qc.invalidateQueries({ queryKey: ['channels', channelId, 'playback-log', 'status'] });
      },
    });

    return () => { ac.abort(); };
  }, [token, channelId, qc]);
}
