import React from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft } from 'lucide-react';
import { useUser, useUpdateUser, useUserChannelGrants, useSetChannelGrant, useRemoveChannelGrant } from '@/api/queries/auth';
import { useChannels } from '@/api/queries/channels';
import { Block } from '@/components/Block';
import { RoleBadge, SourceBadge } from '@/components/HealthBadge';
import { fmtDate, fmtTime } from '@/lib/format';
import { useToast } from '@/hooks/useToast';

// ─── Grants Tab ───────────────────────────────────────────────────────────────
function GrantsTab({ userId }: { userId: number }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { data: grants = [], isLoading: grantsLoading } = useUserChannelGrants(userId);
  const { data: channels = [] } = useChannels();
  const { mutate: setGrant } = useSetChannelGrant(userId);
  const { mutate: removeGrant } = useRemoveChannelGrant(userId);

  // Build a map: channelId → current permission
  const grantMap = React.useMemo(() =>
    Object.fromEntries(grants.map(g => [g.channel_id, g.permission])),
    [grants]
  );

  const handleChange = (channelId: number, value: string) => {
    if (value === 'none') {
      removeGrant(channelId, { onSuccess: () => toast(t('users.grantRemoved'), 'info') });
    } else {
      setGrant(
        { channelId, permission: value },
        { onSuccess: () => toast(t('users.grantSaved'), 'success') },
      );
    }
  };

  if (grantsLoading) return (
    <div className="flex flex-col gap-3 max-w-xl">
      {Array(4).fill(0).map((_,i) => <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}
    </div>
  );

  return (
    <div className="max-w-xl flex flex-col gap-4">
      <p className="text-sm text-[var(--text-muted)]">{t('users.grantsHint')}</p>
      <Block padding="p-0">
        {channels.length === 0 ? (
          <p className="p-4 text-sm text-[var(--text-muted)]">{t('users.noChannels')}</p>
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                <th className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{t('users.grantChannel')}</th>
                <th className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{t('users.grantPermission')}</th>
              </tr>
            </thead>
            <tbody>
              {channels.map(ch => {
                const current = grantMap[ch.id] ?? 'none';
                return (
                  <tr key={ch.id} className="border-b last:border-0 border-[var(--border-subtle)]">
                    <td className="px-4 py-2.5 text-[var(--text-primary)] font-medium">
                      ch{ch.id}‑{ch.name}
                    </td>
                    <td className="px-4 py-2.5">
                      <select
                        value={current}
                        onChange={e => handleChange(ch.id, e.target.value)}
                        className="bg-canvas border border-[var(--border-subtle)] rounded-md px-2 py-1 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]"
                      >
                        <option value="none">{t('common.none')}</option>
                        <option value="view">{t('users.permView')}</option>
                        <option value="operate">{t('users.permOperate')}</option>
                      </select>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Block>
    </div>
  );
}

// ─── Overview Tab ─────────────────────────────────────────────────────────────
function OverviewTab({ user, uid }: { user: ReturnType<typeof useUser>['data']; uid: number }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { mutateAsync: update, isPending } = useUpdateUser(uid);
  const [email, setEmail] = React.useState(user?.email ?? '');
  const [role, setRole] = React.useState(user?.role ?? 'viewer');

  React.useEffect(() => {
    if (user) { setEmail(user.email ?? ''); setRole(user.role); }
  }, [user]);

  if (!user) return null;

  return (
    <Block>
      <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('users.editUser')}</h2>
      <div className="grid grid-cols-2 gap-4 mb-5">
        <div className="flex flex-col gap-1.5">
          <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.colEmail')}</label>
          <input value={email} onChange={e => setEmail(e.target.value)} type="email"
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
        </div>
        <div className="flex flex-col gap-1.5">
          <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.role')}</label>
          <select value={role} onChange={e => setRole(e.target.value as typeof role)}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
            {['viewer','operator','admin'].map(r => <option key={r} value={r}>{r}</option>)}
          </select>
        </div>
      </div>
      <div className="grid grid-cols-3 gap-4 bg-canvas rounded-lg p-4 mb-4 text-sm">
        {([[t('users.created'), fmtDate(user.created_at)], [t('users.colLastLogin'), fmtTime(user.last_login_at)], [t('users.failedLogins'), user.failed_login_count]] as [string, unknown][]).map(([k,v]) => (
          <div key={k}><dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt><dd className="text-[var(--text-primary)]">{String(v ?? '—')}</dd></div>
        ))}
      </div>
      <div className="flex gap-2">
        <button disabled={isPending} onClick={() => update({ email, role }).then(() => toast(t('common.success'), 'success'))}
          className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {isPending ? t('common.loading') : t('common.save')}
        </button>
      </div>
    </Block>
  );
}

// ─── Main UserDetailPage ──────────────────────────────────────────────────────
export default function UserDetailPage() {
  const { id } = useParams<{ id: string }>();
  const navigate = useNavigate();
  const { t } = useTranslation();
  const uid = Number(id);
  const { data: user, isLoading } = useUser(uid);
  const [tab, setTab] = React.useState<'overview'|'grants'>('overview');

  if (isLoading) return <div className="p-7"><div className="h-48 bg-surface2 rounded-xl animate-pulse" /></div>;
  if (!user) return <div className="p-7 text-[var(--text-muted)]">{t('users.notFound')}</div>;

  // Admin-to-admin grants are hidden per spec
  const showGrantsTab = user.role !== 'admin';

  return (
    <div className="flex flex-col h-full">
      {/* Sticky header */}
      <div className="sticky top-0 z-10 bg-surface border-b border-[var(--border-subtle)] px-7 pt-4">
        <button onClick={() => navigate('/settings/users')}
          className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] mb-3 transition-colors">
          <ChevronLeft size={15} /> {t('users.title')}
        </button>
        <div className="flex items-center gap-3 mb-3 flex-wrap">
          <h1 className="text-2xl font-bold text-[var(--text-primary)]">{user.username}</h1>
          <RoleBadge role={user.role} />
          <SourceBadge source={user.source} />
        </div>
        <div className="flex gap-0 -mb-px">
          {([['overview', t('users.tabOverview')], ...(showGrantsTab ? [['grants', t('users.tabGrants')]] : [])] as [string, string][]).map(([id, label]) => (
            <button key={id} onClick={() => setTab(id as typeof tab)}
              className={`px-4 py-2.5 text-sm font-medium border-b-2 transition-colors ${tab === id ? 'text-[var(--accent)] border-[var(--accent)]' : 'text-[var(--text-muted)] border-transparent hover:text-[var(--text-primary)]'}`}>
              {label}
            </button>
          ))}
        </div>
      </div>

      <div className="flex-1 overflow-y-auto p-7 max-w-2xl">
        {tab === 'overview' && <OverviewTab user={user} uid={uid} />}
        {tab === 'grants' && showGrantsTab && <GrantsTab userId={uid} />}
      </div>
    </div>
  );
}
