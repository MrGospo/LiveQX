import { useTranslation } from 'react-i18next';
import { User, LogOut } from 'lucide-react';
import { useAuthStore } from '@/stores/auth';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from '@/features/settings/nav';
import { RoleBadge, SourceBadge } from '@/components/HealthBadge';
import { EmptyState } from '@/components/EmptyState';
import {
  useChangePassword,
  useMe,
  useOwnSessions,
  useRevokeOwnSession,
} from '@/api/queries/auth';
import { useToast } from '@/hooks/useToast';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { changePasswordSchema } from '@/schemas';
import type { ChangePasswordFormValues } from '@/schemas';

function useFmtRelative() {
  const { t } = useTranslation();
  return (unixSec: number | null | undefined): string => {
    if (unixSec == null) return '—';
    const diff = Math.floor(Date.now() / 1000) - unixSec;
    if (diff < 0)        return new Date(unixSec * 1000).toLocaleString();
    if (diff < 60)       return t('profile.agoSec',  { n: diff });
    if (diff < 3_600)    return t('profile.agoMin',  { n: Math.floor(diff / 60)    });
    if (diff < 86_400)   return t('profile.agoHour', { n: Math.floor(diff / 3600)  });
    return                  t('profile.agoDay',  { n: Math.floor(diff / 86_400) });
  };
}

const LANGUAGES = [
  { code: 'ru', label: 'Русский' },
  { code: 'en', label: 'English' },
] as const;

export default function ProfilePage() {
  const { t, i18n }  = useTranslation();
  const toast  = useToast();
  const user   = useAuthStore(s => s.user);
  const role   = useAuthStore(s => s.role);
  const logout = useAuthStore(s => s.logout);

  const { mutateAsync: changePassword } = useChangePassword();
  const { data: me } = useMe();
  const { data: sessions = [], isLoading: sessionsLoading } = useOwnSessions();
  const { mutateAsync: revokeSession, isPending: revoking } = useRevokeOwnSession();
  const fmtRelative = useFmtRelative();

  const { register, handleSubmit, reset, formState: { errors, isSubmitting } } = useForm<ChangePasswordFormValues>({
    resolver: zodResolver(changePasswordSchema),
  });

  const onSubmit = async (values: ChangePasswordFormValues) => {
    try {
      await changePassword({ current_password: values.current_password, new_password: values.new_password });
      toast(t('common.success'), 'success');
      reset();
    } catch (e: unknown) {
      toast((e as Error).message, 'danger');
    }
  };

  const onRevoke = async (jwt_id: string, current: boolean) => {
    try {
      const r = await revokeSession(jwt_id);
      toast(t('common.success'), 'success');
      // backend подтверждает revoke текущей — следующий запрос вернёт 401,
      // делаем clean logout сразу, чтобы UI не моргнул.
      if (current || r.current) logout();
    } catch (e: unknown) {
      toast((e as Error).message, 'danger');
    }
  };

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('profile.account')}</h2>
        <dl className="grid grid-cols-6 gap-x-6 gap-y-5">
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.username')}</dt>
            <dd className="text-sm text-[var(--text-primary)]">{me?.username ?? user?.username ?? '—'}</dd>
          </div>
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.email')}</dt>
            <dd className="text-sm text-[var(--text-primary)]">{me?.email || '—'}</dd>
          </div>
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.role')}</dt>
            <dd className="-ml-2">{(me?.role ?? role) ? <RoleBadge role={(me?.role ?? role)!} /> : '—'}</dd>
          </div>
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.source')}</dt>
            <dd className="-ml-2"><SourceBadge source={me?.source ?? 'local'} /></dd>
          </div>
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.lastLogin')}</dt>
            <dd className="text-sm text-[var(--text-primary)]">{fmtRelative(me?.last_login_at)}</dd>
          </div>
          <div>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('profile.lastIp')}</dt>
            <dd className="text-sm font-mono text-[var(--text-primary)]">{me?.last_login_ip || '—'}</dd>
          </div>
        </dl>
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('profile.preferences')}</h2>
        <div className="flex flex-col gap-1.5 max-w-sm">
          <label className="text-sm font-medium text-[var(--text-primary)]">{t('profile.language')}</label>
          <select value={i18n.language.startsWith('ru') ? 'ru' : 'en'}
            onChange={e => { void i18n.changeLanguage(e.target.value); }}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors">
            {LANGUAGES.map(l => <option key={l.code} value={l.code}>{l.label}</option>)}
          </select>
          <p className="text-xs text-[var(--text-muted)]">{t('profile.languageHint')}</p>
        </div>
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-5">{t('profile.changePassword')}</h2>
        <form onSubmit={handleSubmit(onSubmit)} className="flex flex-col gap-4 max-w-sm">
          {(['current_password', 'new_password', 'confirm_password'] as const).map(field => (
            <div key={field} className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t(`auth.${field === 'current_password' ? 'currentPassword' : field === 'new_password' ? 'newPassword' : 'confirmPassword'}`)}</label>
              <input {...register(field)} type="password"
                className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors" />
              {errors[field] && <p className="text-sm text-[var(--danger)]">{errors[field]?.message}</p>}
            </div>
          ))}
          <button type="submit" disabled={isSubmitting}
            className="self-start h-9 px-4 bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50 text-white text-sm font-medium rounded-md transition-colors">
            {isSubmitting ? t('common.loading') : t('common.save')}
          </button>
        </form>
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-3">{t('profile.activeSessions')}</h2>
        {sessionsLoading ? (
          <div className="h-16 bg-surface2 rounded-md animate-pulse" />
        ) : sessions.length === 0 ? (
          <EmptyState Icon={User} title={t('profile.noSessions')} description="—" />
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {[t('profile.colDevice'), t('profile.colIp'), t('profile.colLastSeen'), t('profile.colCreated'), ''].map(h => (
                  <th key={h}
                    className="px-3 py-2 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {sessions.map(s => (
                <tr key={s.jwt_id}
                    className="border-b last:border-0 border-[var(--border-subtle)]">
                  <td className="px-3 py-2.5 text-[var(--text-primary)]">
                    <span className="font-mono text-xs truncate inline-block max-w-xs align-middle">
                      {s.user_agent || '—'}
                    </span>
                    {s.current && (
                      <span className="ml-2 px-1.5 py-0.5 rounded text-[10px] uppercase tracking-wider bg-[var(--accent)]/10 text-[var(--accent)] border border-[var(--accent)]/30">
                        {t('profile.thisDevice')}
                      </span>
                    )}
                  </td>
                  <td className="px-3 py-2.5 font-mono text-xs text-[var(--text-muted)]">{s.ip || '—'}</td>
                  <td className="px-3 py-2.5 text-xs text-[var(--text-muted)]">{fmtRelative(s.last_seen_at)}</td>
                  <td className="px-3 py-2.5 text-xs text-[var(--text-muted)]">{fmtRelative(s.created_at)}</td>
                  <td className="px-3 py-2.5 text-right">
                    <button onClick={() => onRevoke(s.jwt_id, s.current)} disabled={revoking}
                      className="inline-flex items-center gap-1.5 px-2 py-1 text-xs text-[var(--text-muted)] hover:text-[var(--danger)] disabled:opacity-50 transition-colors">
                      <LogOut size={12} /> {t('profile.revoke')}
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Block>
    </div>
  );
}
