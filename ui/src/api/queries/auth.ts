import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type {
  LoginResponse, TokenPair, User, UserDetail,
  ChannelPermissionRow, AuditEvent, OwnSession, MasterKeyInfo,
  LdapConfig, LdapConfigRequest, LdapTestResponse,
  SmtpConfig, SmtpConfigRequest, SmtpTestResponse,
} from '../types';

// ─── Current user ───────────────────────────────────────────────────────────
// GET /api/auth/me — возвращает текущего юзера (по JWT). Profile-странице
// нужны email/last_login_at/last_login_ip, которые viewer/operator через
// /api/auth/users/{id} (Admin-only) получить не могут.
export const useMe = () =>
  useQuery({
    queryKey: ['auth', 'me'],
    queryFn: () => api.get<User>('/api/auth/me'),
    staleTime: 60_000,
  });

// ─── Own sessions (fix32 B2) ────────────────────────────────────────────────
// GET /api/auth/me/sessions — список активных сессий текущего юзера.
// Backend фильтрует по user_id + revoked_at IS NULL + expires_at > now.

export const useOwnSessions = () =>
  useQuery({
    queryKey: ['auth', 'me', 'sessions'],
    queryFn: () =>
      api.get<{ items: OwnSession[] }>('/api/auth/me/sessions').then(r => r.items),
    staleTime: 30_000,
  });

// DELETE /api/auth/me/sessions/{jwt_id}. Возвращает {status, jwt_id, current}.
// UI делает logout если current=true (текущая сессия ревокнута).
export const useRevokeOwnSession = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (jwt_id: string) =>
      api.delete<{ status: 'revoked'; jwt_id: string; current: boolean }>(
        `/api/auth/me/sessions/${encodeURIComponent(jwt_id)}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['auth', 'me', 'sessions'] }),
  });
};

// ─── Master-key info (fix32 B3) ─────────────────────────────────────────────
// GET /api/auth/master-key/info — admin only. UI показывает fingerprint,
// algorithm, source, created_at, last_rotated_at в Settings/MasterKey page.
export const useMasterKeyInfo = () =>
  useQuery({
    queryKey: ['auth', 'master-key', 'info'],
    queryFn: () => api.get<MasterKeyInfo>('/api/auth/master-key/info'),
    staleTime: 60_000,
  });



export const useLogin = () =>
  useMutation({
    mutationFn: (creds: { username: string; password: string }) =>
      api.post<LoginResponse>('/api/auth/login', creds),
  });

export const useRefresh = () =>
  useMutation({
    mutationFn: (refresh_token: string) =>
      api.post<TokenPair>('/api/auth/refresh', { refresh_token }),
  });

export const useLogout = () =>
  useMutation({ mutationFn: () => api.post<void>('/api/auth/logout') });

export const useChangePassword = () =>
  useMutation({
    mutationFn: (body: { current_password: string; new_password: string }) =>
      api.post<{ status: string }>('/api/auth/me/password', body),
  });

// ─── Users ────────────────────────────────────────────────────────────────────

export const useUsers = () =>
  useQuery({
    queryKey: ['users'],
    queryFn: () => api.get<{ users: User[] }>('/api/auth/users').then(r => r.users),
    staleTime: 60_000,
  });

export const useUser = (id: number) =>
  useQuery({
    queryKey: ['users', id],
    queryFn: () => api.get<UserDetail>(`/api/auth/users/${id}`),
    staleTime: 30_000,
  });

export const useCreateUser = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: Partial<User> & { password?: string }) =>
      api.post<User & { initial_password?: string }>('/api/auth/users', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['users'] }),
  });
};

export const useUpdateUser = (id: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { email?: string; role?: string; source?: string }) =>
      api.put<User>(`/api/auth/users/${id}`, body),
    onSuccess: () => { qc.invalidateQueries({ queryKey: ['users'] }); qc.invalidateQueries({ queryKey: ['users', id] }); },
  });
};

export const useDeleteUser = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.delete<{ status: string; id: number }>(`/api/auth/users/${id}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['users'] }),
  });
};

// POST /api/auth/users/{id}/purge — hard-delete (GDPR right-to-be-forgotten).
// Audit trail сохраняется через snapshot username в payload "admin.user.purged".
// 409 cannot_delete_self — нельзя удалить себя; 409 last_admin — последний admin.
export const usePurgeUser = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) =>
      api.post<{ status: 'purged'; id: number }>(`/api/auth/users/${id}/purge`, {}),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['users'] }),
  });
};

export const useEnableUser = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<{ status: string; id: number }>(`/api/auth/users/${id}/enable`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['users'] }),
  });
};

export const useUnlockUser = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (id: number) => api.post<{ status: string; id: number }>(`/api/auth/users/${id}/unlock`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['users'] }),
  });
};

export const useResetPassword = () =>
  useMutation({
    mutationFn: (id: number) =>
      api.post<{ status: string; id: number; initial_password: string }>(`/api/auth/users/${id}/reset-password`),
  });

// ─── Channel grants ──────────────────────────────────────────────────────────

export const useUserChannelGrants = (userId: number) =>
  useQuery({
    queryKey: ['users', userId, 'channels'],
    queryFn: () =>
      api.get<{ items: ChannelPermissionRow[] }>(`/api/auth/users/${userId}/channels`)
        .then(r => r.items),
    staleTime: 30_000,
  });

export const useSetChannelGrant = (userId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: ({ channelId, permission }: { channelId: number; permission: string }) =>
      api.put(`/api/auth/users/${userId}/channels/${channelId}`, { permission }),
    onSuccess: (_, { channelId }) => {
      qc.invalidateQueries({ queryKey: ['users', userId, 'channels'] });
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'permissions'] });
    },
  });
};

export const useRemoveChannelGrant = (userId: number) => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (channelId: number) =>
      api.delete(`/api/auth/users/${userId}/channels/${channelId}`),
    onSuccess: (_, channelId) => {
      qc.invalidateQueries({ queryKey: ['users', userId, 'channels'] });
      qc.invalidateQueries({ queryKey: ['channels', channelId, 'permissions'] });
    },
  });
};

// ─── Audit ───────────────────────────────────────────────────────────────────

export const useAuditEvents = (params: {
  from_ts?: number; to_ts?: number;
  user_id?: number; username?: string;
  event?: string; limit?: number; offset?: number;
}) =>
  useQuery({
    queryKey: ['audit', params],
    queryFn: () => {
      const qs = new URLSearchParams();
      Object.entries(params).forEach(([k, v]) => v !== undefined && qs.set(k, String(v)));
      return api.get<{ events: AuditEvent[] }>(`/api/auth/audit?${qs}`).then(r => r.events);
    },
    staleTime: 10_000,
  });

export const usePurgeAudit = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (older_than_days: number) =>
      api.post<{ removed: number; older_than_days: number }>(`/api/auth/audit/purge?older_than_days=${older_than_days}`),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['audit'] }),
  });
};

// ─── LDAP ─────────────────────────────────────────────────────────────────────

export const useLdapConfig = () =>
  useQuery({
    queryKey: ['ldap', 'config'],
    queryFn: () => api.get<LdapConfig>('/api/auth/ldap/config'),
    staleTime: 60_000,
  });

export const useSaveLdapConfig = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: LdapConfigRequest) => api.put<LdapConfig>('/api/auth/ldap/config', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['ldap'] }),
  });
};

export const useTestLdap = () =>
  useMutation({
    mutationFn: (body?: Record<string, unknown>) =>
      api.post<LdapTestResponse>('/api/auth/ldap/test', body),
  });

// ─── SMTP ─────────────────────────────────────────────────────────────────────

export const useSmtpConfig = () =>
  useQuery({
    queryKey: ['smtp', 'config'],
    queryFn: () => api.get<SmtpConfig>('/api/auth/smtp/config'),
    staleTime: 60_000,
  });

export const useSaveSmtpConfig = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: SmtpConfigRequest) => api.put<SmtpConfig>('/api/auth/smtp/config', body),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['smtp'] }),
  });
};

export const useTestSmtp = () =>
  useMutation({
    mutationFn: (body: { to: string; subject?: string; body?: string; use_saved?: boolean }) =>
      api.post<SmtpTestResponse>('/api/auth/smtp/test', body),
  });
