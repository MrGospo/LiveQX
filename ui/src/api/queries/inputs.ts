// Q5: per-channel live-status via GET /api/channels/{id}/live-status
// Do NOT parse current_clip.name — use this endpoint instead.
import { useQuery } from '@tanstack/react-query';
import { api } from '../client';
import type { LiveInputStatus } from '../types';

export interface LiveStatusResponse {
  channel_id: number;
  live_inputs: LiveInputStatus[];
}

export const useLiveStatus = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'live-status'],
    queryFn: () =>
      api.get<LiveStatusResponse>(`/api/channels/${channelId}/live-status`),
    staleTime: 5_000,
    refetchInterval: 5_000,
    enabled: channelId > 0,
  });
