import React from 'react';
import { useTranslation } from 'react-i18next';
import { useAuditEvents, usePurgeAudit } from '@/api/queries/auth';
import { api } from '@/api/client';
import type { AuditEvent } from '@/api/types';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { fmtDate } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import { Download, Trash2 } from 'lucide-react';

// CSV-escape: оборачиваем в "..." если есть запятая, перевод строки или
// сама кавычка; внутренние " удваиваются (RFC 4180).
function csvEscape(value: unknown): string {
  if (value === null || value === undefined) return '';
  const s = typeof value === 'string' ? value : JSON.stringify(value);
  if (/[",\r\n]/.test(s)) return '"' + s.replace(/"/g, '""') + '"';
  return s;
}

export default function AuditPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const [eventFilter, setEventFilter] = React.useState('');
  const [purgeDays, setPurgeDays] = React.useState(90);
  const [showPurge, setShowPurge] = React.useState(false);
  const [exporting, setExporting] = React.useState(false);

  const { data: events = [], isLoading } = useAuditEvents({ event: eventFilter || undefined, limit: 100 });
  const { mutateAsync: purge, isPending: purging } = usePurgeAudit();

  // ── CSV export ────────────────────────────────────────────────────────────
  // Бэкенд-эндпоинта /api/auth/audit/export нет — делаем client-side
  // через одноразовый GET с расширенным limit и Blob-download.
  const handleExport = async () => {
    setExporting(true);
    try {
      const qs = new URLSearchParams();
      if (eventFilter) qs.set('event', eventFilter);
      qs.set('limit', '10000');  // backend max — 1000 на запрос, но возьмём с запасом; backend сам clamp'нёт
      const r = await api.get<{ events: AuditEvent[] }>(`/api/auth/audit?${qs}`);
      const rows = r.events ?? [];
      const header = ['id', 'ts', 'time', 'event', 'user_id', 'username', 'ip', 'details'];
      const lines = [header.join(',')];
      for (const ev of rows) {
        const time = new Date(ev.ts * 1000).toISOString();
        const details = ev.details_raw ?? (ev.details != null ? JSON.stringify(ev.details) : '');
        lines.push([
          csvEscape(ev.id),
          csvEscape(ev.ts),
          csvEscape(time),
          csvEscape(ev.event),
          csvEscape(ev.user_id ?? ''),
          csvEscape(ev.username ?? ''),
          csvEscape(ev.ip ?? ''),
          csvEscape(details),
        ].join(','));
      }
      const blob = new Blob(['\uFEFF' + lines.join('\r\n')], { type: 'text/csv;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
      a.href = url;
      a.download = `audit-${stamp}.csv`;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
      toast(t('audit.exportedToast', { count: rows.length }), 'success');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    } finally {
      setExporting(false);
    }
  };

  // Canonical list from AuthService (Q4 — hardcoded until openapi.yaml gets AuditEventCode enum in fix25 c16)
  const KNOWN_EVENTS = [
    'login.ok', 'login.fail', 'login.locked', 'logout',
    'password.change', 'password.reset.admin',
    'user.create', 'user.update', 'user.delete', 'user.enable', 'user.disable',
    'user.unlock', 'user.grants.set',
    'ldap.bind', 'ldap.config.update', 'ldap.test',
    'smtp.config.update', 'smtp.test',
    'plugin.install', 'plugin.uninstall', 'plugin.eula_accept', 'plugin.toggle',
    'master_key.rotate',
  ];

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />
      <div className="flex items-center justify-end gap-3 flex-wrap">
        <button onClick={handleExport} disabled={exporting}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
          <Download size={14} /> {exporting ? t('audit.exporting') : t('audit.exportCsv')}
        </button>
        <button onClick={() => setShowPurge(true)}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--danger)]/10 border border-[var(--danger)]/30 text-[var(--danger)] rounded-md hover:bg-[var(--danger)]/20 transition-colors">
          <Trash2 size={14} /> {t('audit.purge')}
        </button>
      </div>

      <div className="flex gap-3">
        <select value={eventFilter} onChange={e => setEventFilter(e.target.value)}
          className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
          <option value="">{t('audit.allEvents')}</option>
          {KNOWN_EVENTS.map(e => <option key={e} value={e}>{e}</option>)}
        </select>
      </div>

      <Block padding="p-0">
        {isLoading ? (
          <div className="p-5 flex flex-col gap-3">{Array(5).fill(0).map((_,i) => <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}</div>
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead><tr className="border-b border-[var(--border-subtle)]">
              {[t('audit.colTime'), t('audit.colEvent'), t('audit.colUser'), t('audit.colIp'), t('audit.colDetails')].map(h => <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>)}
            </tr></thead>
            <tbody>
              {events.map(ev => (
                <tr key={ev.id} className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 transition-colors">
                  <td className="px-4 py-2.5 text-xs text-[var(--text-muted)] tabular-nums whitespace-nowrap">{fmtDate(ev.ts)}</td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--accent)]">{ev.event}</td>
                  <td className="px-4 py-2.5 text-sm text-[var(--text-primary)]">{ev.username || <span className="text-[var(--text-muted)]">{t('audit.userSystem')}</span>}</td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)]">{ev.ip}</td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)] max-w-xs truncate">{ev.details_raw ?? (ev.details != null ? JSON.stringify(ev.details) : '—')}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Block>

      {/* Purge modal */}
      {showPurge && (
        <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-5">
          <div className="w-full max-w-md bg-surface border border-[var(--border-subtle)] rounded-xl p-7 flex flex-col gap-5">
            <h2 className="text-xl font-semibold text-[var(--text-primary)]">{t('audit.purgeTitle')}</h2>
            <div className="p-3 bg-[var(--warning)]/10 border border-[var(--warning)]/30 rounded-lg text-sm text-[var(--text-primary)]">
              {t('audit.purgeWarning')}
            </div>
            <div className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t('audit.purgeOlderThan')}</label>
              <input type="number" value={purgeDays} onChange={e => setPurgeDays(Number(e.target.value))}
                className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm w-32 text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
            </div>
            <div className="flex gap-2 justify-end">
              <button onClick={() => setShowPurge(false)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">{t('common.cancel')}</button>
              <button disabled={purging} onClick={() => purge(purgeDays).then(r => { setShowPurge(false); toast(t('audit.purgedToast', { count: r.removed }), 'warning'); })}
                className="px-4 py-2 bg-[var(--danger)] hover:bg-[var(--danger)]/80 text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {purging ? t('audit.purging') : t('audit.purgeAction')}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
