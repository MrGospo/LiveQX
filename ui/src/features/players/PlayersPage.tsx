import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link, useNavigate, useSearchParams } from 'react-router-dom';
import { MonitorSmartphone, Building2, Layers, Search, Plus } from 'lucide-react';
import { clsx } from 'clsx';
import {
  usePlayers, usePlayerTree, PLAYER_PAGE_SIZE,
  type Player, type PlayerObject,
} from '@/api/queries/players';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { useNow } from '@/hooks/useNow';
import { useAuthStore } from '@/stores/auth';
import { PLAYERS_NAV } from './nav';
import { inputCls } from './fields';
import { relativeFromNow, absoluteTime } from './format';
import { AddPlayerDialog } from './AddPlayerDialog';
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
  const isAdmin = useAuthStore(s => s.role) === 'admin';
  const [adding, setAdding] = React.useState(false);

  const num = (k: string) => { const v = params.get(k); return v && /^\d+$/.test(v) ? Number(v) : undefined; };
  const selection: Selection = { objectId: num('object'), groupId: num('group') };
  const statusParam = params.get('status');
  const statusFilter = (STATUSES as string[]).includes(statusParam ?? '') ? (statusParam as ScreenStatus) : null;
  const query = params.get('q') ?? '';
  const pageParam = params.get('page');
  const page = pageParam && /^\d+$/.test(pageParam) && Number(pageParam) > 0 ? Number(pageParam) : 1;

  // Any change of what is shown starts again from the first page. Typing in the search
  // box replaces the address in place, so it does not pile up history.
  const patch = (changes: Record<string, string | null>, replace = false) => {
    const next = new URLSearchParams(params);
    for (const [k, v] of Object.entries(changes)) { if (v) next.set(k, v); else next.delete(k); }
    if (Object.keys(changes).some(k => k !== 'page')) next.delete('page');
    setParams(next, { replace });
  };
  const select = (s: Selection) =>
    patch({ object: s.objectId != null ? String(s.objectId) : null, group: s.groupId != null ? String(s.groupId) : null });

  // The box shows what is typed at once; the address (and so the request) follows a moment later.
  const [typed, setTyped] = React.useState(query);
  React.useEffect(() => { setTyped(query); }, [query]);
  React.useEffect(() => {
    if (typed === query) return;
    const id = setTimeout(() => patch({ q: typed || null }, true), 300);
    return () => clearTimeout(id);
  }, [typed]);                                         // eslint-disable-line react-hooks/exhaustive-deps

  const tree = usePlayerTree();
  const list = usePlayers({ ...selection, status: statusFilter ?? undefined, q: query.trim() || undefined, page });

  const rows = list.data?.players ?? [];
  const total = list.data?.total ?? 0;
  const counts = list.data?.counts ?? { online: 0, offline: 0, pending: 0, revoked: 0 };
  const everything = counts.online + counts.offline + counts.pending + counts.revoked;
  const filtered = !!statusFilter || !!query.trim();

  // A page past the end (the list shrank meanwhile): go back to the first one.
  React.useEffect(() => {
    if (list.data && rows.length === 0 && total > 0 && page > 1) patch({ page: null }, true);
  }, [list.data, rows.length, total, page]);           // eslint-disable-line react-hooks/exhaustive-deps

  const nowMs = useNow();
  const pill = (on: boolean) => clsx('px-2.5 py-1 text-xs rounded-full border',
    on ? 'border-[var(--accent)] text-[var(--accent)]'
       : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]');

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={PLAYERS_NAV} />

      <div className="flex items-center gap-3">
        <MonitorSmartphone size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{t('players.list.title')}</h1>
        {isAdmin && (
          <button type="button" onClick={() => setAdding(true)}
                  className="ml-auto inline-flex items-center gap-1.5 px-3 py-2 text-sm text-white rounded-md bg-[var(--accent)] hover:bg-[var(--accent-hover)]">
            <Plus size={14} /> {t('players.add.button')}
          </button>
        )}
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
              <input type="search" value={typed} aria-label={t('players.list.search')}
                     placeholder={t('players.list.search')}
                     onChange={e => setTyped(e.target.value)}
                     className={clsx(inputCls, 'pl-8 w-64')} />
            </div>
            <div role="group" aria-label={t('players.list.statusFilter')} className="flex flex-wrap gap-1">
              <button type="button" aria-pressed={!statusFilter} onClick={() => patch({ status: null })} className={pill(!statusFilter)}>
                {t('players.list.allStatuses')} · {everything}
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
          ) : !filtered && everything === 0 ? (
            <Block>
              <EmptyState Icon={MonitorSmartphone}
                          title={t('players.list.empty.title')} description={t('players.list.empty.text')} />
            </Block>
          ) : total === 0 ? (
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
                  {rows.map(p => (
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

          {total > PLAYER_PAGE_SIZE && (
            <nav aria-label={t('players.list.pages')} className="flex items-center gap-3 text-sm text-[var(--text-muted)]">
              <span>{t('players.list.range', { from: (page - 1) * PLAYER_PAGE_SIZE + 1, to: Math.min(page * PLAYER_PAGE_SIZE, total), total })}</span>
              <div className="ml-auto flex gap-2">
                <button type="button" disabled={page <= 1} onClick={() => patch({ page: page > 2 ? String(page - 1) : null })}
                        className="px-3 py-1.5 rounded-md border border-[var(--border-subtle)] disabled:opacity-40 hover:text-[var(--text-primary)]">
                  {t('players.list.prev')}
                </button>
                <button type="button" disabled={page * PLAYER_PAGE_SIZE >= total} onClick={() => patch({ page: String(page + 1) })}
                        className="px-3 py-1.5 rounded-md border border-[var(--border-subtle)] disabled:opacity-40 hover:text-[var(--text-primary)]">
                  {t('players.list.next')}
                </button>
              </div>
            </nav>
          )}
        </div>
      </div>

      {adding && <AddPlayerDialog objects={tree.data ?? []} initialGroupId={selection.groupId} onClose={() => setAdding(false)} />}
    </div>
  );
}
