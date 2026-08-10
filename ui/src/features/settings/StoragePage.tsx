import React from 'react';
import { useTranslation } from 'react-i18next';
import { HardDrive, Plus, RefreshCw, Pencil, Trash2, Folder, X, Check, AlertTriangle } from 'lucide-react';
import {
  useMounts, useCreateMount, useUpdateMount,
  useDeleteMount, useTestMount, useSyncMount,
} from '@/api/queries/mounts';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { ConfirmModal } from '@/components/ConfirmModal';
import { RowActionsMenu, type RowAction } from '@/components/RowActionsMenu';
import { FolderPickerModal } from '@/components/FolderPickerModal';
import { SETTINGS_NAV } from './nav';
import { fmtTime } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import type { MountPublic, MountSpec } from '@/api/types';
import { useEscClose } from '@/hooks/useEscClose';

// fix41 — список и CRUD сетевых mount'ов под /mnt/liveqx/.
// Active state приходит из фонового sync'а на backend'е (раз в 30s);
// auto-refetch на этой странице тоже 30s. Кнопка sync на строке —
// единичный pull для случая «починили на хосте, хотим обновить UI
// прямо сейчас».

const defaultSpec = (): MountSpec => ({
  fs_type: 'cifs',
  source:  '',
  target:  '/mnt/liveqx/',
  options: '',
  ro: true,
  enabled: true,
  cifs: { username: '', password: '' },
});

const stateColor: Record<string, string> = {
  active:     'text-[var(--success)]',
  activating: 'text-[var(--info)]',
  inactive:   'text-[var(--text-muted)]',
  failed:     'text-[var(--danger)]',
  unknown:    'text-[var(--text-muted)]',
};

export default function StoragePage() {
  const { t } = useTranslation();
  const toast = useToast();

  const { data: mounts = [], isLoading } = useMounts();
  const createMut = useCreateMount();
  const updateMut = useUpdateMount();
  const deleteMut = useDeleteMount();
  const syncMut   = useSyncMount();

  const [editing,    setEditing]    = React.useState<MountPublic | null>(null);
  const [creating,   setCreating]   = React.useState(false);
  const [deleting,   setDeleting]   = React.useState<MountPublic | null>(null);

  const handleSync = (m: MountPublic) => {
    syncMut.mutate(m.id, {
      onSuccess: () => toast(t('storage.synced', { target: m.target }), 'success'),
      onError:   (err: any) => toast(err?.message ?? String(err), 'danger'),
    });
  };

  const handleDelete = () => {
    if (!deleting) return;
    const target = deleting;
    deleteMut.mutate(target.id, {
      onSuccess: () => {
        toast(t('storage.deleted', { target: target.target }), 'success');
        setDeleting(null);
      },
      onError: (err: any) => {
        toast(err?.message ?? String(err), 'danger');
        setDeleting(null);
      },
    });
  };

  const buildActions = (m: MountPublic): RowAction[] => ([
    { key: 'edit',
      label: t('storage.edit'),
      icon: <Pencil size={14} />,
      onClick: () => setEditing(m) },
    { key: 'sync',
      label: t('storage.sync'),
      icon: <RefreshCw size={14} />,
      onClick: () => handleSync(m) },
    { key: 'delete',
      label: t('storage.delete'),
      icon: <Trash2 size={14} />,
      danger: true,
      onClick: () => setDeleting(m) },
  ]);

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />
      <div className="flex items-center justify-end">
        <button onClick={() => setCreating(true)}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
          <Plus size={14} /> {t('storage.create')}
        </button>
      </div>

      <Block padding="p-0">
        {isLoading ? (
          <div className="p-5 flex flex-col gap-3">
            {Array(3).fill(0).map((_, i) => (
              <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />
            ))}
          </div>
        ) : mounts.length === 0 ? (
          <EmptyState Icon={HardDrive}
            title={t('storage.noMounts')}
            description={t('storage.noMountsHint')} />
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {[
                  t('storage.colTarget'),
                  t('storage.colType'),
                  t('storage.colSource'),
                  t('storage.colState'),
                  t('storage.colCreds'),
                  t('storage.colUpdated'),
                  '',
                ].map(h => (
                  <th key={h}
                    className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">
                    {h}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {mounts.map(m => {
                const state = m.active_state || 'unknown';
                return (
                  <tr key={m.id}
                    className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 transition-colors">
                    <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-primary)]">{m.target}</td>
                    <td className="px-4 py-2.5">
                      <span className="text-xs uppercase font-semibold text-[var(--text-muted)]">
                        {m.fs_type}
                      </span>
                      {m.ro && <span className="ml-2 text-[10px] uppercase text-[var(--info)]">ro</span>}
                    </td>
                    <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)] truncate max-w-[260px]">
                      {m.source}
                    </td>
                    <td className="px-4 py-2.5">
                      <span className={`text-xs font-medium ${stateColor[state] ?? stateColor.unknown}`}>
                        {t(`storage.state.${state}`, { defaultValue: state })}
                      </span>
                    </td>
                    <td className="px-4 py-2.5 text-xs text-[var(--text-muted)]">
                      {m.fs_type === 'cifs'
                        ? (m.has_password
                            ? `${m.cifs_username ?? ''} ★`
                            : (m.cifs_username ?? t('storage.guest')))
                        : '—'}
                    </td>
                    <td className="px-4 py-2.5 text-[var(--text-muted)] text-xs">
                      {fmtTime(m.updated_at)}
                    </td>
                    <td className="px-4 py-2.5 text-right">
                      <RowActionsMenu items={buildActions(m)} />
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Block>

      {creating && (
        <MountFormModal
          mode="create"
          initial={defaultSpec()}
          pending={createMut.isPending}
          onCancel={() => setCreating(false)}
          onSubmit={async (spec) => {
            try {
              await createMut.mutateAsync(spec);
              toast(t('storage.created', { target: spec.target }), 'success');
              setCreating(false);
            } catch (err: any) {
              toast(err?.message ?? String(err), 'danger');
            }
          }}
        />
      )}

      {editing && (
        <MountFormModal
          mode="edit"
          initial={publicToSpec(editing)}
          pending={updateMut.isPending}
          onCancel={() => setEditing(null)}
          onSubmit={async (spec) => {
            try {
              await updateMut.mutateAsync({ id: editing.id, body: spec });
              toast(t('storage.updated', { target: spec.target }), 'success');
              setEditing(null);
            } catch (err: any) {
              toast(err?.message ?? String(err), 'danger');
            }
          }}
        />
      )}

      {deleting && (
        <ConfirmModal
          danger
          title={t('storage.deleteTitle')}
          message={t('storage.deleteConfirm', { target: deleting.target })}
          confirmLabel={t('storage.delete')}
          pendingLabel={t('storage.deleting')}
          pending={deleteMut.isPending}
          onConfirm={handleDelete}
          onCancel={() => { if (!deleteMut.isPending) setDeleting(null); }}
        />
      )}
    </div>
  );
}

// ─── helpers ─────────────────────────────────────────────────────────────────

function publicToSpec(m: MountPublic): MountSpec {
  return {
    id: m.id,
    fs_type: m.fs_type,
    source:  m.source,
    target:  m.target,
    options: m.options,
    ro:      m.ro,
    enabled: m.enabled,
    cifs: m.fs_type === 'cifs' ? {
      username: m.cifs_username ?? '',
      domain:   m.cifs_domain,
      // password омитим — backend сохранит существующий blob.
    } : undefined,
  };
}

// ─── form modal ──────────────────────────────────────────────────────────────

interface FormProps {
  mode:     'create' | 'edit';
  initial:  MountSpec;
  pending:  boolean;
  onSubmit: (spec: MountSpec) => void | Promise<void>;
  onCancel: () => void;
}

function MountFormModal({ mode, initial, pending, onSubmit, onCancel }: FormProps) {
  const { t } = useTranslation();
  const toast = useToast();
  const testMut = useTestMount();

  const [spec, setSpec] = React.useState<MountSpec>(initial);
  const [pickFolder, setPickFolder] = React.useState(false);
  const [testResult, setTestResult] =
    React.useState<{ ok: boolean; msg: string } | null>(null);

  const upd = <K extends keyof MountSpec>(k: K, v: MountSpec[K]) =>
    setSpec(p => ({ ...p, [k]: v }));

  const updCifs = (k: 'username' | 'password' | 'domain', v: string) =>
    setSpec(p => ({ ...p, cifs: { ...(p.cifs ?? { username: '', password: '' }), [k]: v } }));

  const onTypeChange = (v: 'cifs' | 'nfs') => {
    setSpec(p => ({
      ...p,
      fs_type: v,
      cifs: v === 'cifs' ? (p.cifs ?? { username: '', password: '' }) : undefined,
    }));
  };

  const handleTest = async () => {
    setTestResult(null);
    try {
      const r = await testMut.mutateAsync(spec);
      setTestResult({ ok: r.ok, msg: r.ok
        ? t('storage.testOk')
        : (r.error ?? t('storage.testFailed')) });
    } catch (err: any) {
      setTestResult({ ok: false, msg: err?.message ?? String(err) });
    }
  };

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!spec.source.trim() || !spec.target.trim()) {
      toast(t('storage.fieldsRequired'), 'danger');
      return;
    }
    if (spec.fs_type === 'cifs' && !(spec.cifs?.username ?? '').trim()) {
      toast(t('storage.usernameRequired'), 'danger');
      return;
    }
    await onSubmit(spec);
  };

  useEscClose(onCancel, !pending);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <form onSubmit={handleSubmit}
        className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-2xl shadow-2xl flex flex-col"
        style={{ maxHeight: '90vh' }}>
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">
            {mode === 'create' ? t('storage.createTitle') : t('storage.editTitle')}
          </h2>
          <button type="button" onClick={onCancel} disabled={pending}
            aria-label="close"
            className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 flex flex-col gap-4 overflow-y-auto">
          {/* fs_type */}
          <div>
            <label className="text-xs uppercase tracking-wider text-[var(--text-muted)] block mb-1.5">
              {t('storage.fsType')}
            </label>
            <div className="flex gap-2">
              {(['cifs', 'nfs'] as const).map(v => (
                <label key={v}
                  className={`flex items-center gap-2 px-3 py-1.5 rounded-md border cursor-pointer text-sm transition-colors ${
                    spec.fs_type === v
                      ? 'border-[var(--accent)] bg-canvas text-[var(--text-primary)]'
                      : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'}`}>
                  <input type="radio" className="hidden"
                    checked={spec.fs_type === v}
                    onChange={() => onTypeChange(v)} />
                  <span className="uppercase">{v}</span>
                </label>
              ))}
            </div>
          </div>

          {/* source */}
          <div>
            <label className="text-xs uppercase tracking-wider text-[var(--text-muted)] block mb-1.5">
              {t('storage.source')}
            </label>
            <input value={spec.source}
              onChange={e => upd('source', e.target.value)}
              placeholder={spec.fs_type === 'cifs' ? '//host/share' : 'host:/export/path'}
              className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
          </div>

          {/* target — picker reuses FolderPickerModal */}
          <div>
            <label className="text-xs uppercase tracking-wider text-[var(--text-muted)] block mb-1.5">
              {t('storage.target')}
            </label>
            <div className="flex gap-2">
              <input value={spec.target}
                onChange={e => upd('target', e.target.value)}
                placeholder="/mnt/liveqx/lib1"
                className="flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
              <button type="button"
                onClick={() => setPickFolder(true)}
                className="px-3 py-2 border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors"
                aria-label="browse">
                <Folder size={14} />
              </button>
            </div>
          </div>

          {/* options */}
          <div>
            <label className="text-xs uppercase tracking-wider text-[var(--text-muted)] block mb-1.5">
              {t('storage.options')}
            </label>
            <input value={spec.options}
              onChange={e => upd('options', e.target.value)}
              placeholder={spec.fs_type === 'cifs' ? 'vers=3.0,iocharset=utf8' : 'vers=4.1'}
              className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
            <p className="mt-1 text-xs text-[var(--text-muted)]">{t('storage.optionsHint')}</p>
          </div>

          <label className="inline-flex items-center gap-2 text-sm text-[var(--text-primary)]">
            <input type="checkbox" checked={spec.ro}
              onChange={e => upd('ro', e.target.checked)} />
            {t('storage.readOnly')}
          </label>

          {/* CIFS creds */}
          {spec.fs_type === 'cifs' && (
            <div className="border border-[var(--border-subtle)] rounded-md p-4 flex flex-col gap-3">
              <h3 className="text-xs uppercase tracking-wider text-[var(--text-muted)]">
                {t('storage.cifsCreds')}
              </h3>
              <div>
                <label className="text-xs text-[var(--text-muted)] block mb-1">{t('storage.username')}</label>
                <input value={spec.cifs?.username ?? ''}
                  onChange={e => updCifs('username', e.target.value)}
                  className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
              </div>
              <div>
                <label className="text-xs text-[var(--text-muted)] block mb-1">{t('storage.password')}</label>
                <input type="password"
                  value={spec.cifs?.password ?? ''}
                  onChange={e => updCifs('password', e.target.value)}
                  placeholder={mode === 'edit' ? t('storage.passwordKeepHint') : ''}
                  className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
              </div>
              <div>
                <label className="text-xs text-[var(--text-muted)] block mb-1">{t('storage.domain')}</label>
                <input value={spec.cifs?.domain ?? ''}
                  onChange={e => updCifs('domain', e.target.value)}
                  className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
              </div>
            </div>
          )}

          {testResult && (
            <div className={`flex items-start gap-2 text-xs ${testResult.ok ? 'text-[var(--success)]' : 'text-[var(--danger)]'}`}>
              {testResult.ok
                ? <Check size={14} className="mt-0.5 shrink-0" />
                : <AlertTriangle size={14} className="mt-0.5 shrink-0" />}
              <span>{testResult.msg}</span>
            </div>
          )}
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button type="button"
            onClick={handleTest}
            disabled={testMut.isPending || pending}
            className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-30 transition-colors">
            {testMut.isPending ? t('storage.testing') : t('storage.test')}
          </button>
          <div className="flex-1" />
          <button type="button"
            onClick={onCancel}
            disabled={pending}
            className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-30 transition-colors">
            {t('common.cancel')}
          </button>
          <button type="submit"
            disabled={pending}
            className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
            {pending ? t('common.saving') : t('common.save')}
          </button>
        </div>

        {pickFolder && (
          <FolderPickerModal
            initialPath={spec.target || '/mnt/liveqx/'}
            onSelect={(p) => { upd('target', p); setPickFolder(false); }}
            onCancel={() => setPickFolder(false)}
          />
        )}
      </form>
    </div>
  );
}
