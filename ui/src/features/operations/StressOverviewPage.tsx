import React, { useEffect, useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useStressStatus, useStartStress, useStopStress, useStressReports, useStressConfig, useSaveStressConfig } from '@/api/queries/operations';
import { Block } from '@/components/Block';
import { HealthBadge } from '@/components/HealthBadge';
import { SubNav } from '@/components/SubNav';
import { OPERATIONS_NAV } from './nav';
import { fmtDate } from '@/lib/format';
import { useToast } from '@/hooks/useToast';

// Converts seconds to human-readable label.
function fmtDuration(sec: number): string {
  if (sec < 60)   return `${sec}s`;
  if (sec < 3600) return `${Math.round(sec / 60)}m`;
  if (sec % 3600 === 0) return `${sec / 3600}h`;
  const h = Math.floor(sec / 3600);
  const m = Math.round((sec % 3600) / 60);
  return `${h}h ${m}m`;
}

// Parse "M H * * *" cron → "HH:MM"; returns '' for non-simple expressions.
function cronToTime(cron: string): string {
  const p = cron.trim().split(/\s+/);
  if (p.length !== 5) return '';
  const [min, hour] = p;
  if (/^\d+$/.test(min) && /^\d+$/.test(hour))
    return `${hour.padStart(2, '0')}:${min.padStart(2, '0')}`;
  return '';
}

// "HH:MM" → "M H * * *" cron (daily).
function timeToCron(hhmm: string): string {
  const [h, m] = hhmm.split(':').map(Number);
  return `${m} ${h} * * *`;
}

function VerdictBadge({ verdict }: { verdict: string }) {
  if (verdict === 'pass') return <span className="text-[var(--ok)] font-medium">OK</span>;
  return <span className="text-[var(--danger)] font-medium">Провал</span>;
}

// Slider steps: 1m, 5m, 10m, 30m, 1h, 2h, 4h, 8h, 12h, 24h
const DURATION_STEPS = [60, 300, 600, 1800, 3600, 7200, 14400, 28800, 43200, 86400];

export default function StressOverviewPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();
  const { data: status }  = useStressStatus();
  const { data: reports = [] } = useStressReports();
  const { mutate: start, isPending: starting } = useStartStress();
  const { mutate: stop,  isPending: stopping  } = useStopStress();
  const { data: cfg } = useStressConfig();
  const { mutate: saveConfig, isPending: savingCfg } = useSaveStressConfig();

  const enabled  = cfg?.enabled ?? false;
  const toggleEnabled = () => cfg && saveConfig({ ...cfg, enabled: !enabled });

  // Duration slider — local index into DURATION_STEPS
  const cfgDuration = cfg?.duration_sec ?? 3600;
  const closestIdx  = DURATION_STEPS.reduce((best, v, i) =>
    Math.abs(v - cfgDuration) < Math.abs(DURATION_STEPS[best] - cfgDuration) ? i : best, 0);
  const [sliderIdx, setSliderIdx] = useState(closestIdx);
  useEffect(() => { setSliderIdx(closestIdx); }, [closestIdx]);

  // Schedule time picker
  const cfgCron = cfg?.schedule_cron ?? '';
  const [scheduleTime, setScheduleTime] = useState(() => cronToTime(cfgCron) || '02:00');
  useEffect(() => {
    const t = cronToTime(cfgCron);
    if (t) setScheduleTime(t);
  }, [cfgCron]);

  function commitSchedule(val: string) {
    if (!cfg || !val) return;
    saveConfig({ ...cfg, schedule_cron: timeToCron(val) });
  }

  const isRunning   = status?.runner.state === 'running';
  const startedAt   = status?.runner.started_at_ms ?? 0;
  const durationMs  = cfgDuration * 1000;

  // Live progress ticker — updates every second while running
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    if (!isRunning) return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [isRunning]);

  const elapsedMs  = isRunning && startedAt ? Math.max(0, now - startedAt) : 0;
  const progressPct = durationMs > 0 ? Math.min(100, (elapsedMs / durationMs) * 100) : 0;
  const remainingSec = durationMs > 0 ? Math.max(0, Math.round((durationMs - elapsedMs) / 1000)) : 0;

  function commitDuration(idx: number) {
    if (!cfg) return;
    saveConfig({ ...cfg, duration_sec: DURATION_STEPS[idx] });
  }

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OPERATIONS_NAV} />
      <div className="flex items-center justify-between">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('stress.title')}</h1>
        <label className="inline-flex items-center gap-2 cursor-pointer select-none">
          <div onClick={() => !savingCfg && toggleEnabled()}
               className={`w-9 h-5 rounded-full relative transition-colors ${enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'} ${savingCfg ? 'opacity-50' : 'cursor-pointer'}`}>
            <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${enabled ? 'left-4' : 'left-0.5'}`} />
          </div>
          <span className="text-sm text-[var(--text-primary)]">{t('stress.enabled')}</span>
        </label>
      </div>

      <Block>
        <h2 className="text-base font-semibold mb-4">{t('stress.currentRun')}</h2>

        {/* Progress bar — visible only while running */}
        {isRunning && (
          <div className="mb-5">
            <div className="flex justify-between text-xs text-[var(--text-muted)] mb-1.5">
              <span>{t('stress.progress')}: <span className="font-semibold text-[var(--text-primary)]">{progressPct.toFixed(1)}%</span></span>
              <span>{t('stress.remaining')}: <span className="font-mono">{fmtDuration(remainingSec)}</span></span>
            </div>
            <div className="h-2 rounded-full bg-[var(--border-subtle)] overflow-hidden">
              <div
                className="h-full rounded-full bg-[var(--accent)] transition-all duration-1000"
                style={{ width: `${progressPct}%` }}
              />
            </div>
          </div>
        )}

        <div className="grid grid-cols-4 gap-4 mb-5 text-sm">
          {([
            [t('stress.status'),      <HealthBadge status={status?.runner.state ?? 'unknown'} small />],
            [t('stress.schedule'),    <input type="time" value={scheduleTime} onChange={e => setScheduleTime(e.target.value)} onBlur={e => commitSchedule(e.target.value)} disabled={savingCfg} className="bg-canvas border border-[var(--border-subtle)] rounded-md px-2 py-1 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors disabled:opacity-40 w-28" />],
            [t('stress.lastPass'),    status?.runner.last_ended_ms ? fmtDate(Math.floor(status.runner.last_ended_ms / 1000)) : '—'],
            [t('stress.lastVerdict'), status?.runner.last_verdict ? <VerdictBadge verdict={status.runner.last_verdict} /> : '—'],
          ] as Array<[string, React.ReactNode]>).map(([k, v]) => (
            <div key={k}><dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt><dd>{v}</dd></div>
          ))}
        </div>

        {/* Duration slider — hidden while test is running */}
        {!isRunning && (
          <div className="mb-5">
            <div className="flex justify-between text-xs text-[var(--text-muted)] mb-1.5">
              <span>{t('stress.duration')}</span>
              <span className="font-semibold text-[var(--text-primary)]">{fmtDuration(DURATION_STEPS[sliderIdx])}</span>
            </div>
            <input
              type="range"
              min={0}
              max={DURATION_STEPS.length - 1}
              step={1}
              value={sliderIdx}
              onChange={e => setSliderIdx(Number(e.target.value))}
              onMouseUp={e => commitDuration(Number((e.target as HTMLInputElement).value))}
              onTouchEnd={e => commitDuration(Number((e.target as HTMLInputElement).value))}
              disabled={savingCfg}
              className="w-full accent-[var(--accent)] disabled:opacity-50"
            />
            <div className="flex justify-between text-xs text-[var(--text-muted)] mt-0.5">
              <span>1m</span><span>5m</span><span>10m</span><span>30m</span>
              <span>1h</span><span>2h</span><span>4h</span><span>8h</span>
              <span>12h</span><span>24h</span>
            </div>
          </div>
        )}

        <div className="flex gap-2">
          {!isRunning ? (
            <button disabled={starting || !enabled} onClick={() => start(undefined, { onSuccess: () => toast(t('stress.started'), 'success') })}
              className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
              {starting ? t('stress.starting') : t('stress.startManual')}
            </button>
          ) : (
            <button disabled={stopping} onClick={() => stop(undefined, { onSuccess: () => toast(t('stress.stopped'), 'warning') })}
              className="px-4 py-2 bg-[var(--danger)] hover:bg-[var(--danger)]/80 text-white text-sm rounded-md disabled:opacity-50 transition-colors">
              {stopping ? t('stress.stopping') : t('stress.stop')}
            </button>
          )}
        </div>
      </Block>

      <Block padding="p-0">
        <div className="px-5 py-3 border-b border-[var(--border-subtle)]">
          <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('stress.reports')}</h2>
        </div>
        <table className="w-full text-sm border-collapse">
          <thead><tr className="border-b border-[var(--border-subtle)]">
            {[t('stress.thStarted'), t('stress.thEnded'), t('stress.thDuration'), t('stress.thVerdict')].map(h => (
              <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
            ))}
          </tr></thead>
          <tbody>
            {reports.map(r => (
              <tr key={r.id}
                onClick={() => navigate(`/operations/stress/${r.id}`)}
                className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 cursor-pointer transition-colors">
                <td className="px-4 py-2.5 text-xs text-[var(--text-muted)] tabular-nums">{fmtDate(Math.floor(r.started_at_ms / 1000))}</td>
                <td className="px-4 py-2.5 text-xs text-[var(--text-muted)] tabular-nums">{fmtDate(Math.floor(r.ended_at_ms / 1000))}</td>
                <td className="px-4 py-2.5 tabular-nums">{Math.round((r.ended_at_ms - r.started_at_ms) / 1000)}s</td>
                <td className="px-4 py-2.5"><VerdictBadge verdict={r.verdict} /></td>
              </tr>
            ))}
          </tbody>
        </table>
      </Block>
    </div>
  );
}
