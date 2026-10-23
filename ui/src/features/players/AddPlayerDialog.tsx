import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { X } from 'lucide-react';
import { usePlayerActions, type ClockPolicy, type PairingPackage, type PlayerObject } from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { useEscClose } from '@/hooks/useEscClose';
import { useDialogFocus } from '@/hooks/useDialogFocus';
import { Field, TextField, inputCls } from './fields';
import { nameProblem, trimName } from './names';
import { PairingPackageDialog } from './PairingPackageDialog';

// A new screen: a name, the group it stands in and (rarely) a clock check other
// than the module's default. The server answers with the pairing package, which
// is shown once in its own window.

type PolicyChoice = 'default' | ClockPolicy;

export function AddPlayerDialog({ objects, initialGroupId, onClose }: {
  objects: PlayerObject[]; initialGroupId?: number; onClose: () => void;
}) {
  const { t } = useTranslation();
  const { create } = usePlayerActions();
  const allGroups = objects.flatMap(o => o.groups);
  const [name, setName] = React.useState('');
  const [groupId, setGroupId] = React.useState<string>(
    initialGroupId != null && allGroups.some(g => g.id === initialGroupId) ? String(initialGroupId) : '');
  const [policy, setPolicy] = React.useState<PolicyChoice>('default');
  const [touched, setTouched] = React.useState(false);
  const [errorCode, setErrorCode] = React.useState<string | null>(null);
  const [given, setGiven] = React.useState<{ pkg: PairingPackage; name: string } | null>(null);
  useEscClose(onClose, !create.isPending && !given);
  const dialogRef = useDialogFocus<HTMLFormElement>();

  // Once the package window is closed nothing of it may stay in memory.
  const finish = () => { create.reset(); setGiven(null); onClose(); };

  if (given) return <PairingPackageDialog pkg={given.pkg} playerName={given.name} onClose={finish} />;

  const problem = nameProblem(name);
  const nameError = touched && problem ? t(`players.objects.nameErrors.${problem}`) : null;
  const groupError = touched && !groupId ? t('players.add.groupRequired') : null;

  const submit = async (e: React.FormEvent) => {
    e.preventDefault();
    setTouched(true);
    setErrorCode(null);
    if (problem || !groupId) return;
    const clean = trimName(name);
    try {
      const pkg = await create.mutateAsync({
        name: clean, group_id: Number(groupId), ...(policy !== 'default' ? { clock_policy: policy } : {}),
      });
      setGiven({ pkg, name: clean });
    } catch (err) {
      setErrorCode(err instanceof ApiClientError ? err.code : 'unknown');
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <form ref={dialogRef} onSubmit={submit} role="dialog" aria-modal="true" aria-label={t('players.add.title')}
            className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)] m-0">{t('players.add.title')}</h2>
          <button type="button" onClick={onClose} disabled={create.isPending} aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 flex flex-col gap-4">
          {allGroups.length === 0 ? (
            <p role="status" className="text-sm text-[var(--text-primary)] m-0">
              {t('players.add.noGroups')}{' '}
              <Link to="/players/objects" className="text-[var(--accent)] hover:underline">{t('players.add.toObjects')}</Link>
            </p>
          ) : (
            <>
              <TextField id="padd-name" label={t('players.add.name')} hint={t('players.add.nameHint')}
                         value={name} onChange={v => { setName(v); setTouched(true); }}
                         error={nameError} disabled={create.isPending} />
              <Field id="padd-group" label={t('players.add.group')} hint={t('players.add.groupHint')} error={groupError}>
                <select id="padd-group" value={groupId} onChange={e => setGroupId(e.target.value)}
                        disabled={create.isPending} aria-invalid={groupError ? true : undefined} className={inputCls}>
                  <option value="">{t('players.add.pickGroup')}</option>
                  {objects.filter(o => o.groups.length > 0).map(o => (
                    <optgroup key={o.id} label={o.name}>
                      {o.groups.map(g => <option key={g.id} value={g.id}>{g.name}</option>)}
                    </optgroup>
                  ))}
                </select>
              </Field>
              <Field id="padd-policy" label={t('players.add.policy')} hint={t('players.add.policyHint')}>
                <select id="padd-policy" value={policy} onChange={e => setPolicy(e.target.value as PolicyChoice)}
                        disabled={create.isPending} className={inputCls}>
                  <option value="default">{t('players.add.policyDefault')}</option>
                  <option value="strict">{t('players.detail.clockValue.strict')}</option>
                  <option value="relaxed">{t('players.detail.clockValue.relaxed')}</option>
                </select>
              </Field>
            </>
          )}

          {errorCode && (
            <p role="alert" className="text-sm text-[var(--danger)] m-0">
              {t(`players.add.errors.${errorCode}`, { defaultValue: t('players.add.errors.unknown') })}{' '}
              {(errorCode === 'not_configured' || errorCode === 'ca_unavailable') && (
                <Link to="/players/settings" className="text-[var(--accent)] hover:underline">{t('players.add.toSettings')}</Link>
              )}
            </p>
          )}
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button type="button" onClick={onClose} disabled={create.isPending}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30">
            {t('common.cancel')}
          </button>
          <button type="submit" disabled={create.isPending || allGroups.length === 0}
                  className="px-4 py-2 text-white text-sm rounded-md bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50">
            {create.isPending ? t('players.add.creating') : t('players.add.submit')}
          </button>
        </div>
      </form>
    </div>
  );
}
