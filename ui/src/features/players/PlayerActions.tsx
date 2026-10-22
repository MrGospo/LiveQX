import React from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import { ArrowRightLeft, KeyRound, RefreshCw, ShieldOff, Trash2, X } from 'lucide-react';
import {
  usePlayerActions, usePlayerTree,
  type PairingPackage, type Player,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { useAuthStore } from '@/stores/auth';
import { useEscClose } from '@/hooks/useEscClose';
import { useToast } from '@/hooks/useToast';
import { Field, TextField, inputCls } from './fields';
import { PairingPackageDialog } from './PairingPackageDialog';
import { screenStatus } from './status';

// What can be done to one screen, as a row of buttons on its card. Who sees which
// button follows the server: moving is open to an operator (the server then
// checks the move right on both groups), everything else to an administrator.

const MAX_REASON_BYTES = 200;

function reasonProblem(raw: string): boolean {
  if (new TextEncoder().encode(raw).length > MAX_REASON_BYTES) return true;
  // eslint-disable-next-line no-control-regex
  return /[\u0000-\u001f\u007f]/.test(raw);
}

type Mode = 'move' | 'token' | 'reenroll' | 'revoke' | 'delete';

function Shell({ title, pending, onClose, onSubmit, submitLabel, danger, submitDisabled, children }: {
  title: string; pending: boolean; onClose: () => void; onSubmit: () => void; submitLabel: string;
  danger?: boolean; submitDisabled?: boolean; children: React.ReactNode;
}) {
  const { t } = useTranslation();
  useEscClose(onClose, !pending);
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <form onSubmit={e => { e.preventDefault(); onSubmit(); }} role="dialog" aria-modal="true" aria-label={title}
            className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)] m-0">{title}</h2>
          <button type="button" onClick={onClose} disabled={pending} aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30"><X size={18} /></button>
        </div>
        <div className="p-5 flex flex-col gap-4">{children}</div>
        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button type="button" onClick={onClose} disabled={pending}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30">
            {t('common.cancel')}
          </button>
          <button type="submit" disabled={pending || submitDisabled}
                  className={'px-4 py-2 text-white text-sm rounded-md disabled:opacity-40 disabled:cursor-not-allowed ' +
                    (danger ? 'bg-[var(--danger)] hover:bg-[var(--danger)]/85' : 'bg-[var(--accent)] hover:bg-[var(--accent-hover)]')}>
            {submitLabel}
          </button>
        </div>
      </form>
    </div>
  );
}

const ActionButton = ({ onClick, icon, label, danger }: { onClick: () => void; icon: React.ReactNode; label: string; danger?: boolean }) => (
  <button type="button" onClick={onClick}
          className={'inline-flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-md border transition-colors ' +
            (danger
              ? 'border-[var(--border-subtle)] text-[var(--text-primary)] hover:border-[var(--danger)] hover:text-[var(--danger)]'
              : 'border-[var(--border-subtle)] text-[var(--text-primary)] hover:bg-surface2')}>
    {icon}{label}
  </button>
);

export function PlayerActions({ player }: { player: Player }) {
  const { t } = useTranslation();
  const toast = useToast();
  const navigate = useNavigate();
  const role = useAuthStore(s => s.role);
  const isAdmin = role === 'admin';
  const canMove = isAdmin || role === 'operator';
  const status = screenStatus(player);
  const tree = usePlayerTree();
  const { move, reissueToken, reEnroll, revoke, remove } = usePlayerActions();

  const [mode, setMode] = React.useState<Mode | null>(null);
  const [target, setTarget] = React.useState('');
  const [reason, setReason] = React.useState('');
  const [typed, setTyped] = React.useState('');
  const [errorCode, setErrorCode] = React.useState<string | null>(null);
  const [given, setGiven] = React.useState<PairingPackage | null>(null);

  const open = (m: Mode) => { setMode(m); setErrorCode(null); setTarget(''); setReason(''); setTyped(''); };
  const close = () => { setMode(null); setErrorCode(null); };
  const fail = (e: unknown) => setErrorCode(e instanceof ApiClientError ? e.code : 'unknown');
  const done = (key: string) => { toast(t(key), 'success'); close(); };

  if (!canMove) return null;

  const groups = (tree.data ?? []).filter(o => o.groups.some(g => g.id !== player.group_id));
  const showMove = canMove && status !== 'revoked';
  const showToken = isAdmin && status === 'pending';
  const showReEnroll = isAdmin && (status === 'online' || status === 'offline' || status === 'revoked');
  const showRevoke = isAdmin && status !== 'revoked';

  const errorLine = errorCode && (
    <p role="alert" className="text-sm text-[var(--danger)] m-0">
      {t(`players.actions.errors.${errorCode}`, { defaultValue: t('players.actions.errors.unknown') })}
    </p>
  );

  return (
    <>
      <div className="flex flex-wrap gap-2">
        {showMove && <ActionButton onClick={() => open('move')} icon={<ArrowRightLeft size={14} />} label={t('players.actions.move')} />}
        {showToken && <ActionButton onClick={() => open('token')} icon={<KeyRound size={14} />} label={t('players.actions.token')} />}
        {showReEnroll && <ActionButton onClick={() => open('reenroll')} icon={<RefreshCw size={14} />} label={t('players.actions.reenroll')} />}
        {showRevoke && <ActionButton danger onClick={() => open('revoke')} icon={<ShieldOff size={14} />} label={t('players.actions.revoke')} />}
        {isAdmin && <ActionButton danger onClick={() => open('delete')} icon={<Trash2 size={14} />} label={t('players.actions.delete')} />}
      </div>

      {mode === 'move' && (
        <Shell title={t('players.actions.moveTitle', { name: player.name })} pending={move.isPending} onClose={close}
               submitLabel={t('players.actions.moveSubmit')} submitDisabled={!target}
               onSubmit={async () => {
                 setErrorCode(null);
                 try { await move.mutateAsync({ id: player.id, groupId: Number(target) }); done('players.actions.moved'); }
                 catch (e) { fail(e); }
               }}>
          <Field id="pmove-group" label={t('players.actions.moveTo')} hint={isAdmin ? undefined : t('players.actions.moveRule')}>
            <select id="pmove-group" value={target} onChange={e => setTarget(e.target.value)} disabled={move.isPending} className={inputCls}>
              <option value="">{t('players.add.pickGroup')}</option>
              {groups.map(o => (
                <optgroup key={o.id} label={o.name}>
                  {o.groups.filter(g => g.id !== player.group_id).map(g => <option key={g.id} value={g.id}>{g.name}</option>)}
                </optgroup>
              ))}
            </select>
          </Field>
          {errorLine}
        </Shell>
      )}

      {mode === 'token' && (
        <Shell title={t('players.actions.tokenTitle', { name: player.name })} pending={reissueToken.isPending} onClose={close}
               submitLabel={t('players.actions.tokenSubmit')}
               onSubmit={async () => {
                 setErrorCode(null);
                 try { setGiven(await reissueToken.mutateAsync(player.id)); setMode(null); }
                 catch (e) { fail(e); }
               }}>
          <p className="text-sm text-[var(--text-primary)] m-0">{t('players.actions.tokenText')}</p>
          {errorLine}
        </Shell>
      )}

      {mode === 'reenroll' && (
        <Shell danger title={t('players.actions.reenrollTitle', { name: player.name })} pending={reEnroll.isPending} onClose={close}
               submitLabel={t('players.actions.reenrollSubmit')} submitDisabled={reasonProblem(reason)}
               onSubmit={async () => {
                 setErrorCode(null);
                 try { setGiven(await reEnroll.mutateAsync({ id: player.id, reason: reason.trim() || undefined })); setMode(null); }
                 catch (e) { fail(e); }
               }}>
          <p className="text-sm text-[var(--text-primary)] m-0">
            {status === 'revoked' ? t('players.actions.reenrollTextRevoked') : t('players.actions.reenrollText')}
          </p>
          <p className="text-sm text-[var(--text-muted)] m-0">{t('players.actions.reenrollKeeps')}</p>
          <TextField id="preenroll-reason" label={t('players.actions.reason')} hint={t('players.actions.reasonHint')}
                     value={reason} onChange={setReason}
                     error={reasonProblem(reason) ? t('players.actions.reasonBad') : null} disabled={reEnroll.isPending} />
          {errorLine}
        </Shell>
      )}

      {mode === 'revoke' && (
        <Shell danger title={t('players.actions.revokeTitle', { name: player.name })} pending={revoke.isPending} onClose={close}
               submitLabel={t('players.actions.revokeSubmit')} submitDisabled={reasonProblem(reason)}
               onSubmit={async () => {
                 setErrorCode(null);
                 try { await revoke.mutateAsync({ id: player.id, reason: reason.trim() || undefined }); done('players.actions.revoked'); }
                 catch (e) { fail(e); }
               }}>
          <p className="text-sm text-[var(--text-primary)] m-0">{t('players.actions.revokeText')}</p>
          <TextField id="prevoke-reason" label={t('players.actions.reason')} hint={t('players.actions.reasonHint')}
                     value={reason} onChange={setReason}
                     error={reasonProblem(reason) ? t('players.actions.reasonBad') : null} disabled={revoke.isPending} />
          {errorLine}
        </Shell>
      )}

      {mode === 'delete' && (
        <Shell danger title={t('players.actions.deleteTitle', { name: player.name })} pending={remove.isPending} onClose={close}
               submitLabel={t('players.actions.deleteSubmit')} submitDisabled={typed !== player.name}
               onSubmit={async () => {
                 setErrorCode(null);
                 try {
                   await remove.mutateAsync(player.id);
                   toast(t('players.actions.deleted'), 'success');
                   navigate('/players');
                 } catch (e) { fail(e); }
               }}>
          <p className="text-sm text-[var(--text-primary)] m-0">{t('players.actions.deleteText')}</p>
          <TextField id="pdelete-typed" label={t('players.actions.typeName', { name: player.name })}
                     value={typed} onChange={setTyped} disabled={remove.isPending} />
          {errorLine}
        </Shell>
      )}

      {given && (
        <PairingPackageDialog pkg={given} playerName={player.name}
                              onClose={() => { reissueToken.reset(); reEnroll.reset(); setGiven(null); }} />
      )}
    </>
  );
}
