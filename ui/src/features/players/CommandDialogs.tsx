import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { X } from 'lucide-react';
import {
  useBulkPlan, useBulkSend, useScreenCommand,
  type BulkPlan, type BulkReport, type BulkTarget, type CommandType,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { useEscClose } from '@/hooks/useEscClose';
import { useDialogFocus } from '@/hooks/useDialogFocus';
import { useToast } from '@/hooks/useToast';
import { Field, TextField, inputCls } from './fields';
import { phraseMatches } from './commands';
import { commandTypeLabel } from './status';

// Sending a command. To a group, an object or everyone it is two steps: first the server is
// asked what would happen (nothing is sent), then the person types a phrase that names the
// target and the command, and only that sends. To one screen it is a single question.

const DANGEROUS: CommandType[] = ['restart_player', 'reboot_screen'];   // the screen goes dark

function Shell({ title, pending, onClose, children, footer }: {
  title: string; pending: boolean; onClose: () => void; children: React.ReactNode; footer: React.ReactNode;
}) {
  useEscClose(onClose, !pending);
  const dialogRef = useDialogFocus<HTMLDivElement>();
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div ref={dialogRef} role="dialog" aria-modal="true" aria-label={title}
           className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-lg max-h-[90vh] overflow-y-auto shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)] m-0">{title}</h2>
          <button type="button" onClick={onClose} disabled={pending} aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30"><X size={18} /></button>
        </div>
        <div className="p-5 flex flex-col gap-4">{children}</div>
        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end flex-wrap">{footer}</div>
      </div>
    </div>
  );
}

const Cancel = ({ onClick, disabled, label }: { onClick: () => void; disabled?: boolean; label: string }) => (
  <button type="button" onClick={onClick} disabled={disabled}
          className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30">
    {label}
  </button>
);

const Primary = ({ onClick, disabled, label, danger }: { onClick: () => void; disabled?: boolean; label: string; danger?: boolean }) => (
  <button type="button" onClick={onClick} disabled={disabled}
          className={'px-4 py-2 text-white text-sm rounded-md disabled:opacity-40 disabled:cursor-not-allowed ' +
            (danger ? 'bg-[var(--danger)] hover:bg-[var(--danger)]/85' : 'bg-[var(--accent)] hover:bg-[var(--accent-hover)]')}>
    {label}
  </button>
);

function TypePicker({ types, value, onChange, disabled }: {
  types: CommandType[]; value: CommandType; onChange: (t: CommandType) => void; disabled?: boolean;
}) {
  const { t } = useTranslation();
  return (
    <fieldset className="border-0 p-0 m-0 flex flex-col gap-2" disabled={disabled}>
      <legend className="text-sm font-medium text-[var(--text-primary)] mb-1">{t('players.command.which')}</legend>
      {types.map(ty => (
        <label key={ty} className="flex gap-3 items-start rounded-md border border-[var(--border-subtle)] p-3 cursor-pointer has-[:checked]:border-[var(--accent)]">
          <input type="radio" name="command-type" value={ty} checked={value === ty} onChange={() => onChange(ty)} className="mt-1" />
          <span>
            <span className="block text-sm text-[var(--text-primary)] font-medium">{commandTypeLabel(ty, t)}</span>
            <span className="block text-xs text-[var(--text-muted)]">{t(`players.command.hint.${ty}`)}</span>
          </span>
        </label>
      ))}
    </fieldset>
  );
}

const errorOf = (e: unknown) => ({
  code: e instanceof ApiClientError ? e.code : 'unknown',
  body: e instanceof ApiClientError ? (e.body ?? {}) : {},
});

// ─── A group, an object or everyone ──────────────────────────────────────────

export function BulkCommandDialog({ target, targetLabel, types, onClose }: {
  target: BulkTarget; targetLabel: string; types: CommandType[]; onClose: () => void;
}) {
  const { t } = useTranslation();
  const planQ = useBulkPlan();
  const sendQ = useBulkSend();
  const [type, setType] = React.useState<CommandType>(types[0]);
  const [plan, setPlan] = React.useState<BulkPlan | null>(null);
  const [typed, setTyped] = React.useState('');
  const [report, setReport] = React.useState<BulkReport | null>(null);
  const [error, setError] = React.useState<{ code: string; body: Record<string, unknown> } | null>(null);
  const pending = planQ.isPending || sendQ.isPending;

  const check = async () => {
    setError(null);
    try { setPlan(await planQ.mutateAsync({ target, type })); setTyped(''); }
    catch (e) { setError(errorOf(e)); }
  };

  const send = async () => {
    if (!plan) return;
    setError(null);
    try { setReport(await sendQ.mutateAsync({ target, type, confirm_phrase: typed.trim(), expected_count: plan.expected_count })); }
    catch (e) {
      const er = errorOf(e);
      setError(er);
      // The screens changed since they were shown (or became busy): what was shown is stale, look again.
      if (er.code === 'expected_count_mismatch' || er.code === 'screens_busy') setPlan(null);
    }
  };

  const title = t('players.command.title', { target: targetLabel });
  const errText = error && (
    <p role="alert" className="text-sm text-[var(--danger)] m-0">
      {t(`players.command.errors.${error.code}`, {
        defaultValue: t('players.command.errors.unknown'),
        actual: String(error.body.actual ?? ''),
      })}
      {Array.isArray(error.body.busy) && error.body.busy.length > 0 && (
        <span className="block mt-1">
          {(error.body.busy as { name?: string; player_id: string }[]).map(b => b.name || b.player_id.slice(0, 8)).join(', ')}
        </span>
      )}
    </p>
  );

  // ── done ──
  if (report) {
    return (
      <Shell title={title} pending={false} onClose={onClose}
             footer={<Primary onClick={onClose} label={t('players.command.close')} />}>
        <p className="text-sm text-[var(--text-primary)] m-0">
          {t('players.command.sentNow', { count: report.sent.length, type: commandTypeLabel(report.type, t) })}
          {(report.queued ?? 0) > 0 && ` ${t('players.command.queued', { count: report.queued, batch: report.batch_size })}`}
        </p>
        {report.skipped_offline.length > 0 && (
          <p className="text-sm text-[var(--text-muted)] m-0">{t('players.command.skipped', { count: report.skipped_offline.length })}</p>
        )}
        {report.failed.length > 0 && (
          <p role="alert" className="text-sm text-[var(--danger)] m-0">{t('players.command.failed', { count: report.failed.length })}</p>
        )}
        {report.job_id && (
          <Link to={`/players/jobs/${report.job_id}`} onClick={onClose} className="text-sm text-[var(--accent)] hover:underline w-fit">
            {t('players.command.watch')}
          </Link>
        )}
      </Shell>
    );
  }

  // ── choose and check ──
  if (!plan) {
    return (
      <Shell title={title} pending={pending} onClose={onClose}
             footer={<>
               <Cancel onClick={onClose} disabled={pending} label={t('common.cancel')} />
               <Primary onClick={check} disabled={pending} label={pending ? t('players.command.checking') : t('players.command.check')} />
             </>}>
        <TypePicker types={types} value={type} onChange={setType} disabled={pending} />
        <p className="text-xs text-[var(--text-muted)] m-0">{t('players.command.checkNote')}</p>
        {errText}
      </Shell>
    );
  }

  // ── confirm ──
  const blocked = plan.busy.length > 0 || plan.online === 0;
  const danger = DANGEROUS.includes(type);
  return (
    <Shell title={title} pending={pending} onClose={onClose}
           footer={<>
             <Cancel onClick={onClose} disabled={pending} label={t('common.cancel')} />
             <Primary danger={danger} onClick={send} disabled={pending || blocked || !phraseMatches(typed, plan.required_phrase)}
                      label={pending ? t('players.command.sending') : t('players.command.send')} />
           </>}>
      <p className="text-sm text-[var(--text-primary)] m-0">
        <strong>{commandTypeLabel(type, t)}</strong> — {plan.target_name}
      </p>
      <ul className="text-sm text-[var(--text-primary)] m-0 pl-5 list-disc">
        <li>{t('players.command.willGet', { count: plan.online })}</li>
        {plan.offline.length > 0 && <li className="text-[var(--text-muted)]">{t('players.command.willSkip', { count: plan.offline.length })}</li>}
      </ul>
      {plan.online === 0 && <p role="alert" className="text-sm text-[var(--danger)] m-0">{t('players.command.errors.no_targets')}</p>}
      {plan.busy.length > 0 && (
        <p role="alert" className="text-sm text-[var(--danger)] m-0">
          {t('players.command.busy', { count: plan.busy.length })}
          <span className="block mt-1">{plan.busy.map(b => b.name || b.player_id.slice(0, 8)).join(', ')}</span>
        </p>
      )}
      {!blocked && (
        <>
          <p className="text-xs text-[var(--text-muted)] m-0">{t('players.command.batches')}</p>
          <TextField id="cmd-phrase" mono label={t('players.command.typePhrase')} value={typed} onChange={setTyped}
                     hint={<span className="font-mono">{plan.required_phrase}</span>} disabled={pending} />
        </>
      )}
      {errText}
    </Shell>
  );
}

// ─── One screen ──────────────────────────────────────────────────────────────

export function ScreenCommandDialog({ player, online, onClose }: {
  player: { id: string; name: string }; online: boolean; onClose: () => void;
}) {
  const { t } = useTranslation();
  const toast = useToast();
  const sendQ = useScreenCommand();
  const [type, setType] = React.useState<CommandType>('restart_player');
  const [error, setError] = React.useState<string | null>(null);

  const send = async () => {
    setError(null);
    try {
      await sendQ.mutateAsync({ id: player.id, type });
      toast(t('players.command.sentOne', { type: commandTypeLabel(type, t) }), 'success');
      onClose();
    } catch (e) { setError(errorOf(e).code); }
  };

  return (
    <Shell title={t('players.command.titleOne', { name: player.name })} pending={sendQ.isPending} onClose={onClose}
           footer={<>
             <Cancel onClick={onClose} disabled={sendQ.isPending} label={t('common.cancel')} />
             <Primary danger={DANGEROUS.includes(type)} onClick={send} disabled={sendQ.isPending}
                      label={sendQ.isPending ? t('players.command.sending') : t('players.command.send')} />
           </>}>
      <Field id="cmd-one-type" label={t('players.command.which')} hint={t(`players.command.hint.${type}`)}>
        <select id="cmd-one-type" value={type} onChange={e => setType(e.target.value as CommandType)} disabled={sendQ.isPending} className={inputCls}>
          {(['restart_player', 'reboot_screen', 'reload_playlist'] as CommandType[]).map(ty =>
            <option key={ty} value={ty}>{commandTypeLabel(ty, t)}</option>)}
        </select>
      </Field>
      {!online && <p role="status" className="text-sm text-[var(--warning)] m-0">{t('players.command.offlineOne')}</p>}
      {error && (
        <p role="alert" className="text-sm text-[var(--danger)] m-0">
          {t(`players.command.errors.${error}`, { defaultValue: t('players.command.errors.unknown') })}
        </p>
      )}
    </Shell>
  );
}
