import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { MonitorSmartphone, Building2, Layers, Search } from 'lucide-react';
import { clsx } from 'clsx';
import {
  usePlayers, usePlayerTree, PLAYER_LIST_LIMIT,
  type Player, type PlayerObject,
} from '@/api/queries/players';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { PLAYERS_NAV } from './nav';
import { inputCls } from './fields';
import { relativeFromNow, absoluteTime } from './format';
import { StatusBadge, screenStatus, type ScreenStatus } from './status';

// The list of screens. On the left the tree "all → object → group" picks the
// place; on the right the screens of that place. The place, the status filter
// and the search live in the address, so a link to a filtered list can be sent
// and the Back button works. What a person may see is decided by the server:
// the tree and the list already arrive cut to their rights.

const STATUSES: ScreenStatus[] = ['online', 'offline', 'pending', 'revoked'];

interface Selection { objectId?: number; groupId?: number }

function TreeButton({ active, depth, icon, label, count, onClick }: {
  active: boolean; depth: 0 | 1 | 2; icon?: React.ReactNode; label: string; count?: number; onClick: () => void;
}) {
  return (
    <button type="button" onClick={onClick} aria-current={active ? 'true' : undefined}
            className={clsx(
              'flex items-center gap-2 w-full text-left rounded-md py-1.5 pr-2 text-sm transition-colors',
              depth === 2 ? 'pl-7' : 'pl-2',
              active ? 'bg-[var(--accent)]/15 text-[var(--accent)]' : 'text-[var(--text-primary)] hover:bg-surface2')}>
      {icon}
      <span className="truncate">{label}</span>
      {count != null && <span className="ml-auto text-xs text-[var(--text-muted)]">{count}</span>}
    </button>
  );
}

function Tree({ objects, selection, onSelect, allLabel }: {
  objects: PlayerObject[]; selection: Selection; onSelect: (s: Selection) => void; allLabel: string;
}) {
  const none = selection.objectId == null && selection.groupId == null;
  return (
    <nav aria-label={allLabel} className="flex flex-col gap-0.5">
      <TreeButton active={none} depth={0} icon={<MonitorSmartphone size={14} className="shrink-0" />}
                  label={allLabel} onClick={() => onSelect({})} />
      {objects.map(o => (
        <React.Fragment key={o.id}>
          <TreeButton active={selection.objectId === o.id && selection.groupId == null} depth={1}
                      icon={<Building2 size={14} className="shrink-0" />} label={o.name}
                      count={o.groups.reduce((n, g) => n + g.players, 0)}
                      onClick={() => onSelect({ objectId: o.id })} />
          {o.groups.map(g => (
            <TreeButton key={g.id} active={selection.groupId === g.id} depth={2}
                        icon={<Layers size={14} className="shrink-0" />} label={g.name} count={g.players}
                        onClick={() => onSelect({ groupId: g.id })} />
          ))}
        </React.Fragment>
      ))}
    </nav>
  );
}

const placeText = (p: Player) =>
  p.group_id == null ? '' : [p.object_name, p.group_name].filter(Boolean).join(' / ');

export default function PlayersPage() {
  const { t, i18n } = useTranslation();
  const navigate = useNavigate();
  const [params, setParams] = useSearchParams();

  const num = (k: string) => { const v = params.get(k); return v && /^\d+$/.test(v) ? Number(v) : undefined; };
  const selection: Selection = { objectId: num('object'), groupId: num('group') };
  const statusParam = params.get('status');
  const statusFilter = (STATUSES as string[]).includes(statusParam ?? '') ? (statusParam as ScreenStatus) : null;
  const query = params.get('q') ?? '';

  // Typing in the search box replaces the address in place, so it does not pile up history.
  const patch = (changes: Record<string, string | null>, replace = false) => {
    const next = new URLSearchParams(params);
    for (const [k, v] of Object.entries(changes)) { if (v) next.set(k, v); else next.delete(k); }
    setParams(next, { replace });
  };
  const select = (s: Selection) =>
    patch({ object: s.objectId != null ? String(s.objectId) : null, group: s.groupId != null ? String(s.groupId) : null });

  const tree = usePlayerTree();
  const list = usePlayers(selection);

  const all = React.useMemo(() => list.data ?? [], [list.data]);
  const counts = React.useMemo(() => {
    const c: Record<ScreenStatus, number> = { online: 0, offline: 0, pending: 0, revoked: 0 };
    for (const p of all) c[screenStatus(p)]++;
    return c;
  }, [all]);

  const shown = React.useMemo(() => {
    const q = query.trim().toLowerCase();
    return all
      .filter(p => !statusFilter || screenStatus(p) === statusFilter)
      .filter(p => !q || p.name.toLowerCase().includes(q) || placeText(p).toLowerCase().includes(q))
      .sort((a, b) => a.name.localeCompare(b.name, i18n.language));
  }, [all, statusFilter, query, i18n.language]);

  const nowMs = Date.now();
  const pill = (on: boolean) => clsx('px-2.5 py-1 text-xs rounded-full border',
    on ? 'border-[var(--accent)] text-[var(--accent)]'
       : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]');

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={PLAYERS_NAV} />

      <div className="flex items-center gap-3">
        <MonitorSmartphone size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{t('players.list.title')}</h1>
      </div>

      <div className="grid gap-5 md:grid-cols-[16rem_1fr] items-start">
        <Block padding="p-3">
          {tree.isLoading
            ? <div className="h-32 bg-surface2 rounded-md animate-pulse" />
            : tree.isError
              ? <p role="alert" className="text-sm text-[var(--danger)] m-0 p-2">{t('players.list.treeFailed')}</p>
              : <Tree objects={tree.data ?? []} selection={selection} onSelect={select} allLabel={t('players.list.all')} />}
        </Block>

        <div className="flex flex-col gap-4 min-w-0">
          <div className="flex flex-wrap items-center gap-2">
            <div className="relative">
              <Search size={14} className="absolute left-3 top-1/2 -translate-y-1/2 text-[var(--text-muted)]" />
              <input type="search" value={query} aria-label={t('players.list.search')}
                     placeholder={t('players.list.search')}
                     onChange={e => patch({ q: e.target.value }, true)}
                     className={clsx(inputCls, 'pl-8 w-64')} />
            </div>
            <div role="group" aria-label={t('players.list.statusFilter')} className="flex flex-wrap gap-1">
              <button type="button" aria-pressed={!statusFilter} onClick={() => patch({ status: null })} className={pill(!statusFilter)}>
                {t('players.list.allStatuses')} · {all.length}
              </button>
              {STATUSES.map(s => (
                <button key={s} type="button" aria-pressed={statusFilter === s} onClick={() => patch({ status: s })} className={pill(statusFilter === s)}>
                  {t(`players.status.${s}`)} · {counts[s]}
                </button>
              ))}
            </div>
          </div>

          {list.isLoading ? (
            <div className="h-64 bg-surface2 rounded-xl animate-pulse" />
          ) : list.isError ? (
            <p role="alert" className="text-sm text-[var(--danger)]">{t('players.list.loadFailed')}</p>
          ) : all.length === 0 ? (
            <Block>
              <EmptyState Icon={MonitorSmartphone}
                          title={t('players.list.empty.title')} description={t('players.list.empty.text')} />
            </Block>
          ) : shown.length === 0 ? (
            <Block>
              <EmptyState Icon={Search} title={t('players.list.noMatch.title')} description={t('players.list.noMatch.text')} />
            </Block>
          ) : (
            <Block padding="p-0">
              <table className="w-full text-sm">
                <thead>
                  <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
                    <th className="font-medium px-4 py-2.5">{t('players.list.col.name')}</th>
                    <th className="font-medium px-4 py-2.5">{t('players.list.col.place')}</th>
                    <th className="font-medium px-4 py-2.5">{t('players.list.col.status')}</th>
                    <th className="font-medium px-4 py-2.5">{t('players.list.col.version')}</th>
                    <th className="font-medium px-4 py-2.5">{t('players.list.col.lastSignal')}</th>
                  </tr>
                </thead>
                <tbody>
                  {shown.map(p => (
                    <tr key={p.id} onClick={() => navigate(`/players/${p.id}`)}
                        className="border-b border-[var(--border-subtle)] last:border-b-0 hover:bg-surface2 cursor-pointer">
                      <td className="px-4 py-2.5">
                        <Link to={`/players/${p.id}`} onClick={e => e.stopPropagation()}
                              className="text-[var(--text-primary)] hover:text-[var(--accent)] font-medium">
                          {p.name}
                        </Link>
                      </td>
                      <td className="px-4 py-2.5 text-[var(--text-muted)]">{placeText(p) || t('players.list.noPlace')}</td>
                      <td className="px-4 py-2.5"><StatusBadge status={screenStatus(p)} /></td>
                      <td className="px-4 py-2.5 text-[var(--text-muted)] font-mono text-xs">{p.status?.version || '—'}</td>
                      <td className="px-4 py-2.5 text-[var(--text-muted)]"
                          title={p.status ? absoluteTime(p.status.updated_at) : undefined}>
                        {p.status ? relativeFromNow(p.status.updated_at, nowMs, i18n.language) : t('players.list.never')}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </Block>
          )}

          {all.length >= PLAYER_LIST_LIMIT && (
            <p role="status" className="text-xs text-[var(--warning)] m-0">
              {t('players.list.cut', { n: PLAYER_LIST_LIMIT })}
            </p>
          )}
        </div>
      </div>
    </div>
  );
}
