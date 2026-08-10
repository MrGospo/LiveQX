import React, { useRef, useState } from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, Download, CheckCircle, XCircle, MoreHorizontal, Trash2 } from 'lucide-react';
import { useStressReport, useDeleteStressReport } from '@/api/queries/operations';
import { Block } from '@/components/Block';
import { HealthBadge } from '@/components/HealthBadge';
import { fmtDate } from '@/lib/format';

export default function StressReportPage() {
  const { reportId } = useParams<{ reportId: string }>();
  const { t } = useTranslation();
  const navigate = useNavigate();
  const [menuOpen, setMenuOpen] = useState(false);
  const menuRef = useRef<HTMLDivElement>(null);
  const deleteReport = useDeleteStressReport();

  React.useEffect(() => {
    if (!menuOpen) return;
    const handler = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) setMenuOpen(false);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [menuOpen]);

  const { data: report, isLoading } = useStressReport(reportId ?? '');

  if (isLoading) return (
    <div className="p-7 flex flex-col gap-4">
      {Array(4).fill(0).map((_,i) => <div key={i} className="h-24 bg-surface2 rounded-xl animate-pulse" />)}
    </div>
  );
  if (!report) return <div className="p-7 text-[var(--text-muted)]">{t('stress.reportNotFound')}</div>;

  const isPassed = report.pass;
  const durationSec = report.duration_sec;
  const totalFramesDropped = report.per_channel.reduce((a, c) => a + c.frames_dropped, 0);

  const downloadJson = () => {
    const blob = new Blob([JSON.stringify(report, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a'); a.href = url; a.download = `stress-report-${reportId}.json`;
    a.click(); URL.revokeObjectURL(url);
  };

  return (
    <div className="p-7 flex flex-col gap-5 max-w-4xl">
      <button onClick={() => navigate('/operations/stress')}
        className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
        <ChevronLeft size={15} /> {t('operations.stress')}
      </button>

      {/* Header */}
      <div className="flex items-start gap-4 flex-wrap">
        <div className="flex-1">
          <div className="flex items-center gap-3 flex-wrap">
            <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('stress.reportTitle')}</h1>
            {isPassed
              ? <span className="flex items-center gap-1.5 text-[var(--success)] text-sm font-semibold bg-[var(--success)]/10 px-3 py-1 rounded-full"><CheckCircle size={14} /> PASS</span>
              : <span className="flex items-center gap-1.5 text-[var(--danger)] text-sm font-semibold bg-[var(--danger)]/10 px-3 py-1 rounded-full"><XCircle size={14} /> FAIL</span>
            }
            <span className="font-mono text-xs text-[var(--text-muted)]">{report.verdict}</span>
          </div>
          <p className="text-sm text-[var(--text-muted)] mt-1 font-mono">{reportId}</p>
        </div>
        <div className="flex items-center gap-2">
          <button onClick={downloadJson}
            className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
            <Download size={14} /> {t('stress.downloadJson')}
          </button>
          <div className="relative" ref={menuRef}>
            <button onClick={() => setMenuOpen(o => !o)}
              className="flex items-center justify-center w-8 h-8 border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
              <MoreHorizontal size={16} />
            </button>
            {menuOpen && (
              <div className="absolute right-0 top-full mt-1 z-50 min-w-[140px] bg-[var(--bg-canvas)] border border-[var(--border-subtle)] rounded-lg shadow-lg py-1">
                <button
                  onClick={() => {
                    setMenuOpen(false);
                    deleteReport.mutate(reportId ?? '', {
                      onSuccess: () => navigate('/operations/stress'),
                    });
                  }}
                  disabled={deleteReport.isPending}
                  className="flex items-center gap-2 w-full px-3 py-2 text-sm text-[var(--danger)] hover:bg-[var(--danger)]/10 transition-colors disabled:opacity-50">
                  <Trash2 size={14} /> {t('common.delete')}
                </button>
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Summary */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('stress.summary')}</h2>
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
          {([
            ['Started',     fmtDate(Math.floor(report.started_at_ms / 1000))],
            ['Ended',       fmtDate(Math.floor(report.ended_at_ms / 1000))],
            ['Duration',    `${durationSec}s`],
            ['Requested',   report.channels_requested],
            ['Started',     report.channels_started],
            ['Alive at end',report.channels_alive_at_end],
            ['Crashes',     report.crashes],
            ['Total drops', totalFramesDropped.toLocaleString()],
          ] as Array<[string, React.ReactNode]>).map(([k,v]) => (
            <div key={k}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
              <dd className="text-[var(--text-primary)] tabular-nums">{v}</dd>
            </div>
          ))}
        </div>
      </Block>

      {/* Health metrics */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('stress.healthMetrics') ?? 'Health metrics'}</h2>
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
          {([
            ['Worst fps drop',   `${report.worst_fps_drop_pct.toFixed(1)}%`],
            ['Mem growth',       `${report.memory_growth_pct_per_hour.toFixed(1)}%/h`],
            ['RSS start',        `${(report.rss_kb_start / 1024).toFixed(1)} MB`],
            ['RSS end',          `${(report.rss_kb_end / 1024).toFixed(1)} MB`],
          ] as Array<[string, React.ReactNode]>).map(([k,v]) => (
            <div key={k}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
              <dd className="text-[var(--text-primary)] tabular-nums">{v}</dd>
            </div>
          ))}
        </div>
      </Block>

      {/* Fail reasons */}
      {report.fail_reasons.length > 0 && (
        <Block>
          <h2 className="text-base font-semibold mb-4 text-[var(--danger)]">{t('stress.failReasons') ?? 'Fail reasons'}</h2>
          <ul className="flex flex-col gap-1.5">
            {report.fail_reasons.map((r, i) => (
              <li key={i} className="text-sm font-mono text-[var(--danger)]">• {r}</li>
            ))}
          </ul>
        </Block>
      )}

      {/* Per-channel breakdown */}
      {report.per_channel.length > 0 && (
        <Block padding="p-0">
          <div className="px-5 py-3 border-b border-[var(--border-subtle)]">
            <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('stress.perChannel')}</h2>
          </div>
          <div className="overflow-x-auto">
            <table className="w-full text-sm border-collapse">
              <thead>
                <tr className="border-b border-[var(--border-subtle)]">
                  {['Channel','Alive','Frames','Drops','Actual fps','Expected fps','Δ%'].map(h => (
                    <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {report.per_channel.map((ch, i) => (
                  <tr key={i} className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 transition-colors">
                    <td className="px-4 py-2.5 font-mono text-xs">{ch.id}</td>
                    <td className="px-4 py-2.5"><HealthBadge status={ch.alive ? 'ok' : 'fail'} small /></td>
                    <td className="px-4 py-2.5 tabular-nums">{ch.frames_rendered.toLocaleString()}</td>
                    <td className="px-4 py-2.5 tabular-nums text-[var(--warning)]">{ch.frames_dropped.toLocaleString()}</td>
                    <td className="px-4 py-2.5 tabular-nums">{ch.actual_fps.toFixed(2)}</td>
                    <td className="px-4 py-2.5 tabular-nums text-[var(--text-muted)]">{ch.expected_fps.toFixed(2)}</td>
                    <td className="px-4 py-2.5 tabular-nums">{ch.fps_drop_pct.toFixed(1)}%</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </Block>
      )}

      {/* Scenario events */}
      {report.scenario_events.length > 0 && (
        <Block padding="p-0">
          <div className="px-5 py-3 border-b border-[var(--border-subtle)]">
            <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('stress.scenarioEvents') ?? 'Scenario events'}</h2>
          </div>
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {['t','Scenario','Detail','OK'].map(h => (
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {report.scenario_events.map((e, i) => {
                const tRel = ((e.ts_ms - report.started_at_ms) / 1000).toFixed(1);
                return (
                  <tr key={i} className="border-b last:border-0 border-[var(--border-subtle)]">
                    <td className="px-4 py-2.5 font-mono text-xs tabular-nums">+{tRel}s</td>
                    <td className="px-4 py-2.5 font-mono text-xs">{e.scenario}</td>
                    <td className="px-4 py-2.5 text-xs text-[var(--text-muted)]">{e.detail}</td>
                    <td className="px-4 py-2.5"><HealthBadge status={e.ok ? 'ok' : 'fail'} small /></td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </Block>
      )}

      {/* Raw JSON (collapsed) */}
      <details className="group">
        <summary className="cursor-pointer text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] list-none flex items-center gap-2">
          <span className="group-open:rotate-90 transition-transform inline-block">▶</span>
          {t('stress.rawJson')}
        </summary>
        <pre className="mt-3 font-mono text-xs text-[var(--text-muted)] bg-canvas border border-[var(--border-subtle)] rounded-lg p-4 overflow-x-auto max-h-96 overflow-y-auto">
          {JSON.stringify(report, null, 2)}
        </pre>
      </details>
    </div>
  );
}
