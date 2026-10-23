import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { ArrowRight, ScrollText } from 'lucide-react';
import { useAuditTrail } from '@/api/queries/audit';
import { usePlayerTree } from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import type { AuditTrailEvent } from '@/api/types';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { useAuthStore } from '@/stores/auth';
import { PlayersSubNav } from './PlayersSubNav';
import { PlaceFilter, type PlaceValue } from './PlaceFilter';
import { placeIndex, placeText } from './places';

// The journal of what was done about screens, by place. An administrator reads every place;
// anyone else reads only the places whose journal an administrator has opened to them, and the
// server cuts the list (and its count) to exactly that. A screen moved from one place to another
// is one row that shows both ends.

const PAGE = 50;

const detailsOf = (ev: AuditTrailEvent): Record<string, unknown> => {
  if (ev.details && typeof ev.details === 'object') return ev.details as Record<string, unknown>;
  if (ev.details_raw) { try { const v: unknown = JSON.parse(ev.details_raw); if (v && typeof v === 'object') return v as Record<string, unknown>; } catch { /* not JSON */ } }
  return {};
};

const text = (v: unknown) => (typeof v === 'string' ? v : '');

export default function PlayersAuditPage() {
  const { t } = useTranslation();
  const isAdmin = useAuthStore(s => s.role) === 'admin';
  const tree = usePlayerTree();
  const [place, setPlace] = React.useState<PlaceValue>({});
  const [offset, setOffset] = React.useState(0);

  const { data, isLoading, isError, error } = useAuditTrail({
    category: 'player', object_id: place.objectId, group_id: place.groupId, limit: PAGE, offset,
  });

  const idx = React.useMemo(() => placeIndex(tree.data), [tree.data]);
  const other = t('players.audit.otherPlace');
  const events = data?.events ?? [];
  const total = data?.total ?? 0;
  const forbidden = error instanceof ApiClientError && error.status === 403;

  const where = (ev: AuditTrailEvent) => {
    const to = placeText(idx, ev.object_id, ev.group_id, other);
    const moved = ev.from_object_id != null || ev.from_group_id != null;
    if (!moved) return <>{to || '—'}</>;
    const from = placeText(idx, ev.from_object_id, ev.from_group_id, other);
    return (
      <span className="inline-flex items-center gap-1.5 flex-wrap">
        <span>{from}</span>
        <ArrowRight size={12} className="text-[var(--text-muted)]" aria-label={t('players.audit.to')} />
        <span>{to}</span>
      </span>
    );
  };

  const subject = (ev: AuditTrailEvent) => {
    const d = detailsOf(ev);
    return text(d.name) || text(d.target) || ev.summary || '';
  };

  return (
    <div className="p-7 flex flex-col gap-5">
      <PlayersSubNav />
      <div className="flex items-center gap-3">
        <ScrollText size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{t('players.audit.title')}</h1>
        {isAdmin && (
          <Link to="/settings/audit-trail" className="ml-auto text-sm text-[var(--accent)] hover:underline">{t('players.audit.fullJournal')}</Link>
        )}
      </div>
      <p className="text-sm text-[var(--text-muted)] -mt-3 mb-0">{isAdmin ? t('players.audit.leadAdmin') : t('players.audit.lead')}</p>

      {!forbidden && (
        <PlaceFilter objects={tree.data ?? []} value={place} onlyAudit={!isAdmin}
                     onChange={v => { setPlace(v); setOffset(0); }} />
      )}

      {isLoading ? (
        <div className="h-48 bg-surface2 rounded-xl animate-pulse" />
      ) : forbidden ? (
        <p role="alert" className="text-sm text-[var(--text-primary)] m-0 rounded-md border border-[var(--border-subtle)] bg-surface2 p-3">{t('players.audit.noRight')}</p>
      ) : isError || !data ? (
        <p role="alert" className="text-sm text-[var(--danger)]">{t('players.audit.loadFailed')}</p>
      ) : events.length === 0 ? (
        <Block><EmptyState Icon={ScrollText} title={t('players.audit.empty.title')} description={t('players.audit.empty.text')} /></Block>
      ) : (
        <Block padding="p-0">
          <table className="w-full text-sm">
            <thead>
              <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
                <th className="font-medium px-4 py-2.5">{t('players.audit.col.time')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.audit.col.who')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.audit.col.what')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.audit.col.about')}</th>
                <th className="font-medium px-4 py-2.5">{t('players.audit.col.where')}</th>
              </tr>
            </thead>
            <tbody>
              {events.map(ev => (
                <tr key={ev.id} className="border-b border-[var(--border-subtle)] last:border-b-0">
                  <td className="px-4 py-2.5 text-[var(--text-muted)] whitespace-nowrap">{new Date(ev.ts_ms).toLocaleString()}</td>
                  <td className="px-4 py-2.5 text-[var(--text-primary)]">{ev.actor_username || '—'}</td>
                  <td className="px-4 py-2.5 text-[var(--text-primary)]">
                    {t(`players.audit.action.${ev.action.replace(/\./g, '_')}`, { defaultValue: ev.action })}
                  </td>
                  <td className="px-4 py-2.5 text-[var(--text-primary)]">
                    {ev.target_type === 'player' && ev.target_id
                      ? <Link to={`/players/${ev.target_id}`} className="hover:text-[var(--accent)]">{subject(ev) || ev.target_id.slice(0, 8)}</Link>
                      : (subject(ev) || '—')}
                  </td>
                  <td className="px-4 py-2.5 text-[var(--text-muted)]">{where(ev)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </Block>
      )}

      {total > PAGE && (
        <nav aria-label={t('players.list.pages')} className="flex items-center gap-3 text-sm text-[var(--text-muted)]">
          <span>{t('players.list.range', { from: offset + 1, to: Math.min(offset + PAGE, total), total })}</span>
          <div className="ml-auto flex gap-2">
            <button type="button" disabled={offset === 0} onClick={() => setOffset(Math.max(0, offset - PAGE))}
                    className="px-3 py-1.5 rounded-md border border-[var(--border-subtle)] disabled:opacity-40 hover:text-[var(--text-primary)]">{t('players.list.prev')}</button>
            <button type="button" disabled={offset + PAGE >= total} onClick={() => setOffset(offset + PAGE)}
                    className="px-3 py-1.5 rounded-md border border-[var(--border-subtle)] disabled:opacity-40 hover:text-[var(--text-primary)]">{t('players.list.next')}</button>
          </div>
        </nav>
      )}
    </div>
  );
}
