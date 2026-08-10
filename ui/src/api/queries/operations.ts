import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { StressConfig, StressStatus, StressReportSummary, StressReport, ProfileSnapshot } from '../types';

// ─── Stress ───────────────────────────────────────────────────────────────────

export const useStressStatus = () =>
  useQuery({
    queryKey: ['stress', 'status'],
    queryFn: () => api.get<StressStatus>('/api/stress/status'),
    staleTime: 3_000,
    refetchInterval: 5_000,
  });

export const useStressConfig = () =>
  useQuery({
    queryKey: ['stress', 'config'],
    queryFn: () => api.get<StressConfig>('/api/stress/config'),
    staleTime: 30_000,
  });

export const useSaveStressConfig = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: StressConfig) => api.put<StressConfig>('/api/stress/config', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['stress', 'config'] }),
  });
};

export const useStartStress = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body?: Partial<StressConfig>) =>
      api.post<StressStatus>('/api/stress/start', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['stress'] }),
  });
};

export const useStopStress = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.post<StressStatus>('/api/stress/stop'),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['stress'] }),
  });
};

export const useStressReports = () =>
  useQuery({
    queryKey: ['stress', 'reports'],
    queryFn: () => api.get<StressReportSummary[]>('/api/stress/reports'),
    staleTime: 30_000,
  });

export const useStressReport = (id: string) =>
  useQuery({
    queryKey: ['stress', 'reports', id],
    queryFn: () => api.get<StressReport>(`/api/stress/reports/${id}`),
    staleTime: 60_000,
    enabled: !!id,
  });

export const useDeleteStressReport = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: string) => api.delete(`/api/stress/reports/${id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['stress', 'reports'] }),
  });
};

// ─── Perf ─────────────────────────────────────────────────────────────────────

export const usePerfSnapshot = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'perf'],
    queryFn: () => api.get<ProfileSnapshot>(`/api/channels/${channelId}/perf`),
    staleTime: 2_000,
    refetchInterval: 3_000,
    enabled: channelId > 0,
  });

export const useStartPerf = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (mode: 'sampling' | 'instrumentation') =>
      api.post<ProfileSnapshot>(`/api/channels/${channelId}/perf/start`, { mode }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'perf'] }),
  });
};

export const useStopPerf = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.post<ProfileSnapshot>(`/api/channels/${channelId}/perf/stop`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'perf'] }),
  });
};

// ─── Health probes ────────────────────────────────────────────────────────────

export const useHealthz = () =>
  useQuery({
    queryKey: ['healthz'],
    queryFn: () => fetch('/healthz').then(r => ({ status: r.status, body: r.json() })),
    staleTime: 5_000,
    refetchInterval: 5_000,
  });

export const useReadyz = () =>
  useQuery({
    queryKey: ['readyz'],
    queryFn: () => fetch('/readyz').then(async r => ({ status: r.status, body: await r.json() })),
    staleTime: 5_000,
    refetchInterval: 5_000,
  });

export const useLivez = () =>
  useQuery({
    queryKey: ['livez'],
    queryFn: () => fetch('/livez').then(async r => ({ status: r.status, body: await r.json() })),
    staleTime: 5_000,
    refetchInterval: 5_000,
  });
