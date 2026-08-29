/**
 * useChannelLogStream — SSE tail-F of {channel_dir}/logs/channel.log.
 *
 * The backend endpoint /api/channels/{id}/logs/stream emits one
 * `data: {"line":"..."}` frame per appended line, and a
 * `data: {"rotated":true}` frame when the file was truncated (spdlog
 * rotation). We accumulate lines in a bounded ring so the UI never
 * balloons for a long-lived tab.
 *
 * Admin-only on the backend — a non-admin subscriber sees the stream
 * fail (403) through runSseLoop and just gets an empty buffer.
 */
import React from 'react';
import { runSseLoop } from '@/api/sseClient';
import { useAuthStore } from '@/stores/auth';

const MAX_LINES = 2000;

export interface ChannelLogStreamState {
  lines:     string[];
  connected: boolean;
  rotatedAt: number | null;
}

export function useChannelLogStream(
  channelId: number,
  enabled: boolean,
): ChannelLogStreamState {
  const token = useAuthStore(s => s.accessToken);
  const [lines, setLines]         = React.useState<string[]>([]);
  const [connected, setConnected] = React.useState(false);
  const [rotatedAt, setRotatedAt] = React.useState<number | null>(null);

  React.useEffect(() => {
    if (!enabled || !token || channelId <= 0) {
      setConnected(false);
      return;
    }
    const ac = new AbortController();

    runSseLoop({
      url: new URL(`/api/channels/${channelId}/logs/stream`,
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
      onOpen:       () => setConnected(true),
      onDisconnect: () => setConnected(false),

      onMessage: (msg) => {
        let payload: { line?: string; rotated?: boolean };
        try { payload = JSON.parse(msg.data); } catch { return; }
        if (payload.rotated === true) {
          setLines([]);
          setRotatedAt(Date.now());
          return;
        }
        if (typeof payload.line !== 'string') return;
        const line = payload.line;
        setLines(prev => {
          const next = prev.length >= MAX_LINES
            ? prev.slice(prev.length - MAX_LINES + 1)
            : prev.slice();
          next.push(line);
          return next;
        });
      },
    });

    return () => {
      ac.abort();
      setConnected(false);
    };
  }, [enabled, token, channelId]);

  return { lines, connected, rotatedAt };
}
