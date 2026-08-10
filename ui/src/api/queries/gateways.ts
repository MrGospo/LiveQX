import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { GatewayStatus, GatewayConfig } from '../types';

// ─── Gateways ─────────────────────────────────────────────────────────────────

export const useGateways = () =>
  useQuery({
    queryKey: ['gateways'],
    queryFn: () => api.get<GatewayStatus[]>('/api/gateways'),
    staleTime: 5_000,
    // fix33 D1 — gateway_state_change SSE drives invalidation, no polling.
  });

// fix40 UI-4 — opt-in live polling for the detail page. SSE fires only on
// `gateway_state_change`, so stat counters (pkt_in/bytes_in/fec.*) need
// periodic polling to feel "live". The list page leaves `live` off and
// relies on SSE invalidation alone.
export const useGateway = (id: number, opts?: { live?: boolean; intervalMs?: number }) =>
  useQuery({
    queryKey: ['gateways', id],
    queryFn: () => api.get<GatewayStatus>(`/api/gateways/${id}`),
    staleTime: 5_000,
    enabled: id > 0,
    refetchInterval: opts?.live ? (opts.intervalMs ?? 2_000) : false,
    refetchIntervalInBackground: false,
  });

export const useCreateGateway = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: GatewayConfig) => api.post<{ id: number }>('/api/gateways', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['gateways'] }),
  });
};

export const useDeleteGateway = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.delete<{ status: string; id: number }>(`/api/gateways/${id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['gateways'] }),
  });
};

export const usePatchGateway = (id: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<GatewayConfig>) =>
      api.patch<GatewayStatus>(`/api/gateways/${id}`, body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['gateways', id] }),
  });
};

export const usePlayGateway = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<{ status: string; id: number }>(`/api/gateways/${id}/play`),
    onSuccess: (_, id) => qc.invalidateQueries({ queryKey: ['gateways', id] }),
  });
};

export const useStopGateway = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<{ status: string; id: number }>(`/api/gateways/${id}/stop`),
    onSuccess: (_, id) => qc.invalidateQueries({ queryKey: ['gateways', id] }),
  });
};

