import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
// ScheduleUpcomingItem type used in useScheduleUpcoming below
declare module '../types' {
  interface ScheduleUpcomingItem {
    entry_id: string;
    starts_at: number;
    ends_at?: number;
    priority?: number;
  }
}
import { api } from '../client';
import type { Playlist, Schedule, ScheduleActive, WatcherStatus } from '../types';

// Backend-truth shapes (OpenAPI mismatch — real responses match the C++ sinks).
// File sink:   sink_type='file', + files_count, playback_dir
// SQLite sink: sink_type='db',   + channels_count, files_count, schema_errors,
//                                  schema_version, retention_days
// None sink:   sink_type='none'
export interface PlaybackLogStatusResponse {
  sink_type: 'none' | 'file' | 'db';
  queue_depth?: number;
  dropped_count?: number;
  last_write_ns?: number | null;
  files_count?: number;
  playback_dir?: string;
  channels_count?: number;
  schema_errors?: number;
  schema_version?: number;
  retention_days?: number;
}

export interface PlaybackLogEvent {
  channel_id: number;
  clip_path: string;
  clip_type: 'image' | 'video' | string;
  started_at_ns: number;
  ended_at_ns: number;
  played_sec: number;
  transition_type: string;
  status: 'completed' | 'skipped_user' | 'removed' | 'error' | string;
  error_reason?: string;
}

export interface PlaybackLogResponse {
  events: PlaybackLogEvent[];
  next_after_ns: number | null;
}

// ─── Playlist ─────────────────────────────────────────────────────────────────

export const usePlaylist = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'playlist'],
    queryFn: () => api.get<Playlist>(`/api/channels/${channelId}/playlist`),
    staleTime: 10_000,
    enabled: channelId > 0,
  });

export const useReplacePlaylist = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (playlist: Playlist) =>
      api.post<Playlist>(`/api/channels/${channelId}/playlist`, playlist),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'playlist'] }),
  });
};

export const useAppendPlaylist = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (items: Playlist) =>
      api.post<{ first_idx: number; playlist: Playlist }>(`/api/channels/${channelId}/playlist/append`, items),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'playlist'] }),
  });
};

export const useDeletePlaylistItem = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (idx: number) =>
      api.delete<{ was_active: boolean; playlist: Playlist }>(`/api/channels/${channelId}/playlist/${idx}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'playlist'] }),
  });
};

export const useClearPlaylist = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.delete<void>(`/api/channels/${channelId}/playlist`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'playlist'] }),
  });
};

// ─── Schedule ─────────────────────────────────────────────────────────────────
// Q4.1: upcoming entries
export const useScheduleUpcoming = (channelId: number, withinSec = 86400) =>
  useQuery({
    queryKey: ['channels', channelId, 'schedule', 'upcoming', withinSec],
    queryFn: () =>
      api.get<import('../types').ScheduleUpcomingItem[]>(
        `/api/channels/${channelId}/schedule/upcoming?within_sec=${withinSec}`,
      ),
    staleTime: 30_000,
    refetchInterval: 30_000,
    enabled: channelId > 0,
  });

// Q4.2: notify-deleted
export const useNotifyDeleted = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (paths: string[]) =>
      api.post(`/api/channels/${channelId}/playlist/notify-deleted`, { paths }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'playlist'] }),
  });
};

export const useSchedule = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'schedule'],
    queryFn: () => api.get<Schedule>(`/api/channels/${channelId}/schedule`),
    staleTime: 15_000,
    enabled: channelId > 0,
  });

export const useReplaceSchedule = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (schedule: Schedule) =>
      api.put<Schedule>(`/api/channels/${channelId}/schedule`, schedule),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['channels', channelId, 'schedule'] }),
  });
};

export const useScheduleActive = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'schedule', 'active'],
    queryFn: () => api.get<ScheduleActive>(`/api/channels/${channelId}/schedule/active`),
    staleTime: 5_000,
    refetchInterval: 5_000,
    enabled: channelId > 0,
  });

// ─── Watcher ──────────────────────────────────────────────────────────────────

export const useWatcherStatus = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'watcher'],
    queryFn: () => api.get<WatcherStatus>(`/api/channels/${channelId}/watcher/status`),
    staleTime: 5_000,
    refetchInterval: 5_000,
    retry: false,
    enabled: channelId > 0,
  });

export const useRescan = (channelId: number) =>
  useMutation({
    mutationFn: () => api.post<void>(`/api/channels/${channelId}/watcher/rescan`),
  });

// ─── Playback Log ──────────────────────────────────────────────────────────────

export const usePlaybackLogStatus = (channelId: number) =>
  useQuery({
    queryKey: ['channels', channelId, 'playback-log', 'status'],
    queryFn: () => api.get<PlaybackLogStatusResponse>(`/api/channels/${channelId}/playback-log/status`),
    staleTime: 10_000,
    enabled: channelId > 0,
  });

export const usePlaybackLog = (
  channelId: number,
  params: {
    limit?: number;
    offset?: number;
    from_ns?: number;
    to_ns?: number;
    after_ns?: number;
  },
) =>
  useQuery({
    queryKey: ['channels', channelId, 'playback-log', params],
    queryFn: () => {
      const qs = new URLSearchParams();
      Object.entries(params).forEach(([k, v]) => v !== undefined && qs.set(k, String(v)));
      return api.get<PlaybackLogResponse>(
        `/api/channels/${channelId}/playback-log?${qs}`,
      );
    },
    staleTime: 10_000,
    enabled: channelId > 0,
  });

export interface PlaybackLogPurgeResponse {
  deleted_rows: number;
  removed_files: number;
}

// DELETE /api/channels/{id}/playback-log?from_ns=..&to_ns=..
// Оба параметра опциональны: без них вычищается весь лог канала. Закрытый
// интервал [from_ns, to_ns] совпадает с семантикой query().
export const usePurgePlaybackLog = (channelId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (params: { from_ns?: number; to_ns?: number }) => {
      const qs = new URLSearchParams();
      if (params.from_ns !== undefined) qs.set('from_ns', String(params.from_ns));
      if (params.to_ns !== undefined)   qs.set('to_ns',   String(params.to_ns));
      const suffix = qs.toString() ? `?${qs}` : '';
      return api.delete<PlaybackLogPurgeResponse>(
        `/api/channels/${channelId}/playback-log${suffix}`,
      );
    },
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'playback-log'] });
    },
  });
};

// ─── Channel raw log file (channel.log) ────────────────────────────────────────

// GET /api/channels/{id}/logs?tail=N — last N lines of the raw spdlog file.
// Admin-only on the backend; hook stays enabled regardless so a 403 surfaces
// naturally through the api client and the caller renders an empty state.
export interface ChannelLogTailResponse {
  lines: string[];
  truncated: boolean;
  size_bytes: number;
}

export const useChannelLogTail = (channelId: number, tail = 500) =>
  useQuery({
    queryKey: ['channels', channelId, 'log-file', 'tail', tail],
    queryFn: () =>
      api.get<ChannelLogTailResponse>(
        `/api/channels/${channelId}/logs?tail=${tail}`,
      ),
    staleTime: 5_000,
    enabled: channelId > 0,
  });
