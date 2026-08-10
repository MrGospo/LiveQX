import React from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { useUsers, useDeleteUser, useEnableUser, useUnlockUser, usePurgeUser, useResetPassword } from '@/api/queries/auth';
import { RoleBadge, SourceBadge } from '@/components/HealthBadge';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { PurgeUserModal } from '@/components/PurgeUserModal';
import { ConfirmModal } from '@/components/ConfirmModal';
import { InitialPasswordModal } from '@/components/InitialPasswordModal';
import { RowActionsMenu, type RowAction } from '@/components/RowActionsMenu';
import { SETTINGS_NAV } from './nav';
import { fmtTime } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import { Users, Plus, Search, Pencil, Power, KeyRound, Trash2, Unlock } from 'lucide-react';
import type { User } from '@/api/types';

export default function UsersPage() {
  const { t }    = useTranslation();
  const navigate = useNavigate();
  const toast    = useToast();
  const [search, setSearch] = React.useState('');
  const [roleFilter, setRoleFilter] = React.useState('any');

  const { data: users = [], isLoading } = useUsers();
  const { mutate: deleteUser  } = useDeleteUser();
  const { mutate: enableUser  } = useEnableUser();
  const { mutate: unlockUser  } = useUnlockUser();
  const { mutate: purgeUser, isPending: purgePending } = usePurgeUser();
  const { mutateAsync: resetPasswordAsync, isPending: resetPending } = useResetPassword();

  const [purgeTarget,  setPurgeTarget]  = React.useState<User | null>(null);
  const [resetTarget,  setResetTarget]  = React.useState<User | null>(null);
  const [issuedPwd,    setIssuedPwd]    = React.useState<{ user: User; password: string } | null>(null);

  const handlePurgeConfirm = () => {
    if (!purgeTarget) return;
    const target = purgeTarget;
    purgeUser(target.id, {
      onSuccess: () => {
        toast(t('users.userPurged', { user: target.username }), 'success');
        setPurgeTarget(null);
      },
      onError: (err: any) => {
        const code = err?.body?.error;
        if (code === 'cannot_delete_self') toast(t('users.cannotDeleteSelf'), 'danger');
        else if (code === 'last_admin')    toast(t('users.lastAdminError'), 'danger');
        else                                toast(err?.message ?? String(err), 'danger');
        setPurgeTarget(null);
      },
    });
  };

  const handleResetConfirm = async () => {
    if (!resetTarget) return;
    const target = resetTarget;
    try {
      const r = await resetPasswordAsync(target.id);
      setResetTarget(null);
      setIssuedPwd({ user: target, password: r.initial_password });
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
      setResetTarget(null);
    }
  };

  const filtered = users.filter(u => {
    if (search && !u.username.includes(search) && !(u.email ?? '').includes(search)) return false;
    if (roleFilter !== 'any' && u.role !== roleFilter) return false;
    return true;
  });

  const buildActions = (u: User): RowAction[] => {
    const locked = !!(u.locked_until && u.locked_until > Date.now() / 1000);
    return [
      { key: 'edit',
        label: t('users.edit'),
        icon: <Pencil size={14} />,
        onClick: () => navigate(`/settings/users/${u.id}`) },
      u.disabled
        ? { key: 'enable',
            label: t('users.enable'),
            icon: <Power size={14} />,
            onClick: () => enableUser(u.id, {
              onSuccess: () => toast(t('users.userEnabled', { user: u.username }), 'success'),
            }) }
        : { key: 'disable',
            label: t('users.disable'),
            icon: <Power size={14} />,
            onClick: () => deleteUser(u.id, {
              onSuccess: () => toast(t('users.userDisabled', { user: u.username }), 'warning'),
            }) },
      ...(locked ? [{
        key: 'unlock',
        label: t('users.unlock'),
        icon: <Unlock size={14} />,
        onClick: () => unlockUser(u.id, {
          onSuccess: () => toast(t('users.userUnlocked', { user: u.username }), 'success'),
        }),
      } as RowAction] : []),
      { key: 'reset',
        label: t('users.resetPassword'),
        icon: <KeyRound size={14} />,
        onClick: () => setResetTarget(u) },
      { key: 'purge',
        label: t('users.purge'),
        icon: <Trash2 size={14} />,
        danger: true,
        onClick: () => setPurgeTarget(u) },
    ];
  };

  const getUserStatus = (u: User): { label: string; color: string } => {
    if (u.disabled) return { label: t('users.status.disabled'), color: 'text-[var(--text-muted)]' };
    if (u.locked_until && u.locked_until > Date.now() / 1000) return { label: t('users.status.locked'), color: 'text-[var(--warning)]' };
    if (u.must_change_password) return { label: t('users.status.mustChangePw'), color: 'text-[var(--info)]' };
    return { label: t('users.status.enabled'), color: 'text-[var(--success)]' };
  };

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />
      <div className="flex items-center justify-end">
        <button onClick={() => navigate('/settings/users/new')}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
          <Plus size={14} /> {t('users.create')}
        </button>
      </div>

      {/* Filters */}
      <div className="flex gap-3 flex-wrap">
        <div className="relative">
          <Search size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-[var(--text-muted)]" />
          <input value={search} onChange={e => setSearch(e.target.value)}
            placeholder={t('users.searchPlaceholder')}
            className="pl-8 pr-3 py-2 bg-canvas border border-[var(--border-subtle)] rounded-md text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] w-56" />
        </div>
        <select value={roleFilter} onChange={e => setRoleFilter(e.target.value)}
          className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
          <option value="any">{t('users.roleAny')}</option>
          <option value="admin">{t('users.roleAdmin')}</option>
          <option value="operator">{t('users.roleOperator')}</option>
          <option value="viewer">{t('users.roleViewer')}</option>
        </select>
      </div>

      <Block padding="p-0">
        {isLoading ? (
          <div className="p-5 flex flex-col gap-3">{Array(5).fill(0).map((_,i) => <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}</div>
        ) : filtered.length === 0 ? (
          <EmptyState Icon={Users} title={t('users.noUsers')} description={t('users.noUsersHint')} />
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {[t('users.colUsername'), t('users.colEmail'), t('users.colRole'), t('users.colSource'), t('users.colStatus'), t('users.colLastLogin'), ''].map(h => (
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {filtered.map(u => {
                const status = getUserStatus(u);
                return (
                  <tr key={u.id} className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 transition-colors">
                    <td className="px-4 py-2.5">
                      <button onClick={() => navigate(`/settings/users/${u.id}`)}
                        className="font-medium text-[var(--accent)] underline decoration-dotted hover:decoration-solid">
                        {u.username}
                      </button>
                    </td>
                    <td className="px-4 py-2.5 text-[var(--text-muted)]">{u.email ?? '—'}</td>
                    <td className="pl-2 pr-4 py-2.5"><RoleBadge role={u.role} /></td>
                    <td className="pl-2 pr-4 py-2.5"><SourceBadge source={u.source} /></td>
                    <td className="px-4 py-2.5"><span className={`text-xs font-medium ${status.color}`}>{status.label}</span></td>
                    <td className="px-4 py-2.5 text-[var(--text-muted)] text-xs">{fmtTime(u.last_login_at)}</td>
                    <td className="px-4 py-2.5 text-right">
                      <RowActionsMenu items={buildActions(u)} />
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Block>

      {purgeTarget && (
        <PurgeUserModal
          username={purgeTarget.username}
          pending={purgePending}
          onConfirm={handlePurgeConfirm}
          onCancel={() => { if (!purgePending) setPurgeTarget(null); }}
        />
      )}

      {resetTarget && (
        <ConfirmModal
          danger
          title={t('users.resetPwdTitle')}
          message={t('users.resetPwdConfirm', { user: resetTarget.username })}
          confirmLabel={t('users.resetPassword')}
          pendingLabel={t('users.resetting')}
          pending={resetPending}
          onConfirm={handleResetConfirm}
          onCancel={() => { if (!resetPending) setResetTarget(null); }}
        />
      )}

      {issuedPwd && (
        <InitialPasswordModal
          username={issuedPwd.user.username}
          password={issuedPwd.password}
          title={t('users.passwordResetTitle')}
          hint={t('users.passwordResetHint')}
          onClose={() => setIssuedPwd(null)}
        />
      )}
    </div>
  );
}
