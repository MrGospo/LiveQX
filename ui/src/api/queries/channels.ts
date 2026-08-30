import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { ChannelStatus, ChannelPermissionRow } from '../types';

const STALE_FAST = 5_000;
const STALE_CFG  = 60_000;

export const useChannels = () =>
  useQuery({
    queryKey: ['channels'],
    queryFn: () => api.get<ChannelStatus[]>('/api/channels'),
    staleTime: STALE_FAST,
    refetchInterval: 10_000,
  });

export const useChannel = (id: number) =>
  useQuery({
    queryKey: ['channels', id],
    queryFn: () => api.get<ChannelStatus>(`/api/channels/${id}`),
    staleTime: STALE_FAST,
    refetchInterval: 5_000,
    enabled: id > 0,
  });

export const useCreateChannel = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Record<string, unknown>) =>
      api.post<{ id: number }>('/api/channels', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels'] }),
  });
};

export const useUpdateChannelConfig = (id: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Record<string, unknown>) =>
      api.put<ChannelStatus>(`/api/channels/${id}/config`, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['channels', id] });
      qc.invalidateQueries({ queryKey: ['channels'] });
    },
  });
};

export const useDeleteChannel = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.delete<void>(`/api/channels/${id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels'] }),
  });
};

export const usePlayChannel = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<ChannelStatus>(`/api/channels/${id}/play`),
    onSuccess: (_, id) => {
      qc.invalidateQueries({ queryKey: ['channels', id] });
      qc.invalidateQueries({ queryKey: ['channels'] });
    },
  });
};

export const useStopChannel = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<ChannelStatus>(`/api/channels/${id}/stop`),
    onSuccess: (_, id) => {
      qc.invalidateQueries({ queryKey: ['channels', id] });
      qc.invalidateQueries({ queryKey: ['channels'] });
    },
  });
};

export const useRestartChannel = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<ChannelStatus>(`/api/channels/${id}/restart`),
    onSuccess: (_, id) => {
      qc.invalidateQueries({ queryKey: ['channels', id] });
      qc.invalidateQueries({ queryKey: ['channels'] });
    },
  });
};

export const useNextClip = () =>
  useMutation({
    mutationFn: (id: number) => api.post<{ ok: boolean }>(`/api/channels/${id}/next`),
  });

export const useChannelPermissions = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'permissions'],
    queryFn: () =>
      api.get<{ channel_id: number; items: ChannelPermissionRow[] }>(`/api/channels/${channelId}/permissions`)
        .then(r => r.items),
    staleTime: STALE_CFG,
    enabled: channelId > 0,
  });

// ─── Outputs ──────────────────────────────────────────────────────────────────

export const useOutputs = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'outputs'],
    queryFn: () =>
      api.get<{ channel_id: number; outputs: import('../types').OutputStatus[] }>(`/api/channels/${channelId}/outputs`)
        .then(r => r.outputs),
    staleTime: STALE_FAST,
    enabled: channelId > 0,
  });

export const useAddOutput = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Record<string, unknown>) =>
      api.post(`/api/channels/${channelId}/outputs`, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'outputs'] });
      qc.invalidateQueries({ queryKey: ['channels', channelId] });
    },
  });
};

export const usePatchOutput = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ outputId, body }: { outputId: string; body: Record<string, unknown> }) =>
      api.patch(`/api/channels/${channelId}/outputs/${outputId}`, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'outputs'] });
      qc.invalidateQueries({ queryKey: ['channels', channelId] });
    },
  });
};

export const useDeleteOutput = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (outputId: string) =>
      api.delete(`/api/channels/${channelId}/outputs/${outputId}`),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'outputs'] });
      qc.invalidateQueries({ queryKey: ['channels', channelId] });
    },
  });
};

export const useRestartOutput = (channelId: number) =>
  useMutation({
    mutationFn: (outputId: string) =>
      api.post(`/api/channels/${channelId}/outputs/${outputId}/restart`),
  });
