import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type { PluginListing } from '../types';

export const usePlugins = () =>
  useQuery({
    queryKey: ['plugins'],
    queryFn: () =>
      api.get<{ items: PluginListing[] }>('/api/plugins').then(r => r.items),
    staleTime: 30_000,
  });

export const usePlugin = (name: string) =>
  useQuery({
    queryKey: ['plugins', name],
    queryFn: () => api.get<PluginListing>(`/api/plugins/${name}`),
    staleTime: 30_000,
    enabled: !!name,
  });

export const useInstallPlugin = (name: string) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({
      file,
      force,
      iUnderstand,
    }: {
      file: File;
      force?: boolean;
      iUnderstand?: boolean;
    }) => {
      const form = new FormData();
      form.append('plugin', file);
      const qs = new URLSearchParams();
      if (force) qs.set('force', 'true');
      if (iUnderstand) qs.set('i_understand', 'true');
      return fetch(`/api/plugins/${name}/install?${qs}`, {
        method: 'POST',
        body: form,
        headers: { Authorization: `Bearer ${localStorage.getItem('access_token') ?? ''}` },
      }).then(r => r.json());
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: ['plugins'] }),
  });
};

export const useUninstallPlugin = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (name: string) =>
      api.delete<{ status: string; name: string }>(`/api/plugins/${name}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['plugins'] }),
  });
};

export const useAcceptEula = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (name: string) =>
      api.post<{ status: string; name: string }>(`/api/plugins/${name}/eula/accept`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['plugins'] }),
  });
};
