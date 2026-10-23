import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { AlertTriangle, MonitorSmartphone } from 'lucide-react';
import { useBrokerStatus, useCommandJobs, usePlayersSummary } from '@/api/queries/players';
import { Block } from '@/components/Block';
import { useAuthStore } from '@/stores/auth';

// Screens at a glance, for the main page: how many are on, how many are not, and what needs
// a look. It shows only what the person may see (the server cuts the counts to their rights);
// for someone with no screens at all it does not appear. Numbers follow the live events, so
// there is nothing to refresh by hand.

export function PlayersSummary() {
  const { t } = useTranslation();
  const role = useAuthStore(s => s.role);
  const isAdmin = role === 'admin';
  const mayCommand = role === 'admin' || role === 'operator';
  const summary = usePlayersSummary();
  const broker = useBrokerStatus({ enabled: isAdmin });
  const jobs = useCommandJobs({ enabled: mayCommand });

  // The module off, or an error: say nothing here rather than a failure on the main page.
  if (summary.isLoading || summary.isError || !summary.data) return null;
  const { counts, total } = summary.data;
  const all = counts.online + counts.offline + counts.pending + counts.revoked;
  if (all === 0 && !isAdmin) return null;                      // nothing to show to someone with no screens

  const running = (jobs.data ?? []).filter(j => j.state === 'running').length;
  const brokerDown = isAdmin && !!broker.data && broker.data.enabled && broker.data.state !== 'connected';
  const cells: { key: 'online' | 'offline' | 'pending' | 'revoked'; tone: string }[] = [
    { key: 'online',  tone: 'text-[var(--success)]' },
    { key: 'offline', tone: counts.offline > 0 ? 'text-[var(--danger)]' : 'text-[var(--text-primary)]' },
    { key: 'pending', tone: counts.pending > 0 ? 'text-[var(--warning)]' : 'text-[var(--text-primary)]' },
    { key: 'revoked', tone: 'text-[var(--text-muted)]' },
  ];

  return (
    <Block padding="p-5">
      <div className="flex items-center gap-2 mb-4">
        <MonitorSmartphone size={16} className="text-[var(--accent)]" />
        <h2 className="text-base font-semibold text-[var(--text-primary)] m-0">{t('players.summary.title')}</h2>
        <Link to="/players" className="ml-auto text-sm text-[var(--accent)] hover:underline">{t('players.summary.open')}</Link>
      </div>

      {all === 0 ? (
        <p className="text-sm text-[var(--text-muted)] m-0">{t('players.summary.none')}</p>
      ) : (
        <dl className="grid grid-cols-2 sm:grid-cols-4 gap-4 m-0">
          {cells.map(c => (
            <div key={c.key} className="text-center">
              <dd className={`m-0 text-3xl font-bold tabular-nums ${c.tone}`}>{counts[c.key]}</dd>
              <dt className="text-sm text-[var(--text-muted)] mt-1">
                <Link to={`/players?status=${c.key}`} className="hover:text-[var(--text-primary)]">{t(`players.status.${c.key}`)}</Link>
              </dt>
            </div>
          ))}
        </dl>
      )}

      {(counts.offline > 0 || running > 0 || brokerDown) && (
        <ul className="list-none m-0 mt-4 p-0 flex flex-col gap-1.5 text-sm" aria-label={t('players.summary.attention')}>
          {counts.offline > 0 && total > 0 && (
            <li className="text-[var(--text-muted)]">
              {t('players.summary.offlineNote', { count: counts.offline })}{' '}
              <Link to="/players?status=offline" className="text-[var(--accent)] hover:underline">{t('players.summary.show')}</Link>
            </li>
          )}
          {running > 0 && (
            <li className="text-[var(--text-muted)]">
              {t('players.summary.jobs', { count: running })}{' '}
              <Link to="/players/jobs" className="text-[var(--accent)] hover:underline">{t('players.summary.show')}</Link>
            </li>
          )}
          {brokerDown && (
            <li className="flex items-center gap-1.5 text-[var(--warning)]">
              <AlertTriangle size={14} aria-hidden /> {t('players.summary.brokerDown')}{' '}
              <Link to="/players/broker" className="text-[var(--accent)] hover:underline">{t('players.summary.show')}</Link>
            </li>
          )}
        </ul>
      )}
    </Block>
  );
}
