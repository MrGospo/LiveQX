import React from 'react';
import { useTranslation } from 'react-i18next';
import { Building2, Layers, Plus, Pencil, Trash2, X } from 'lucide-react';
import {
  usePlayerTree, usePlayerTreeMutations,
  type PlayerObject, type PlayerGroup,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { Block } from '@/components/Block';
import { ConfirmModal } from '@/components/ConfirmModal';
import { EmptyState } from '@/components/EmptyState';
import { useEscClose } from '@/hooks/useEscClose';
import { useDialogFocus } from '@/hooks/useDialogFocus';
import { useToast } from '@/hooks/useToast';
import { PlayersSubNav } from './PlayersSubNav';
import { TextField } from './fields';
import { nameProblem, trimName } from './names';

// Objects (sites) and the groups inside them. A screen belongs to one group, a
// group to one object; rights and commands are given by these places. Only an
// administrator edits the tree. A place with something inside cannot be deleted:
// the buttons say so up front, and the server refuses the same way if the page
// was out of date.

type Dialog =
  | { kind: 'createObject' }
  | { kind: 'renameObject'; object: PlayerObject }
  | { kind: 'createGroup'; object: PlayerObject }
  | { kind: 'renameGroup'; group: PlayerGroup; objectName: string }
  | { kind: 'deleteObject'; object: PlayerObject }
  | { kind: 'deleteGroup'; group: PlayerGroup; objectName: string };

interface NameDialogProps {
  title: string;
  label: string;
  initial: string;
  submitLabel: string;
  pending: boolean;
  serverError: string | null;
  onSubmit: (name: string) => void;
  onCancel: () => void;
}

function NameDialog({ title, label, initial, submitLabel, pending, serverError, onSubmit, onCancel }: NameDialogProps) {
  const { t } = useTranslation();
  const [value, setValue] = React.useState(initial);
  const [touched, setTouched] = React.useState(false);
  useEscClose(onCancel, !pending);
  const dialogRef = useDialogFocus<HTMLFormElement>();

  const problem = nameProblem(value);
  const error = serverError ?? (touched && problem ? t(`players.objects.nameErrors.${problem}`) : null);

  const submit = (e: React.FormEvent) => {
    e.preventDefault();
    setTouched(true);
    if (problem) return;
    onSubmit(trimName(value));
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <form ref={dialogRef} onSubmit={submit} role="dialog" aria-modal="true" aria-label={title}
            className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)] m-0">{title}</h2>
          <button type="button" onClick={onCancel} disabled={pending} aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30">
            <X size={18} />
          </button>
        </div>
        <div className="p-5">
          <TextField id="pobj-name" label={label} value={value}
                     onChange={v => { setValue(v); setTouched(true); }}
                     error={error} disabled={pending} />
        </div>
        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button type="button" onClick={onCancel} disabled={pending}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30">
            {t('common.cancel')}
          </button>
          <button type="submit" disabled={pending}
                  className="px-4 py-2 text-white text-sm rounded-md bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50">
            {submitLabel}
          </button>
        </div>
      </form>
    </div>
  );
}

function IconButton({ label, onClick, disabled, danger, children }: {
  label: string; onClick: () => void; disabled?: boolean; danger?: boolean; children: React.ReactNode;
}) {
  return (
    <button type="button" aria-label={label} title={label} onClick={onClick} disabled={disabled}
            className={'p-1.5 rounded-md border border-transparent transition-colors disabled:opacity-30 disabled:cursor-not-allowed ' +
                       (danger
                         ? 'text-[var(--text-muted)] hover:text-[var(--danger)] hover:border-[var(--border-subtle)]'
                         : 'text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:border-[var(--border-subtle)]')}>
      {children}
    </button>
  );
}

export default function ObjectsPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { data, isLoading, isError } = usePlayerTree();
  const m = usePlayerTreeMutations();
  const [dialog, setDialog] = React.useState<Dialog | null>(null);
  const [serverError, setServerError] = React.useState<string | null>(null);

  const close = () => { setDialog(null); setServerError(null); };
  const open = (d: Dialog) => { setServerError(null); setDialog(d); };

  // A refusal about the name stays in the dialog; anything else is a toast and the dialog closes.
  const run = async (op: () => Promise<unknown>, doneKey: string, keepOpenOn: string[] = []) => {
    try {
      await op();
      toast(t(doneKey), 'success');
      close();
    } catch (e) {
      const code = e instanceof ApiClientError ? e.code : '';
      if (keepOpenOn.includes(code)) {
        setServerError(t(`players.objects.serverErrors.${code}`));
        return;
      }
      toast(code ? t(`players.objects.serverErrors.${code}`, { defaultValue: code }) : (e instanceof Error ? e.message : String(e)), 'danger');
      close();
    }
  };

  const nameErrors = ['name_taken', 'invalid_name'];

  if (isLoading) {
    return <div className="p-7"><div className="h-64 bg-surface2 rounded-xl animate-pulse" /></div>;
  }
  if (isError || !data) {
    return (
      <div className="p-7 flex flex-col gap-5">
        <PlayersSubNav />
        <p role="alert" className="text-sm text-[var(--danger)]">{t('players.objects.loadFailed')}</p>
      </div>
    );
  }

  return (
    <div className="p-7 flex flex-col gap-5">
      <PlayersSubNav />

      <div className="flex items-center gap-3">
        <Building2 size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{t('players.objects.title')}</h1>
        <button type="button" onClick={() => open({ kind: 'createObject' })}
                className="ml-auto inline-flex items-center gap-1.5 px-3 py-2 text-sm text-white rounded-md bg-[var(--accent)] hover:bg-[var(--accent-hover)]">
          <Plus size={14} /> {t('players.objects.addObject')}
        </button>
      </div>
      <p className="text-sm text-[var(--text-muted)] -mt-3 mb-0">{t('players.objects.lead')}</p>

      {data.length === 0 ? (
        <Block>
          <EmptyState Icon={Building2} title={t('players.objects.empty.title')} description={t('players.objects.empty.text')} />
        </Block>
      ) : data.map(o => (
        <Block key={o.id}>
          <div className="flex items-center gap-2 mb-3">
            <Building2 size={16} className="text-[var(--text-muted)]" />
            <h2 className="text-base font-semibold text-[var(--text-primary)] m-0">{o.name}</h2>
            <div className="ml-auto flex items-center gap-1">
              <IconButton label={t('players.objects.renameObject', { name: o.name })}
                          onClick={() => open({ kind: 'renameObject', object: o })}>
                <Pencil size={14} />
              </IconButton>
              <IconButton danger disabled={o.groups.length > 0}
                          label={o.groups.length > 0
                            ? t('players.objects.deleteObjectBlocked', { name: o.name })
                            : t('players.objects.deleteObject', { name: o.name })}
                          onClick={() => open({ kind: 'deleteObject', object: o })}>
                <Trash2 size={14} />
              </IconButton>
            </div>
          </div>

          {o.groups.length === 0 ? (
            <p className="text-sm text-[var(--text-muted)] m-0 mb-3">{t('players.objects.noGroups')}</p>
          ) : (
            <ul className="list-none m-0 mb-3 p-0 flex flex-col divide-y divide-[var(--border-subtle)] border border-[var(--border-subtle)] rounded-md">
              {o.groups.map(g => (
                <li key={g.id} className="flex items-center gap-2 px-3 py-2">
                  <Layers size={14} className="text-[var(--text-muted)]" />
                  <span className="text-sm text-[var(--text-primary)]">{g.name}</span>
                  <span className="text-xs text-[var(--text-muted)]">{t('players.objects.playersCount', { count: g.players })}</span>
                  <div className="ml-auto flex items-center gap-1">
                    <IconButton label={t('players.objects.renameGroup', { name: g.name })}
                                onClick={() => open({ kind: 'renameGroup', group: g, objectName: o.name })}>
                      <Pencil size={14} />
                    </IconButton>
                    <IconButton danger disabled={g.players > 0}
                                label={g.players > 0
                                  ? t('players.objects.deleteGroupBlocked', { name: g.name })
                                  : t('players.objects.deleteGroup', { name: g.name })}
                                onClick={() => open({ kind: 'deleteGroup', group: g, objectName: o.name })}>
                      <Trash2 size={14} />
                    </IconButton>
                  </div>
                </li>
              ))}
            </ul>
          )}

          <button type="button" onClick={() => open({ kind: 'createGroup', object: o })}
                  className="inline-flex items-center gap-1.5 px-3 py-1.5 text-sm text-[var(--text-primary)] border border-[var(--border-subtle)] rounded-md hover:bg-surface2">
            <Plus size={14} /> {t('players.objects.addGroup')}
          </button>
        </Block>
      ))}

      {dialog?.kind === 'createObject' && (
        <NameDialog title={t('players.objects.dialog.createObject')} label={t('players.objects.dialog.objectName')}
                    initial="" submitLabel={t('players.objects.dialog.create')} pending={m.createObject.isPending}
                    serverError={serverError} onCancel={close}
                    onSubmit={name => run(() => m.createObject.mutateAsync(name), 'players.objects.done.objectCreated', nameErrors)} />
      )}
      {dialog?.kind === 'renameObject' && (
        <NameDialog title={t('players.objects.dialog.renameObject')} label={t('players.objects.dialog.objectName')}
                    initial={dialog.object.name} submitLabel={t('common.save')} pending={m.renameObject.isPending}
                    serverError={serverError} onCancel={close}
                    onSubmit={name => run(() => m.renameObject.mutateAsync({ id: dialog.object.id, name }), 'players.objects.done.renamed', nameErrors)} />
      )}
      {dialog?.kind === 'createGroup' && (
        <NameDialog title={t('players.objects.dialog.createGroup', { object: dialog.object.name })} label={t('players.objects.dialog.groupName')}
                    initial="" submitLabel={t('players.objects.dialog.create')} pending={m.createGroup.isPending}
                    serverError={serverError} onCancel={close}
                    onSubmit={name => run(() => m.createGroup.mutateAsync({ objectId: dialog.object.id, name }), 'players.objects.done.groupCreated', nameErrors)} />
      )}
      {dialog?.kind === 'renameGroup' && (
        <NameDialog title={t('players.objects.dialog.renameGroup', { object: dialog.objectName })} label={t('players.objects.dialog.groupName')}
                    initial={dialog.group.name} submitLabel={t('common.save')} pending={m.renameGroup.isPending}
                    serverError={serverError} onCancel={close}
                    onSubmit={name => run(() => m.renameGroup.mutateAsync({ id: dialog.group.id, name }), 'players.objects.done.renamed', nameErrors)} />
      )}
      {dialog?.kind === 'deleteObject' && (
        <ConfirmModal danger title={t('players.objects.confirm.deleteObjectTitle')}
                      message={t('players.objects.confirm.deleteObject', { name: dialog.object.name })}
                      confirmLabel={t('common.delete')} pending={m.deleteObject.isPending}
                      onCancel={close}
                      onConfirm={() => run(() => m.deleteObject.mutateAsync(dialog.object.id), 'players.objects.done.deleted')} />
      )}
      {dialog?.kind === 'deleteGroup' && (
        <ConfirmModal danger title={t('players.objects.confirm.deleteGroupTitle')}
                      message={t('players.objects.confirm.deleteGroup', { name: dialog.group.name, object: dialog.objectName })}
                      confirmLabel={t('common.delete')} pending={m.deleteGroup.isPending}
                      onCancel={close}
                      onConfirm={() => run(() => m.deleteGroup.mutateAsync(dialog.group.id), 'players.objects.done.deleted')} />
      )}
    </div>
  );
}
