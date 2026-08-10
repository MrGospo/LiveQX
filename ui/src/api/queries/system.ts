import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { VersionInfo, SystemStatus, NetworkInterface, GpuInfo } from '../types';

export interface MetricsTokenStatus {
  configured: boolean;
  preview?: string;   // first 8 chars + "…", only when configured
}

export interface MetricsTokenResult {
  token: string;
  configured: boolean;
}

export interface BrowseEntry {
  name:        string;
  full_path:   string;
  is_dir:      boolean;
  size_bytes?: number;
}

export interface BrowseResponse {
  path:    string;
  parent:  string | null;
  entries: BrowseEntry[];
}

export const useVersion = () =>
  useQuery({
    queryKey: ['version'],
    queryFn: () => api.get<VersionInfo>('/api/version', { noAuth: true }),
    staleTime: 5 * 60_000,
  });

export const useSystemStatus = () =>
  useQuery({
    queryKey: ['status'],
    queryFn: () => api.get<SystemStatus>('/api/status'),
    staleTime: 5_000,
    refetchInterval: 10_000,
  });

export const useNetworkInterfaces = () =>
  useQuery({
    queryKey: ['system', 'interfaces'],
    queryFn: () => api.get<NetworkInterface[]>('/api/system/interfaces'),
    staleTime: 60_000,
  });

export const useGpuInfo = () =>
  useQuery({
    queryKey: ['system', 'gpu'],
    queryFn: () => api.get<GpuInfo>('/api/system/gpu'),
    staleTime: 60_000,
  });

export const useMetricsTokenStatus = () =>
  useQuery({
    queryKey: ['system', 'metrics-token'],
    queryFn: () => api.get<MetricsTokenStatus>('/api/metrics/token'),
    staleTime: 30_000,
  });

export const useGenerateMetricsToken = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.post<MetricsTokenResult>('/api/metrics/token', { generate: true }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['system', 'metrics-token'] }),
  });
};

export const useDeleteMetricsToken = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.delete<MetricsTokenStatus>('/api/metrics/token'),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['system', 'metrics-token'] }),
  });
};

export const useBrowseFolder = (path: string, includeFiles = false, enabled = true) =>
  useQuery({
    queryKey: ['system', 'browse', path, includeFiles],
    queryFn: () => {
      const q = new URLSearchParams({ path });
      if (includeFiles) q.set('include_files', 'true');
      return api.get<BrowseResponse>(`/api/system/browse?${q.toString()}`);
    },
    enabled,
    staleTime: 10_000,
  });

// ─── System Time (fix33) ─────────────────────────────────────────────────────
// /api/system/time GET — read effective config + runtime snapshot
// /api/system/time PUT — merge-patch (RFC 7396)
// /api/system/time/test POST — SNTP probe (admin-only, does not mutate state)

export type TimeSourceKind = 'ntp' | 'system_local' | 'manual';

export interface NtpConfig {
  enabled: boolean;
  servers: string[];
  poll_interval_s: number;
  last_offset_ms: number | null;
  last_sync_at: number | null;
}

export interface ManualConfig {
  offset_ms: number;
  set_at: number | null;
}

export interface TimeConfig {
  source: TimeSourceKind;
  server_timezone: string;
  ntp: NtpConfig;
  manual: ManualConfig;
}

export interface TimeRuntime {
  source: string;
  offset_ms: number;
  effective_now: number;
  system_now: number;
  server_timezone: string;
}

export interface TimeConfigResponse {
  config: TimeConfig;
  runtime: TimeRuntime;
}

export interface TimeConfigPatch {
  source?: TimeSourceKind;
  server_timezone?: string;
  ntp?: Partial<Pick<NtpConfig, 'enabled' | 'servers' | 'poll_interval_s'>>;
  manual?: { offset_ms?: number; epoch_unix_ms?: number };
}

export interface TimeProbeResult {
  server: string;
  ok: boolean;
  offset_ms?: number;
  round_trip_ms?: number;
  server_unix_ms?: number;
  error?: string;
}

export interface TimeProbeResponse {
  results: TimeProbeResult[];
}

export const useTimeConfig = () =>
  useQuery({
    queryKey: ['system', 'time'],
    queryFn: () => api.get<TimeConfigResponse>('/api/system/time'),
    staleTime: 30_000,
    refetchInterval: 30_000,
  });

export const useSaveTimeConfig = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: TimeConfigPatch) =>
      api.put<{ config: TimeConfig }>('/api/system/time', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['system', 'time'] }),
  });
};

export const useTestNtp = () =>
  useMutation({
    mutationFn: (body: { servers?: string[]; timeout_ms?: number } = {}) =>
      api.post<TimeProbeResponse>('/api/system/time/test', body),
  });
