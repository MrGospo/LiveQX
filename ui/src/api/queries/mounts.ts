import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { MountPublic, MountSpec, MountTestResponse } from '../types';

const STALE = 30_000;

// fix41 — каталог mount'ов хранится в state/mounts.db; helper-status
// мержится фоновым sync'ом раз в 30s. Auto-refetch 30s даёт UI свежий
// active_state без ручного refresh, не задавливая backend.
export const useMounts = () =>
  useQuery({
    queryKey: ['mounts'],
    queryFn: () => api.get<{ mounts: MountPublic[] }>('/api/system/mounts')
      .then(r => r.mounts),
    staleTime: STALE,
    refetchInterval: 30_000,
  });

export const useMount = (id: number) =>
  useQuery({
    queryKey: ['mounts', id],
    queryFn: () => api.get<MountPublic>(`/api/system/mounts/${id}`),
    staleTime: STALE,
    enabled: id > 0,
  });

export const useCreateMount = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: MountSpec) =>
      api.post<MountPublic>('/api/system/mounts', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['mounts'] }),
  });
};

export const useUpdateMount = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ id, body }: { id: number; body: MountSpec }) =>
      api.put<MountPublic>(`/api/system/mounts/${id}`, body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['mounts'] }),
  });
};

export const useDeleteMount = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) =>
      api.delete<{ status: string; id: number }>(`/api/system/mounts/${id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['mounts'] }),
  });
};

// /api/system/mounts/test — pre-flight (БД не трогается). Используется
// в форме создания, чтобы оператор увидел «source/target/creds валидны»
// до commit'а.
export const useTestMount = () =>
  useMutation({
    mutationFn: (body: MountSpec) =>
      api.post<MountTestResponse>('/api/system/mounts/test', body),
  });

// /api/system/mounts/{id}/sync — единичный pull helper-status'а.
// Кнопка «Refresh» на строке списка после ручной починки на хосте.
export const useSyncMount = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) =>
      api.post<MountPublic>(`/api/system/mounts/${id}/sync`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['mounts'] }),
  });
};
