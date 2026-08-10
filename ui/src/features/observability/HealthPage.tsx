/**
 * HealthPage — /observability/health
 * Full implementation per § 18.3 ui-spec.md
 */
import { useTranslation } from 'react-i18next';
import { RefreshCw, CheckCircle, XCircle, AlertCircle } from 'lucide-react';
import { useHealthz, useReadyz, useLivez } from '@/api/queries/operations';
import { useChannels } from '@/api/queries/channels';
import { usePlugins } from '@/api/queries/plugins';
import { Block } from '@/components/Block';
import { HealthBadge } from '@/components/HealthBadge';
import { SubNav } from '@/components/SubNav';
import { OBSERVABILITY_NAV } from './nav';

type ProbeResult = { status: number; body: unknown } | undefined;

function ProbeBadge({ result }: { result: ProbeResult }) {
  const { t } = useTranslation();
  if (!result) return <span className="text-xs text-[var(--text-muted)]">{t('health.checking')}</span>;
  const ok = result.status === 200;
  return ok
    ? <span className="flex items-center gap-1.5 text-[var(--success)] text-sm font-medium"><CheckCircle size={14} /> {result.status} OK</span>
    : <span className="flex items-center gap-1.5 text-[var(--danger)] text-sm font-medium"><XCircle size={14} /> {result.status}</span>;
}

export default function HealthPage() {
  const { t } = useTranslation();

  const { data: healthz, refetch: r1, isFetching: f1 } = useHealthz();
  const { data: readyz,  refetch: r2, isFetching: f2 } = useReadyz();
  const { data: livez,   refetch: r3, isFetching: f3 } = useLivez();
  const { data: channels = [] } = useChannels();
  const { data: plugins = [] } = usePlugins();

  const isFetching = f1 || f2 || f3;

  const refetchAll = () => { r1(); r2(); r3(); };

  // Build the health tree from channel data
  const channelTree = channels.map(ch => ({
    id: ch.id, name: ch.name, state: ch.state,
    outputs: ch.outputs ?? [],
  }));

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OBSERVABILITY_NAV} />
      {/* Header */}
      <div className="flex items-center gap-3">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('observability.health')}</h1>
        <button onClick={refetchAll} disabled={isFetching}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-50 transition-colors">
          <RefreshCw size={13} className={isFetching ? 'animate-spin' : ''} />
          {isFetching ? t('health.checking') : t('common.refresh')}
        </button>
        <span className="text-xs text-[var(--text-muted)]">{t('health.autoRefresh')}</span>
      </div>

      {/* Three probe badges */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('health.probes')}</h2>
        <div className="grid grid-cols-3 gap-4">
          {[
            { name: '/healthz', result: healthz as ProbeResult, hint: t('health.healthzHint') },
            { name: '/readyz',  result: readyz  as ProbeResult, hint: t('health.readyzHint') },
            { name: '/livez',   result: livez   as ProbeResult, hint: t('health.livezHint') },
          ].map(({ name, result, hint }) => (
            <div key={name} className="flex flex-col gap-2 p-3 bg-canvas rounded-lg border border-[var(--border-subtle)]">
              <div className="flex items-center justify-between">
                <code className="font-mono text-sm text-[var(--text-primary)]">{name}</code>
                <ProbeBadge result={result} />
              </div>
              <p className="text-xs text-[var(--text-muted)]">{hint}</p>
              {result && (result as ProbeResult)!.status !== 200 && (
                <pre className="font-mono text-xs text-[var(--danger)] bg-[var(--danger)]/5 rounded p-2 overflow-auto">
                  {JSON.stringify((result as ProbeResult)!.body, null, 2)}
                </pre>
              )}
            </div>
          ))}
        </div>
      </Block>

      {/* Channel → Outputs → Inputs health tree */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('health.channelTree')}</h2>
        {channelTree.length === 0 ? (
          <p className="text-sm text-[var(--text-muted)]">{t('health.noChannels')}</p>
        ) : (
          <div className="flex flex-col gap-3">
            {channelTree.map(ch => {
              const hasFailedOutput = ch.outputs.some(o => !o.healthy);
              const overallHealth = hasFailedOutput && ch.state === 'running' ? 'degraded' : ch.state;

              return (
                <div key={ch.id} className="border border-[var(--border-subtle)] rounded-lg overflow-hidden">
                  {/* Channel row */}
                  <div className={`flex items-center gap-3 px-4 py-2.5 ${hasFailedOutput ? 'bg-[var(--warning)]/5' : ''}`}>
                    <HealthBadge status={overallHealth} small />
                    <span className="font-semibold text-sm text-[var(--text-primary)]">ch{ch.id}‑{ch.name}</span>
                    {hasFailedOutput && (
                      <span className="flex items-center gap-1 text-xs text-[var(--warning)]">
                        <AlertCircle size={12} /> {t('health.degradedOutputs')}
                      </span>
                    )}
                  </div>

                  {/* Output rows */}
                  {ch.outputs.map(out => {
                    const outType = (out.transport ?? out.mode ?? 'unknown') as string;
                    const lastError = (out as { last_error?: string }).last_error;
                    return (
                      <div key={out.id}
                        className={`flex items-center gap-3 px-6 py-1.5 border-t border-[var(--border-subtle)] text-xs ${!out.healthy ? 'bg-[var(--danger)]/5' : ''}`}>
                        <div className="w-3 border-l-2 border-b-2 border-[var(--border-subtle)] h-3 flex-shrink-0" />
                        <HealthBadge status={out.healthy ? 'ok' : 'fail'} small />
                        <span className="font-mono text-[var(--text-muted)]">{outType.toUpperCase()}</span>
                        <span className="text-[var(--text-muted)]">{out.id}</span>
                        {lastError && (
                          <span className="text-[var(--danger)] truncate max-w-xs">{lastError}</span>
                        )}
                      </div>
                    );
                  })}
                </div>
              );
            })}
          </div>
        )}
      </Block>

      {/* Plugin health */}
      {plugins.length > 0 && (
        <Block>
          <h2 className="text-base font-semibold mb-4">{t('health.pluginsHealth')}</h2>
          <div className="flex flex-col gap-2">
            {plugins.map(p => (
              <div key={p.name} className="flex items-center gap-3 text-sm">
                <HealthBadge status={p.pending_unload ? 'stopping' : 'running'} small />
                <span className="font-mono text-[var(--text-primary)]">{p.name}</span>
                <span className="text-[var(--text-muted)]">v{p.version}</span>
                {p.pending_unload && <span className="text-xs text-[var(--warning)]">(pending unload)</span>}
              </div>
            ))}
          </div>
        </Block>
      )}
    </div>
  );
}
