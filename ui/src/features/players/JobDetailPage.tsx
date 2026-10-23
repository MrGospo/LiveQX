import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link, useParams } from 'react-router-dom';
import { ArrowLeft } from 'lucide-react';
import { useCommandJob, useCancelCommandJob, type CommandJobItem } from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { Block } from '@/components/Block';
import { ConfirmModal } from '@/components/ConfirmModal';
import { SubNav } from '@/components/SubNav';
import { useToast } from '@/hooks/useToast';
import { PLAYERS_NAV } from './nav';
import { absoluteTime } from './format';
import { commandTypeLabel } from './status';
import { JobStateBadge, progressOf } from './JobsPage';

// One job: how far it got, which batch is out, and what each screen answered. It stays
// current without asking: the server announces every change of a job and every settled command.

type T = (k: string, o?: Record<string, unknown>) => string;

function itemOutcome(it: CommandJobItem, t: T): string {
  switch (it.state) {
    case 'queued':          return t('players.jobs.item.queued');
    case 'skipped_offline': return t('players.jobs.item.skipped');
    case 'cancelled':       return t('players.jobs.item.cancelled');
    case 'failed':          return t('players.jobs.item.failed', { reason: t(`players.jobs.item.reason.${it.error}`, { defaultValue: it.error ?? '' }) });
    default: {              // sent: what the screen did with it
      const state = t(`players.detail.commands.state.${it.command_state ?? 'pending'}`, { defaultValue: it.command_state ?? '' });
      return it.command_result && it.command_state !== 'executed' && it.command_state !== 'pending'
        ? `${state} · ${t(`players.commandResults.${it.command_result}`, { defaultValue: it.command_result })}` : state;
    }
  }
}

export default function JobDetailPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { id = '' } = useParams();
  const job = useCommandJob(id);
  const cancel = useCancelCommandJob();
  const [asking, setAsking] = React.useState(false);

  const back = (
    <Link to="/players/jobs" className="inline-flex items-center gap-1.5 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] w-fit">
      <ArrowLeft size={14} /> {t('players.jobs.back')}
    </Link>
  );

  if (job.isLoading) return <div className="p-7"><div className="h-64 bg-surface2 rounded-xl animate-pulse" /></div>;
  if (job.isError || !job.data) {
    const notFound = job.error instanceof ApiClientError && job.error.status === 404;
    return (
      <div className="p-7 flex flex-col gap-5">
        <SubNav items={PLAYERS_NAV} />
        {back}
        <p role="alert" className="text-sm text-[var(--danger)]">{notFound ? t('players.jobs.notFound') : t('players.jobs.loadFailed')}</p>
      </div>
    );
  }

  const j = job.data;
  const p = progressOf(j);
  const s = j.summary;
  const items = [...(j.items ?? [])].sort((a, b) => (a.batch - b.batch) || (a.player_name ?? '').localeCompare(b.player_name ?? ''));

  const doCancel = async () => {
    try {
      await cancel.mutateAsync(j.id);
      toast(t('players.jobs.cancelled'), 'success');
    } catch (e) {
      const code = e instanceof ApiClientError ? e.code : '';
      toast(t(`players.jobs.errors.${code}`, { defaultValue: t('players.jobs.errors.unknown') }), 'danger');
    }
    setAsking(false);
  };

  const chips: [string, number][] = [
    ['executed', s.executed], ['pending', s.pending], ['queued', s.queued], ['unconfirmed', s.unconfirmed],
    ['failed', s.failed], ['skipped_offline', s.skipped_offline], ['cancelled', s.cancelled],
  ];

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={PLAYERS_NAV} />
      {back}

      <div className="flex flex-wrap items-center gap-3">
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">
          {commandTypeLabel(j.type, t)} — {j.target.kind === 'all' ? t('players.command.everyone') : j.target.name}
        </h1>
        <JobStateBadge state={j.state} />
        {j.state === 'running' && (
          <button type="button" onClick={() => setAsking(true)}
                  className="ml-auto px-3 py-1.5 text-sm rounded-md border border-[var(--border-subtle)] text-[var(--text-primary)] hover:border-[var(--danger)] hover:text-[var(--danger)]">
            {t('players.jobs.cancelRest')}
          </button>
        )}
      </div>

      {j.state === 'stopped' && j.stop_reason === 'too_many_unconfirmed' && (
        <p role="alert" className="text-sm text-[var(--text-primary)] m-0 rounded-md border border-[var(--danger)] bg-[var(--danger)]/10 p-3">
          {t('players.jobs.stoppedTooMany', { percent: j.stop_percent })}
        </p>
      )}

      <Block>
        <p className="text-sm text-[var(--text-muted)] m-0 mb-3">
          {t('players.jobs.startedBy', { when: absoluteTime(j.created_at), who: j.created_by_name || '—', size: j.batch_size })}
        </p>
        <p className="text-base text-[var(--text-primary)] m-0 mb-2">{t('players.jobs.progress', { done: p.done, expected: p.expected })}</p>
        <div className="h-2 bg-surface2 rounded overflow-hidden mb-4" aria-hidden>
          <div className="h-full bg-[var(--accent)]" style={{ width: `${p.percent}%` }} />
        </div>
        <ul className="list-none m-0 p-0 flex flex-wrap gap-2">
          {chips.filter(([, n]) => n > 0).map(([k, n]) => (
            <li key={k} className="text-xs rounded-full border border-[var(--border-subtle)] px-2.5 py-1 text-[var(--text-muted)]">
              {t(`players.jobs.summary.${k}`)} · {n}
            </li>
          ))}
        </ul>
      </Block>

      <Block padding="p-0">
        <table className="w-full text-sm">
          <thead>
            <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
              <th className="font-medium px-4 py-2.5">{t('players.jobs.item.col.screen')}</th>
              <th className="font-medium px-4 py-2.5">{t('players.jobs.item.col.batch')}</th>
              <th className="font-medium px-4 py-2.5">{t('players.jobs.item.col.outcome')}</th>
            </tr>
          </thead>
          <tbody>
            {items.map(it => (
              <tr key={it.player_id} className="border-b border-[var(--border-subtle)] last:border-b-0">
                <td className="px-4 py-2">
                  <Link to={`/players/${it.player_id}`} className="text-[var(--text-primary)] hover:text-[var(--accent)]">
                    {it.player_name || it.player_id.slice(0, 8)}
                  </Link>
                </td>
                <td className="px-4 py-2 text-[var(--text-muted)]">{it.batch < 0 ? t('players.jobs.item.notConnected') : it.batch + 1}</td>
                <td className="px-4 py-2 text-[var(--text-primary)]">{itemOutcome(it, t as unknown as T)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </Block>

      {asking && (
        <ConfirmModal danger title={t('players.jobs.confirm.title')} message={t('players.jobs.confirm.text')}
                      confirmLabel={t('players.jobs.cancelRest')} pending={cancel.isPending}
                      onCancel={() => setAsking(false)} onConfirm={doCancel} />
      )}
    </div>
  );
}
