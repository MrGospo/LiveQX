/**
 * PerfPage — /operations/perf
 * Full implementation per § 19.3 ui-spec.md
 * Live sampling graph via inline SVG canvas (no recharts dependency).
 */
import React from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle } from 'lucide-react';
import { usePerfSnapshot, useStartPerf, useStopPerf } from '@/api/queries/operations';
import { useChannels } from '@/api/queries/channels';
import { useChannelAccess } from '@/hooks/useChannelAccess';
import { Block } from '@/components/Block';
import { HealthBadge } from '@/components/HealthBadge';
import { SubNav } from '@/components/SubNav';
import { OPERATIONS_NAV } from './nav';

const STAGES = ['decode', 'compose', 'encode', 'output'] as const;
type Stage = typeof STAGES[number];

const STAGE_COLORS: Record<Stage, string> = {
  decode:  'var(--accent)',
  compose: 'var(--info)',
  encode:  'var(--success)',
  output:  'var(--warning)',
};

// Rolling 60-sample history for live graph
function usePerfHistory(snapshot: ReturnType<typeof usePerfSnapshot>['data']) {
  const [history, setHistory] = React.useState<Array<Record<Stage, number>>>([]);
  React.useEffect(() => {
    if (!snapshot?.sampled_hits) return;
    const h = snapshot.sampled_hits;
    const total = STAGES.reduce((acc, s) => acc + (h[s] ?? 0), 0) || 1;
    setHistory(prev => [
      ...prev.slice(-59),
      Object.fromEntries(STAGES.map(s => [s, ((h[s] ?? 0) / total) * 100])) as Record<Stage, number>,
    ]);
  }, [snapshot?.sampled_hits]);
  return history;
}

// Tiny stacked area chart with inline SVG (responsive via viewBox)
function StackedAreaChart({ history }: { history: Array<Record<Stage, number>> }) {
  const W = 1000, H = 80;
  if (history.length < 2) return <div style={{ height: H }} className="w-full bg-canvas rounded" />;

  const n = history.length;
  const points = (stage: Stage): string => {
    const prev = history.map((h, i) => {
      const stagePct = STAGES.slice(0, STAGES.indexOf(stage)).reduce((acc, s) => acc + h[s], 0);
      return `${(i / (n - 1)) * W},${H - ((stagePct + h[stage]) / 100) * H}`;
    });
    const base = history.map((_, i) => {
      const stagePct = STAGES.slice(0, STAGES.indexOf(stage)).reduce((acc, s) => acc + history[i][s], 0);
      return `${((n - 1 - i) / (n - 1)) * W},${H - (stagePct / 100) * H}`;
    });
    return [...prev, ...base].join(' ');
  };

  return (
    <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" className="w-full h-20 overflow-visible rounded">
      {STAGES.map(s => (
        <polygon key={s} points={points(s)} fill={STAGE_COLORS[s]} opacity={0.25} />
      ))}
      {/* Axis labels */}
      <text x={2} y={H - 2} fontSize={9} fill="var(--text-muted)">0%</text>
      <text x={2} y={10}    fontSize={9} fill="var(--text-muted)">100%</text>
    </svg>
  );
}

// ─── Channel guard ─────────────────────────────────────────────────────────────
function ChannelGuard({ channelId, children }: { channelId: number; children: React.ReactNode }) {
  const { canOperate } = useChannelAccess(channelId);
  if (!canOperate) return <p className="text-sm text-[var(--danger)]">No operate access to this channel.</p>;
  return <>{children}</>;
}

// ─── Perf panel ───────────────────────────────────────────────────────────────
function PerfPanel({ channelId }: { channelId: number }) {
  const { t } = useTranslation();
  const { data: snap } = usePerfSnapshot(channelId);
  const { mutate: startPerf, isPending: starting } = useStartPerf(channelId);
  const { mutate: stopPerf,  isPending: stopping  } = useStopPerf(channelId);
  const [pendingMode, setPendingMode] = React.useState<'sampling'|'instrumentation'>('sampling');
  const [instrConfirmed, setInstrConfirmed] = React.useState(false);
  const history = usePerfHistory(snap);

  const isRunning = snap?.running ?? false;
  const mode = snap?.mode ?? 'off';

  const handleStart = () => {
    if (pendingMode === 'instrumentation' && !instrConfirmed) return;
    startPerf(pendingMode);
    setInstrConfirmed(false);
  };

  const h = snap?.sampled_hits ?? {};
  const total = STAGES.reduce((acc, s) => acc + (h[s] ?? 0), 0) || 1;

  const ins = snap?.stage_us ?? {};
  const cnt = snap?.stage_count ?? {};

  return (
    <div className="flex flex-col gap-5 w-full">
      {/* Instrumentation warning banner */}
      {mode === 'instrumentation' && isRunning && (
        <div className="flex items-start gap-2 p-3 bg-[var(--warning)]/10 border border-[var(--warning)]/30 rounded-lg text-sm text-[var(--text-primary)]">
          <AlertTriangle size={15} className="text-[var(--warning)] mt-0.5 flex-shrink-0" />
          {t('perf.instrWarning')}
        </div>
      )}

      {/* Controls */}
      <Block>
        <div className="flex items-center gap-3 flex-wrap">
          <div className="flex items-center gap-2">
            <span className="text-sm font-medium text-[var(--text-primary)]">{t('perf.mode')}:</span>
            <HealthBadge status={isRunning ? mode as string : 'stopped'} small />
          </div>
          <div className="flex-1" />
          <div className="flex items-center gap-2">
            {(['sampling','instrumentation'] as const).map(m => (
              <button key={m} onClick={() => setPendingMode(m)}
                className={`px-3 py-1.5 text-sm rounded-md border transition-colors ${pendingMode === m ? 'border-[var(--accent)] bg-[var(--accent)]/10 text-[var(--accent)]' : 'border-[var(--border-subtle)] text-[var(--text-muted)]'}`}>
                {m}
              </button>
            ))}
          </div>
          {isRunning ? (
            <button disabled={stopping} onClick={() => stopPerf()}
              className="px-3 py-1.5 text-sm bg-[var(--danger)] text-white rounded-md disabled:opacity-50 transition-colors">
              {stopping ? '…' : t('perf.stop')}
            </button>
          ) : (
            <button disabled={starting || (pendingMode === 'instrumentation' && !instrConfirmed)} onClick={handleStart}
              className="px-3 py-1.5 text-sm bg-[var(--accent)] text-white rounded-md disabled:opacity-50 transition-colors">
              {starting ? '…' : t('perf.start')}
            </button>
          )}
        </div>

        {pendingMode === 'instrumentation' && !isRunning && (
          <label className="flex items-start gap-2 mt-3 cursor-pointer">
            <input type="checkbox" checked={instrConfirmed} onChange={e => setInstrConfirmed(e.target.checked)} className="mt-0.5" />
            <span className="text-xs text-[var(--text-muted)]">{t('perf.instrConfirm')}</span>
          </label>
        )}
      </Block>

      {/* Stage distribution bar */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('perf.stageDistribution')}</h2>
        <div className="flex flex-col gap-3 mb-5">
          {STAGES.map(s => {
            const pct = ((h[s] ?? 0) / total) * 100;
            return (
              <div key={s} className="flex items-center gap-3">
                <span className="w-16 text-sm font-medium capitalize" style={{ color: STAGE_COLORS[s] }}>{s}</span>
                <div className="flex-1 h-2 bg-surface2 rounded-full overflow-hidden">
                  <div className="h-full rounded-full transition-all duration-500"
                    style={{ width: `${pct}%`, background: STAGE_COLORS[s] }} />
                </div>
                <span className="w-12 text-right text-sm text-[var(--text-muted)] tabular-nums">{pct.toFixed(1)}%</span>
              </div>
            );
          })}
        </div>
        {/* Rolling live chart */}
        <StackedAreaChart history={history} />
        <div className="flex gap-4 mt-2">
          {STAGES.map(s => (
            <span key={s} className="flex items-center gap-1 text-xs">
              <span style={{ background: STAGE_COLORS[s], width: 10, height: 10, borderRadius: 2, display: 'inline-block' }} />
              <span className="text-[var(--text-muted)] capitalize">{s}</span>
            </span>
          ))}
        </div>
      </Block>

      {/* Instrumentation breakdown (only when mode=instrumentation) */}
      {mode === 'instrumentation' && (
        <Block padding="p-0">
          <div className="px-5 py-3 border-b border-[var(--border-subtle)]">
            <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('perf.instrBreakdown')}</h2>
          </div>
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {['Stage','Total (µs)','Calls','Avg (µs)'].map(h => (
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {STAGES.map(s => {
                const total_us = ins[s] ?? 0;
                const calls = cnt[s] ?? 0;
                const avg = calls > 0 ? Math.round(total_us / calls) : 0;
                return (
                  <tr key={s} className="border-b last:border-0 border-[var(--border-subtle)]">
                    <td className="px-4 py-2.5 font-medium capitalize" style={{ color: STAGE_COLORS[s] }}>{s}</td>
                    <td className="px-4 py-2.5 tabular-nums">{total_us.toLocaleString()}</td>
                    <td className="px-4 py-2.5 tabular-nums">{calls.toLocaleString()}</td>
                    <td className="px-4 py-2.5 tabular-nums">{avg.toLocaleString()}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </Block>
      )}

      {/* Counter stats */}
      <Block>
        <div className="flex gap-6 text-sm">
          <KV label="Mode"       value={mode} />
          <KV label="Running"    value={isRunning ? 'yes' : 'no'} />
          <KV label="Active ms"  value={(snap?.active_ms ?? 0).toLocaleString()} />
        </div>
      </Block>
    </div>
  );
}

function KV({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-0.5">{label}</dt>
      <dd className="font-mono text-[var(--text-primary)]">{value}</dd>
    </div>
  );
}

// ─── Page ─────────────────────────────────────────────────────────────────────
export default function PerfPage() {
  const { t } = useTranslation();
  const { data: channels = [], isLoading } = useChannels();
  const [chId, setChId] = React.useState(0);

  React.useEffect(() => {
    if (channels.length && !chId) setChId(channels[0].id);
  }, [channels, chId]);

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OPERATIONS_NAV} />
      <div className="flex items-center gap-4 flex-wrap">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('operations.perf')}</h1>
        <select
          value={chId}
          onChange={e => setChId(Number(e.target.value))}
          disabled={isLoading || channels.length === 0}
          className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] disabled:opacity-50"
        >
          {isLoading && <option value={0}>{t('common.loading')}</option>}
          {!isLoading && channels.length === 0 && <option value={0}>{t('perf.noChannels')}</option>}
          {channels.map(c => <option key={c.id} value={c.id}>ch{c.id} · {c.name}</option>)}
        </select>
      </div>
      {!isLoading && channels.length === 0 && (
        <p className="text-sm text-[var(--text-muted)]">{t('perf.noChannelsHint')}</p>
      )}
      {chId > 0 && (
        <ChannelGuard channelId={chId}>
          <PerfPanel channelId={chId} />
        </ChannelGuard>
      )}
    </div>
  );
}
