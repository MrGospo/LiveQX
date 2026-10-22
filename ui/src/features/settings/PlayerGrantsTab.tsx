import React from 'react';
import { useTranslation } from 'react-i18next';
import { Building2, Layers } from 'lucide-react';
import {
  usePlayerTree, useUserPlayerGrants, useSaveUserPlayerGrants,
  type UserPlayerGrant,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { Block } from '@/components/Block';
import { useToast } from '@/hooks/useToast';

// Rights of one person on screens: which objects and groups they see, and what else
// they may do there. The server keeps the whole list as one piece (a save replaces
// it), so this page edits a draft and saves it once. Rights are read on every request
// on the server, so what is saved works at once; nothing here needs a re-login.

type Scope = 'object' | 'group';
type Marks = { can_command_group: boolean; can_command_object: boolean; can_move: boolean; can_view_audit: boolean };
type MarkKey = keyof Marks;
type Draft = Record<string, Marks>;            // "object:3" / "group:7" → its marks; a missing key = no access

const NO_MARKS: Marks = { can_command_group: false, can_command_object: false, can_move: false, can_view_audit: false };
const keyOf = (scope: Scope, id: number) => `${scope}:${id}`;

// A viewer only looks: commands and moves do nothing for them, so they are never shown as given or saved.
const ACTIVE_FOR_VIEWER: MarkKey[] = ['can_view_audit'];
const effective = (m: Marks, role: 'viewer' | 'operator'): Marks =>
  role === 'viewer' ? { ...NO_MARKS, can_view_audit: m.can_view_audit } : m;

// What the server holds, as it is (nothing hidden).
function draftFrom(grants: UserPlayerGrant[]): Draft {
  const d: Draft = {};
  for (const g of grants) {
    if (g.missing) continue;                    // its group or object is gone; a save drops it
    d[keyOf(g.scope, g.id)] = {
      can_command_group: g.can_command_group, can_command_object: g.can_command_object,
      can_move: g.can_move, can_view_audit: g.can_view_audit,
    };
  }
  return d;
}
const masked = (d: Draft, role: 'viewer' | 'operator'): Draft =>
  Object.fromEntries(Object.entries(d).map(([k, m]) => [k, effective(m, role)]));

// One list for comparing "was it changed" and for sending.
const canon = (d: Draft) => JSON.stringify(Object.keys(d).sort().map(k => [k, d[k]]));

function Check({ label, checked, disabled, title, onChange }: {
  label: string; checked: boolean; disabled?: boolean; title?: string; onChange: (v: boolean) => void;
}) {
  return (
    <input type="checkbox" aria-label={label} title={title} checked={checked} disabled={disabled}
           onChange={e => onChange(e.target.checked)}
           className="w-4 h-4 accent-[var(--accent)] disabled:opacity-40 disabled:cursor-not-allowed" />
  );
}

export function PlayerGrantsTab({ userId, role }: { userId: number; role: 'viewer' | 'operator' }) {
  const { t } = useTranslation();
  const toast = useToast();
  const tree = usePlayerTree();
  const server = useUserPlayerGrants(userId);
  const save = useSaveUserPlayerGrants(userId);

  // What the server holds, and the draft the page starts from. For a viewer the commands and
  // moves that may be stored are not shown as given (they do nothing), but they ARE a difference
  // from what is stored: a save removes them, so they cannot come alive if the role changes later.
  const stored = React.useMemo(() => (server.data ? draftFrom(server.data.grants) : {}), [server.data]);
  const baseline = React.useMemo(() => masked(stored, role), [stored, role]);
  const revision = server.data?.revision;
  const dropped = server.data ? server.data.grants.filter(g => g.missing).length : 0;
  const inertStored = role === 'viewer' && canon(baseline) !== canon(stored);

  // The draft starts from what the server holds and starts over when that changes (a save, a re-read).
  const [draft, setDraft] = React.useState<Draft>({});
  const [conflict, setConflict] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);
  React.useEffect(() => { setDraft(baseline); }, [baseline]);

  const dirty = canon(masked(draft, role)) !== canon(stored);

  const setAccess = (scope: Scope, id: number, on: boolean) => setDraft(d => {
    const next = { ...d };
    if (on) next[keyOf(scope, id)] = { ...NO_MARKS }; else delete next[keyOf(scope, id)];
    return next;
  });
  const setMark = (scope: Scope, id: number, mark: MarkKey, on: boolean) =>
    setDraft(d => ({ ...d, [keyOf(scope, id)]: { ...d[keyOf(scope, id)], [mark]: on } }));

  const submit = async () => {
    setConflict(false); setError(null);
    const grants = Object.keys(draft).map(k => {
      const [scope, id] = k.split(':');
      return { scope: scope as Scope, id: Number(id), ...effective(draft[k], role) };
    });
    try {
      await save.mutateAsync({ grants, revision });
      toast(t('users.playerGrants.saved'), 'success');
    } catch (e) {
      const code = e instanceof ApiClientError ? e.code : '';
      if (code === 'grants_changed') {
        // Somebody else changed these rights meanwhile: show theirs, do not overwrite them.
        setConflict(true);
        void server.refetch();
        void tree.refetch();
      } else {
        setError(t(`users.playerGrants.errors.${code}`, { defaultValue: t('users.playerGrants.errors.unknown') }));
        if (code === 'unknown_scope') { void server.refetch(); void tree.refetch(); }
      }
    }
  };

  if (tree.isLoading || server.isLoading) {
    return <div className="flex flex-col gap-3">{Array(4).fill(0).map((_, i) => <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}</div>;
  }
  if (tree.isError || server.isError || !server.data) {
    return <p role="alert" className="text-sm text-[var(--danger)]">{t('users.playerGrants.loadFailed')}</p>;
  }

  const objects = tree.data ?? [];
  const nothingGiven = Object.keys(draft).length === 0;
  const marks: { key: MarkKey; for: Scope[] }[] = [
    { key: 'can_command_group',  for: ['object', 'group'] },
    { key: 'can_command_object', for: ['object'] },
    { key: 'can_move',           for: ['object', 'group'] },
    { key: 'can_view_audit',     for: ['object', 'group'] },
  ];

  const row = (scope: Scope, id: number, name: string, extra?: React.ReactNode) => {
    const mine = draft[keyOf(scope, id)];
    return (
      <tr key={keyOf(scope, id)} className="border-b last:border-0 border-[var(--border-subtle)]">
        <td className={'px-4 py-2.5 text-[var(--text-primary)] ' + (scope === 'group' ? 'pl-10' : 'font-medium')}>
          <span className="inline-flex items-center gap-2">
            {scope === 'object' ? <Building2 size={14} className="text-[var(--text-muted)]" /> : <Layers size={14} className="text-[var(--text-muted)]" />}
            {name}
            {extra}
          </span>
        </td>
        <td className="px-3 py-2.5 text-center">
          <Check label={`${t('users.playerGrants.col.access')}: ${name}`} checked={!!mine}
                 onChange={v => setAccess(scope, id, v)} />
        </td>
        {marks.map(m => {
          const applies = m.for.includes(scope);
          const blockedByRole = role === 'viewer' && !ACTIVE_FOR_VIEWER.includes(m.key);
          return (
            <td key={m.key} className="px-3 py-2.5 text-center">
              {applies
                ? <Check label={`${t(`users.playerGrants.col.${m.key}`)}: ${name}`}
                         checked={!!mine && mine[m.key] && !blockedByRole}
                         disabled={!mine || blockedByRole}
                         title={blockedByRole ? t('users.playerGrants.viewerNote') : undefined}
                         onChange={v => setMark(scope, id, m.key, v)} />
                : <span className="text-[var(--text-muted)]" aria-hidden>—</span>}
            </td>
          );
        })}
      </tr>
    );
  };

  return (
    <div className="flex flex-col gap-4">
      <p className="text-sm text-[var(--text-muted)] m-0">{t('users.playerGrants.lead')}</p>

      {role === 'viewer' && (
        <p role="note" className="text-sm text-[var(--text-primary)] m-0 rounded-md border border-[var(--border-subtle)] bg-surface2 p-3">
          {t('users.playerGrants.viewerNote')}
        </p>
      )}
      {conflict && (
        <p role="alert" className="text-sm text-[var(--text-primary)] m-0 rounded-md border border-[var(--warning)] bg-[var(--warning)]/10 p-3">
          {t('users.playerGrants.conflict')}
        </p>
      )}
      {error && <p role="alert" className="text-sm text-[var(--danger)] m-0">{error}</p>}
      {inertStored && (
        <p role="status" className="text-xs text-[var(--text-muted)] m-0">{t('users.playerGrants.inertStored')}</p>
      )}
      {dropped > 0 && (
        <p role="status" className="text-xs text-[var(--text-muted)] m-0">{t('users.playerGrants.dropped', { count: dropped })}</p>
      )}

      {objects.length === 0 ? (
        <Block><p className="text-sm text-[var(--text-muted)] m-0">{t('users.playerGrants.noPlaces')}</p></Block>
      ) : (
        <Block padding="p-0">
          <div className="overflow-x-auto">
            <table className="w-full text-sm border-collapse">
              <thead>
                <tr className="border-b border-[var(--border-subtle)]">
                  <th className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{t('users.playerGrants.col.place')}</th>
                  <th className="px-3 py-2.5 text-center text-xs font-semibold text-[var(--text-muted)]" title={t('users.playerGrants.hint.access')}>{t('users.playerGrants.col.access')}</th>
                  {marks.map(m => (
                    <th key={m.key} className="px-3 py-2.5 text-center text-xs font-semibold text-[var(--text-muted)]" title={t(`users.playerGrants.hint.${m.key}`)}>
                      {t(`users.playerGrants.col.${m.key}`)}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {objects.flatMap(o => [
                  row('object', o.id, o.name),
                  ...o.groups.map(g => row('group', g.id, g.name,
                    draft[keyOf('object', o.id)] && !draft[keyOf('group', g.id)]
                      ? <span className="text-xs text-[var(--text-muted)]">{t('users.playerGrants.viaObject')}</span> : null)),
                ])}
              </tbody>
            </table>
          </div>
        </Block>
      )}

      {nothingGiven && objects.length > 0 && (
        <p role="status" className="text-sm text-[var(--warning)] m-0">{t('users.playerGrants.nothingGiven')}</p>
      )}

      <div className="flex gap-2">
        <button type="button" onClick={submit} disabled={!dirty || save.isPending}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {save.isPending ? t('common.loading') : t('common.save')}
        </button>
        <button type="button" onClick={() => { setDraft(baseline); setConflict(false); setError(null); }} disabled={!dirty || save.isPending}
                className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-40">
          {t('users.playerGrants.reset')}
        </button>
      </div>
    </div>
  );
}
