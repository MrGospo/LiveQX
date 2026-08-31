/**
 * AuditTrailPage — /settings/audit-trail
 *
 * Enterprise audit trail (state/audit.db). Distinct from /settings/audit,
 * which reads the legacy auth-only auth_audit table. This page shows
 * every server mutation (channels/outputs/gateways/plugins/mounts/system)
 * plus mirrored auth events, with HMAC-chain verification and live SSE
 * updates via the `audit_event` bus signal.
 *
 * Admin-only — enforced by RequireRole in routes.tsx (matches backend
 * RBAC rule `GET /api/audit/events → Admin`).
 */
import React from 'react';
import { useTranslation } from 'react-i18next';
import { Download, ShieldCheck, RefreshCw, AlertTriangle, Activity } from 'lucide-react';
import { format } from 'date-fns';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { useToast } from '@/hooks/useToast';
import { api } from '@/api/client';
import {
  useAuditTrail, useAuditTrailCategories, useAuditTrailStats,
  useAuditTrailReverify,
} from '@/api/queries/audit';
import type {
  AuditTrailEvent, AuditTrailFilter, AuditTrailListResponse,
} from '@/api/types';

const PAGE_SIZE = 100;

function csvEscape(value: unknown): string {
  if (value === null || value === undefined) return '';
  const s = typeof value === 'string' ? value : JSON.stringify(value);
  if (/[",\r\n]/.test(s)) return '"' + s.replace(/"/g, '""') + '"';
  return s;
}

function fmtMs(ms: number): string {
  if (!ms) return '—';
  return format(new Date(ms), 'dd.MM.yyyy HH:mm:ss');
}

// Colour coding for HTTP status: 2xx neutral, 4xx warning, 5xx danger.
function statusClass(s: number): string {
  if (s >= 500) return 'text-[var(--danger)]';
  if (s >= 400) return 'text-[var(--warning)]';
  if (s >= 200 && s < 300) return 'text-[var(--text-muted)]';
  return 'text-[var(--text-primary)]';
}

export default function AuditTrailPage() {
  const { t } = useTranslation();
  const toast = useToast();

  // ── Filter state ─────────────────────────────────────────────────────
  const [category, setCategory]     = React.useState('');
  const [action, setAction]         = React.useState('');
  const [actorName, setActorName]   = React.useState('');
  const [targetType, setTargetType] = React.useState('');
  const [targetId, setTargetId]     = React.useState('');
  const [offset, setOffset]         = React.useState(0);
  const [selected, setSelected]     = React.useState<AuditTrailEvent | null>(null);
  const [exporting, setExporting]   = React.useState(false);

  const filter: AuditTrailFilter = React.useMemo(() => ({
    category:       category       ? (category as AuditTrailFilter['category']) : undefined,
    action:         action         || undefined,
    actor_username: actorName      || undefined,
    target_type:    targetType     || undefined,
    target_id:      targetId       || undefined,
    limit:          PAGE_SIZE,
    offset,
  }), [category, action, actorName, targetType, targetId, offset]);

  const { data, isLoading, isFetching } = useAuditTrail(filter);
  const { data: categories = [] } = useAuditTrailCategories();
  const { data: stats } = useAuditTrailStats({ refetchInterval: 5000 });
  const { mutateAsync: reverify, isPending: verifying, data: verifyResult } =
    useAuditTrailReverify();

  // Reset pagination when the filter changes.
  React.useEffect(() => { setOffset(0); }, [category, action, actorName, targetType, targetId]);

  const events = data?.events ?? [];
  const total  = data?.total  ?? 0;
  const page   = Math.floor(offset / PAGE_SIZE) + 1;
  const pages  = Math.max(1, Math.ceil(total / PAGE_SIZE));

  // ── CSV export — client-side, paged through backend (max 1000/req) ──
  const handleExport = async () => {
    setExporting(true);
    try {
      const all: AuditTrailEvent[] = [];
      let cur = 0;
      const cap = 10_000;   // hard client-side cap
      while (all.length < cap) {
        const qs = new URLSearchParams();
        Object.entries(filter).forEach(([k, v]) => {
          if (v === undefined || v === null || v === '') return;
          if (k === 'limit' || k === 'offset') return;
          qs.set(k, String(v));
        });
        qs.set('limit', '1000');
        qs.set('offset', String(cur));
        const chunk = await api.get<AuditTrailListResponse>(
          `/api/audit/events?${qs}`);
        all.push(...chunk.events);
        if (chunk.events.length < 1000) break;
        cur += 1000;
      }
      const header = [
        'id', 'ts_ms', 'time', 'category', 'action', 'target_type', 'target_id',
        'http_method', 'http_path', 'http_status', 'elapsed_ms',
        'actor_user_id', 'actor_username', 'actor_role', 'actor_ip',
        'request_id', 'key_fingerprint', 'mac', 'summary', 'details',
      ];
      const lines = [header.join(',')];
      for (const ev of all) {
        const details = ev.details_raw ?? (ev.details != null ? JSON.stringify(ev.details) : '');
        lines.push([
          csvEscape(ev.id), csvEscape(ev.ts_ms),
          csvEscape(new Date(ev.ts_ms).toISOString()),
          csvEscape(ev.category), csvEscape(ev.action),
          csvEscape(ev.target_type), csvEscape(ev.target_id),
          csvEscape(ev.http_method), csvEscape(ev.http_path),
          csvEscape(ev.http_status), csvEscape(ev.elapsed_ms),
          csvEscape(ev.actor_user_id ?? ''), csvEscape(ev.actor_username),
          csvEscape(ev.actor_role), csvEscape(ev.actor_ip),
          csvEscape(ev.request_id), csvEscape(ev.key_fingerprint),
          csvEscape(ev.mac ?? ''), csvEscape(ev.summary),
          csvEscape(details),
        ].join(','));
      }
      const blob = new Blob(['\uFEFF' + lines.join('\r\n')],
                            { type: 'text/csv;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      a.href = url;
      a.download = `audit-trail-${stamp}.csv`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
      toast(t('auditTrail.exportedToast', { count: all.length }), 'success');
    } catch (err) {
      toast(String((err as Error)?.message ?? err), 'danger');
    } finally {
      setExporting(false);
    }
  };

  const handleReverify = async () => {
    try {
      const r = await reverify();
      if (r.ok) toast(t('auditTrail.verifyOk', { scanned: r.scanned }), 'success');
      else      toast(t('auditTrail.verifyBad',
                        { id: r.first_bad_id, reason: r.reason }), 'danger');
    } catch (err) {
      toast(String((err as Error)?.message ?? err), 'danger');
    }
  };

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />

      {/* ── Ops health strip ─────────────────────────────────────── */}
      {stats && (
        <div className="flex items-center gap-3 flex-wrap text-xs">
          <span className="flex items-center gap-1.5 px-2 py-1 rounded bg-surface2">
            <Activity size={12} />
            {t('auditTrail.statsQueue', { n: stats.queue_depth })}
          </span>
          <span className="px-2 py-1 rounded bg-surface2 text-[var(--text-muted)]">
            {t('auditTrail.statsWritten', { n: stats.written_db })}
          </span>
          {stats.written_emergency > 0 && (
            <span className="flex items-center gap-1.5 px-2 py-1 rounded bg-[var(--warning)]/10 text-[var(--warning)]">
              <AlertTriangle size={12} />
              {t('auditTrail.statsEmergency', { n: stats.written_emergency })}
            </span>
          )}
          {stats.fail_closed && (
            <span className="flex items-center gap-1.5 px-2 py-1 rounded bg-[var(--danger)]/10 text-[var(--danger)]">
              <AlertTriangle size={12} />
              {t('auditTrail.statsFailClosed')}
            </span>
          )}
          {!stats.db_ok && (
            <span className="flex items-center gap-1.5 px-2 py-1 rounded bg-[var(--danger)]/10 text-[var(--danger)]">
              <AlertTriangle size={12} /> {t('auditTrail.statsDbDown')}
            </span>
          )}
        </div>
      )}

      {/* ── Toolbar ──────────────────────────────────────────────── */}
      <div className="flex items-center justify-between gap-3 flex-wrap">
        <div className="flex gap-2 flex-wrap items-center">
          <select value={category} onChange={e => setCategory(e.target.value)}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
            <option value="">{t('auditTrail.allCategories')}</option>
            {categories.map(c =>
              <option key={c.name} value={c.name}>
                {c.name} ({c.retention_days}d)
              </option>)}
          </select>
          <input value={action} onChange={e => setAction(e.target.value)}
            placeholder={t('auditTrail.actionPlaceholder')}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] w-52" />
          <input value={actorName} onChange={e => setActorName(e.target.value)}
            placeholder={t('auditTrail.actorPlaceholder')}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] w-40" />
          <input value={targetType} onChange={e => setTargetType(e.target.value)}
            placeholder={t('auditTrail.targetTypePlaceholder')}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] w-32" />
          <input value={targetId} onChange={e => setTargetId(e.target.value)}
            placeholder={t('auditTrail.targetIdPlaceholder')}
            className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] w-32" />
        </div>
        <div className="flex gap-2 items-center">
          <button onClick={handleReverify} disabled={verifying}
            className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
            <ShieldCheck size={14} />
            {verifying ? t('auditTrail.verifying') : t('auditTrail.verify')}
          </button>
          <button onClick={handleExport} disabled={exporting}
            className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
            <Download size={14} />
            {exporting ? t('auditTrail.exporting') : t('auditTrail.exportCsv')}
          </button>
        </div>
      </div>

      {verifyResult && (
        <div className={verifyResult.ok
          ? 'p-3 rounded border border-[var(--success)]/30 bg-[var(--success)]/10 text-sm text-[var(--text-primary)]'
          : 'p-3 rounded border border-[var(--danger)]/30 bg-[var(--danger)]/10 text-sm text-[var(--text-primary)]'}>
          {verifyResult.ok
            ? t('auditTrail.verifyOk', { scanned: verifyResult.scanned })
            : t('auditTrail.verifyBad', {
                id: verifyResult.first_bad_id, reason: verifyResult.reason,
              })}
        </div>
      )}

      {/* ── Table ────────────────────────────────────────────────── */}
      <Block padding="p-0">
        {isLoading ? (
          <div className="p-5 flex flex-col gap-3">
            {Array(6).fill(0).map((_, i) =>
              <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}
          </div>
        ) : events.length === 0 ? (
          <EmptyState
            Icon={Activity}
            title={t('auditTrail.emptyTitle')}
            description={t('auditTrail.emptyMsg')} />
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {[
                  t('auditTrail.colTime'), t('auditTrail.colCategory'),
                  t('auditTrail.colAction'), t('auditTrail.colTarget'),
                  t('auditTrail.colActor'), t('auditTrail.colIp'),
                  t('auditTrail.colStatus'),
                ].map(h =>
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">
                    {h}
                  </th>)}
              </tr>
            </thead>
            <tbody>
              {events.map(ev => (
                <tr key={ev.id}
                    onClick={() => setSelected(ev)}
                    className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 cursor-pointer transition-colors">
                  <td className="px-4 py-2.5 text-xs text-[var(--text-muted)] tabular-nums whitespace-nowrap">
                    {fmtMs(ev.ts_ms)}
                  </td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--accent)]">
                    {ev.category}
                  </td>
                  <td className="px-4 py-2.5 font-mono text-xs">
                    {ev.action}
                  </td>
                  <td className="px-4 py-2.5 text-xs text-[var(--text-muted)]">
                    {ev.target_type
                      ? `${ev.target_type}/${ev.target_id || '—'}`
                      : '—'}
                  </td>
                  <td className="px-4 py-2.5 text-sm text-[var(--text-primary)]">
                    {ev.actor_username || (
                      <span className="text-[var(--text-muted)]">
                        {t('auditTrail.actorSystem')}
                      </span>
                    )}
                  </td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)]">
                    {ev.actor_ip || '—'}
                  </td>
                  <td className={'px-4 py-2.5 font-mono text-xs tabular-nums ' + statusClass(ev.http_status)}>
                    {ev.http_status || '—'}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Block>

      {/* ── Pager ────────────────────────────────────────────────── */}
      {total > PAGE_SIZE && (
        <div className="flex items-center justify-between text-xs text-[var(--text-muted)]">
          <span>{t('auditTrail.pagerSummary', { page, pages, total })}</span>
          <div className="flex gap-2">
            <button disabled={offset <= 0 || isFetching}
              onClick={() => setOffset(Math.max(0, offset - PAGE_SIZE))}
              className="px-3 py-1.5 border border-[var(--border-subtle)] rounded-md disabled:opacity-40">
              {t('common.back')}
            </button>
            <button disabled={offset + PAGE_SIZE >= total || isFetching}
              onClick={() => setOffset(offset + PAGE_SIZE)}
              className="px-3 py-1.5 border border-[var(--border-subtle)] rounded-md disabled:opacity-40">
              {t('common.next')}
            </button>
          </div>
        </div>
      )}

      {isFetching && !isLoading && (
        <div className="flex items-center gap-1.5 text-xs text-[var(--text-muted)]">
          <RefreshCw size={12} className="animate-spin" />
          {t('auditTrail.refreshing')}
        </div>
      )}

      {/* ── Detail drawer ───────────────────────────────────────── */}
      {selected && (
        <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-5"
             onClick={() => setSelected(null)}>
          <div onClick={e => e.stopPropagation()}
               className="w-full max-w-3xl max-h-[85vh] overflow-y-auto bg-surface border border-[var(--border-subtle)] rounded-xl p-6 flex flex-col gap-4">
            <div className="flex items-center justify-between">
              <h2 className="text-lg font-semibold text-[var(--text-primary)]">
                #{selected.id} · {selected.category} · {selected.action}
              </h2>
              <button onClick={() => setSelected(null)}
                      className="text-[var(--text-muted)] hover:text-[var(--text-primary)]">
                ✕
              </button>
            </div>
            <div className="grid grid-cols-2 gap-x-6 gap-y-2 text-xs">
              <div><span className="text-[var(--text-muted)]">{t('auditTrail.colTime')}:</span> {fmtMs(selected.ts_ms)}</div>
              <div><span className="text-[var(--text-muted)]">{t('auditTrail.colActor')}:</span> {selected.actor_username || '—'} ({selected.actor_role || '—'})</div>
              <div><span className="text-[var(--text-muted)]">{t('auditTrail.colIp')}:</span> {selected.actor_ip || '—'}</div>
              <div><span className="text-[var(--text-muted)]">HTTP:</span> {selected.http_method} {selected.http_path} → {selected.http_status} ({selected.elapsed_ms}ms)</div>
              <div><span className="text-[var(--text-muted)]">{t('auditTrail.colTarget')}:</span> {selected.target_type || '—'}/{selected.target_id || '—'}</div>
              <div><span className="text-[var(--text-muted)]">request_id:</span> <code className="font-mono">{selected.request_id || '—'}</code></div>
              <div className="col-span-2">
                <span className="text-[var(--text-muted)]">key_fingerprint:</span>{' '}
                <code className="font-mono">{selected.key_fingerprint || '—'}</code>
              </div>
              {selected.mac && (
                <div className="col-span-2">
                  <span className="text-[var(--text-muted)]">mac:</span>{' '}
                  <code className="font-mono break-all">{selected.mac}</code>
                </div>
              )}
            </div>
            {selected.summary && (
              <div className="text-sm text-[var(--text-primary)]">{selected.summary}</div>
            )}
            {(selected.details || selected.details_raw) && (
              <pre className="text-xs bg-canvas border border-[var(--border-subtle)] rounded p-3 overflow-x-auto font-mono text-[var(--text-primary)]">
                {selected.details_raw
                  ?? JSON.stringify(selected.details, null, 2)}
              </pre>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
