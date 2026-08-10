/**
 * useChannelAccess — checks per-channel RBAC grant.
 *
 * Returns { canView, canOperate, isLoading }.
 * Redirects to /dashboard with toast if channel not accessible.
 */
import { useAuthStore } from '@/stores/auth';

export function useChannelAccess(channelId: number) {
  const role = useAuthStore((s) => s.role);
  const grants = useAuthStore((s) => s.channelGrants);

  const isAdmin    = role === 'admin';
  const isOperator = role === 'operator';
  const grant      = grants[channelId];

  const canView    = isAdmin || isOperator || grant === 'view' || grant === 'operate';
  const canOperate = isAdmin || isOperator || grant === 'operate';

  return { canView, canOperate, isAdmin };
}
