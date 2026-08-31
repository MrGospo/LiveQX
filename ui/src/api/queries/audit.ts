import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api } from '../client';
import type {
  AuditTrailFilter, AuditTrailListResponse,
  AuditTrailVerify, AuditTrailCategory, AuditTrailStats,
} from '../types';

// Enterprise audit trail (state/audit.db). Distinct from /api/auth/audit —
// that endpoint reads the legacy auth-only auth_audit table. This one
// covers every server mutation (channels/outputs/gateways/plugins/mounts/
// system) plus auth events mirrored from AuthService.

function buildQs(f: AuditTrailFilter): string {
  const qs = new URLSearchParams();
  Object.entries(f).forEach(([k, v]) => {
    if (v === undefined || v === null || v === '') return;
    qs.set(k, String(v));
  });
  return qs.toString();
}

export const useAuditTrail = (filter: AuditTrailFilter) =>
  useQuery({
    queryKey: ['audit-trail', filter],
    queryFn:  () =>
      api.get<AuditTrailListResponse>(`/api/audit/events?${buildQs(filter)}`),
    staleTime: 5_000,
    placeholderData: (prev) => prev,
  });

export const useAuditTrailVerify = (opts?: { enabled?: boolean }) =>
  useQuery({
    queryKey: ['audit-trail', 'verify'],
    queryFn:  () => api.get<AuditTrailVerify>('/api/audit/verify'),
    enabled:  opts?.enabled ?? false,
    staleTime: 30_000,
  });

export const useAuditTrailCategories = () =>
  useQuery({
    queryKey: ['audit-trail', 'categories'],
    queryFn:  () =>
      api.get<{ categories: AuditTrailCategory[] }>('/api/audit/categories')
         .then(r => r.categories),
    staleTime: 5 * 60_000,
  });

export const useAuditTrailStats = (opts?: { refetchInterval?: number }) =>
  useQuery({
    queryKey: ['audit-trail', 'stats'],
    queryFn:  () => api.get<AuditTrailStats>('/api/audit/stats'),
    refetchInterval: opts?.refetchInterval,
    staleTime: 5_000,
  });

// Manual re-verify trigger. Chain verification is a full-table scan —
// keep it explicit rather than auto-refetching.
export const useAuditTrailReverify = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.get<AuditTrailVerify>('/api/audit/verify'),
    onSuccess:  (data) =>
      qc.setQueryData(['audit-trail', 'verify'], data),
  });
};
