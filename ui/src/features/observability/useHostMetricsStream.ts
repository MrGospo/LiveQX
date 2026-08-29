/**
 * useHostMetricsStream — per-component SSE subscription to
 * /api/system/host_metrics/stream.
 *
 * Unlike the singleton EventBusProvider (always-on across the whole
 * session), this hook opens the connection when the consuming component
 * mounts and closes it on unmount. Mirrors the Windows Task Manager
 * model: no viewer → no work on the server (the /proc sampler on the
 * backend only runs while at least one subscriber is connected).
 */
import React from 'react';
import { runSseLoop } from '@/api/sseClient';
import { useAuthStore } from '@/stores/auth';

// Mirrors the JSON shape produced by HostMetricsReader::toJsonWithRates.
// Fields that require a delta (cpu.total_pct, per-core pct, nic bps,
// disk bps) are nullable — populated from the second frame onward.
export interface HostMetricsFrame {
  sampled_at_unix_ms: number;
  uptime_seconds:     number;
  load1:  number;
  load5:  number;
  load15: number;
  cpu: {
    total_pct: number | null;
    aggregate: { user: number; nice: number; system: number; idle: number;
                 iowait: number; irq: number; softirq: number; steal: number } | null;
    per_core: Array<{
      name: string;
      user: number; nice: number; system: number; idle: number;
      iowait: number; irq: number; softirq: number; steal: number;
      pct?: number;
    }>;
  };
  mem: {
    total_bytes:      number;
    free_bytes:       number;
    available_bytes:  number;
    buffers_bytes:    number;
    cached_bytes:     number;
    used_bytes:       number;
    swap_total_bytes: number;
    swap_free_bytes:  number;
    swap_used_bytes:  number;
  };
  nics: Array<{
    name:       string;
    is_up:      boolean;
    rx_bytes:   number;
    tx_bytes:   number;
    rx_packets: number;
    tx_packets: number;
    rx_errs:    number;
    tx_errs:    number;
    rx_drop:    number;
    tx_drop:    number;
    rx_bps:     number | null;
    tx_bps:     number | null;
  }>;
  fs: Array<{
    mount:       string;
    fstype:      string;
    total_bytes: number;
    free_bytes:  number;
    used_bytes:  number;
  }>;
  disks: Array<{
    name:             string;
    reads_completed:  number;
    writes_completed: number;
    sectors_read:     number;
    sectors_written:  number;
    read_bps:         number | null;
    write_bps:        number | null;
  }>;
}

export interface UseHostMetricsResult {
  data:      HostMetricsFrame | null;
  connected: boolean;
}

export function useHostMetricsStream(): UseHostMetricsResult {
  const token = useAuthStore(s => s.accessToken);
  const [data,      setData]      = React.useState<HostMetricsFrame | null>(null);
  const [connected, setConnected] = React.useState(false);

  React.useEffect(() => {
    if (!token) return;
    const ac = new AbortController();

    runSseLoop({
      url:      new URL('/api/system/host_metrics/stream', window.location.origin).toString(),
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
        useAuthStore.getState().setTokens(j.access_token, j.refresh_token, j.expires_in);
      },
      onAuthFailed: () => { useAuthStore.getState().logout(); },

      onOpen:       () => setConnected(true),
      onDisconnect: () => setConnected(false),

      onMessage: (msg) => {
        try {
          setData(JSON.parse(msg.data) as HostMetricsFrame);
        } catch {
          // malformed frame — keep the previous snapshot, wait for next tick
        }
      },
    });

    return () => { ac.abort(); setConnected(false); };
  }, [token]);

  return { data, connected };
}
