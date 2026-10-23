import { useTranslation } from 'react-i18next';
import { Link, useNavigate } from 'react-router-dom';
import { ListChecks } from 'lucide-react';
import { clsx } from 'clsx';
import { useCommandJobs, type CommandJob, type JobState } from '@/api/queries/players';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { PLAYERS_NAV } from './nav';
import { absoluteTime } from './format';
import { commandTypeLabel } from './status';

// Commands that go out in batches to a group, an object or everyone. The list is the
// person's own jobs (an administrator sees all); a job is opened to follow it screen by screen.

const STATE_STYLE: Record<JobState, string> = {
  running:   'text-[var(--accent)] border-[var(--accent)] bg-[var(--accent)]/10',
  done:      'text-[var(--success)] border-[var(--success)] bg-[var(--success)]/10',
  stopped:   'text-[var(--danger)] border-[var(--danger)] bg-[var(--danger)]/10',
  cancelled: 'text-[var(--text-muted)] border-[var(--border-subtle)] bg-surface2',
};

export function JobStateBadge({ state }: { state: JobState }) {
  const { t } = useTranslation();
  return (
    <span className={clsx('inline-flex items-center rounded-full border px-2.5 py-0.5 text-xs font-medium whitespace-nowrap', STATE_STYLE[state])}>
      {t(`players.jobs.state.${state}`)}
    </span>
  );
}

// How far it got: screens that confirmed, out of those that were to receive it (not connected ones are not counted).
export const progressOf = (j: CommandJob) => {
  const expected = Math.max(0, j.summary.total - j.summary.skipped_offline);
  return { done: j.summary.executed, expected, percent: expected ? Math.round((j.summary.executed / expected) * 100) : 0 };
};

export default function JobsPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const { data, isLoading, isError } = useCommandJobs();

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={PLAYERS_NAV} />
      <div className="flex items-center gap-3">
        <ListChecks size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{t('players.jobs.title')}</h1>
      </div>
      <p className="text-sm text-[var(--text-muted)] -mt-3 mb-0">{t('players.jobs.lead')}</p>

      {isLoading ? (
        <div className="h-48 bg-surface2 rounded-xl animate-pulse" />
      ) : isError || !data ? (
        <p role="alert" className="text-sm text-[var(--danger)]">{t('players.jobs.loadFailed')}</p>
      ) : data.length === 0 ? (
        <Block><EmptyState Icon={ListChecks} title={t('players.jobs.empty.title')} description={t('players.jobs.empty.text')} /></Block>
      ) : (
        <Block padding="p-0">
          <table className="w-full text-sm">
            <thead>
              <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.started')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.command')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.target')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.by')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.state')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.jobs.col.progress')}</th>
              </tr>
            </thead>
            <tbody>
              {data.map(j => {
                const p = progressOf(j);
                return (
                  <tr key={j.id} onClick={() => navigate(`/players/jobs/${j.id}`)}
                      className="border-b border-[var(--border-subtle)] last:border-b-0 hover:bg-surface2 cursor-pointer">
                    <td className="px-4 py-2.5 text-[var(--text-muted)] whitespace-nowrap">
                      <Link to={`/players/jobs/${j.id}`} onClick={e => e.stopPropagation()} className="hover:text-[var(--accent)]">{absoluteTime(j.created_at)}</Link>
                    </td>
                    <td className="px-4 py-2.5 text-[var(--text-primary)]">{commandTypeLabel(j.type, t)}</td>
                    <td className="px-4 py-2.5 text-[var(--text-primary)]">{j.target.kind === 'all' ? t('players.command.everyone') : j.target.name}</td>
                    <td className="px-4 py-2.5 text-[var(--text-muted)]">{j.created_by_name || '—'}</td>
                    <td className="px-4 py-2.5"><JobStateBadge state={j.state} /></td>
                    <td className="px-4 py-2.5 text-[var(--text-muted)] whitespace-nowrap">
                      {t('players.jobs.progress', { done: p.done, expected: p.expected })}
                      <div className="h-1 w-24 bg-surface2 rounded mt-1 overflow-hidden" aria-hidden>
                        <div className="h-full bg-[var(--accent)]" style={{ width: `${p.percent}%` }} />
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </Block>
      )}
    </div>
  );
}
