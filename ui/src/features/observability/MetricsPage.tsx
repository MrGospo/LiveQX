import React from 'react';
import { useTranslation } from 'react-i18next';
import { useSystemStatus, useMetricsTokenStatus, useGenerateMetricsToken, useDeleteMetricsToken } from '@/api/queries/system';
import { Block } from '@/components/Block';
import { HealthBadge } from '@/components/HealthBadge';
import { SubNav } from '@/components/SubNav';
import { fmtBytes, fmtUptime } from '@/lib/format';
import { OBSERVABILITY_NAV } from './nav';
import { Copy, Check, Trash2 } from 'lucide-react';

export default function MetricsPage() {
  const { t } = useTranslation();
  const { data: status, isLoading } = useSystemStatus();
  const visibleChannels = status?.channels ?? [];

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OBSERVABILITY_NAV} />
      <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('metrics.title')}</h1>

      {isLoading ? (
        <div className="flex flex-col gap-4">
          {Array(3).fill(0).map((_, i) => (
            <div key={i} className="h-32 bg-surface2 rounded-xl animate-pulse" />
          ))}
        </div>
      ) : (
        <>
          <Block>
            <h2 className="text-base font-semibold mb-4">{t('metrics.process')}</h2>
            <div className="grid grid-cols-3 sm:grid-cols-6 gap-4 text-sm">
              {[
                [t('metrics.uptime'),  status?.uptime_seconds != null ? fmtUptime(status.uptime_seconds) : '—'],
                [t('metrics.rss'),     fmtBytes(status?.process?.rss_bytes)],
                [t('metrics.vsize'),   fmtBytes(status?.process?.vsize_bytes)],
                [t('metrics.fds'),     status?.process?.open_fds ?? '—'],
                [t('metrics.threads'), status?.process?.threads ?? '—'],
                [t('metrics.cpu'),     status?.process?.cpu_seconds_total != null ? `${status.process.cpu_seconds_total.toFixed(0)}s` : '—'],
              ].map(([k, v]) => (
                <div key={String(k)}>
                  <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
                  <dd className="font-mono text-[var(--text-primary)]">{v}</dd>
                </div>
              ))}
            </div>
          </Block>

          <Block>
            <h2 className="text-base font-semibold mb-4">{t('metrics.channelsFps')}</h2>
            {visibleChannels.length === 0 ? (
              <p className="text-sm text-[var(--text-muted)]">{t('health.noChannels')}</p>
            ) : (
              <div className="flex flex-col gap-3">
                {visibleChannels.map(ch => {
                  const fps = ch.fps ?? 0;
                  const targetFps = ch.target_fps ?? 0;
                  const pct = targetFps > 0 ? Math.min(100, (fps / targetFps) * 100) : 0;
                  const color = ch.state !== 'running' ? 'bg-[var(--text-muted)]' : pct < 95 ? 'bg-[var(--warning)]' : 'bg-[var(--success)]';
                  const textColor = ch.state !== 'running' ? 'text-[var(--text-muted)]' : pct < 95 ? 'text-[var(--warning)]' : 'text-[var(--success)]';
                  return (
                    <div key={ch.id} className="flex items-center gap-3">
                      <span className="text-sm font-medium text-[var(--text-primary)] w-36 truncate">ch{ch.id}‑{ch.name}</span>
                      <HealthBadge status={ch.state} small />
                      <div className="flex-1 h-1.5 bg-surface2 rounded-full overflow-hidden">
                        <div className={`h-full rounded-full transition-all duration-500 ${color}`} style={{ width: `${pct}%` }} />
                      </div>
                      <span className={`text-sm tabular-nums font-mono w-20 text-right ${textColor}`}>
                        {fps > 0 ? fps.toFixed(2) : '—'} / {targetFps || '—'}
                      </span>
                    </div>
                  );
                })}
              </div>
            )}
          </Block>

          <PrometheusBlock />
        </>
      )}
    </div>
  );
}

function PrometheusBlock() {
  const { t } = useTranslation();
  const { data: status } = useMetricsTokenStatus();
  const { mutate: generate, isPending: generating } = useGenerateMetricsToken();
  const { mutate: remove,   isPending: removing   } = useDeleteMetricsToken();

  const [newToken, setNewToken] = React.useState<string | null>(null);
  const [copied,   setCopied]   = React.useState<'token' | 'yaml' | null>(null);

  const handleCopy = (text: string, kind: 'token' | 'yaml') => {
    navigator.clipboard.writeText(text).then(() => {
      setCopied(kind);
      setTimeout(() => setCopied(null), 2000);
    });
  };

  const host   = window.location.hostname;
  const port   = window.location.port || '8080';
  const target = `${host}:${port}`;

  const yamlSnippet = (token: string) =>
    `scrape_configs:\n  - job_name: liveqx\n    static_configs:\n      - targets: ['${target}']\n    metrics_path: /api/metrics\n    bearer_token: ${token}`;

  return (
    <Block>
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('metrics.prometheus')}</h2>
        {status?.configured && !newToken && (
          <button onClick={() => remove(undefined, { onSuccess: () => setNewToken(null) })}
            disabled={removing}
            className="flex items-center gap-1.5 text-xs text-[var(--danger)] hover:opacity-80 disabled:opacity-50 transition-opacity">
            <Trash2 size={12} /> {t('system.prometheusRevoke')}
          </button>
        )}
      </div>

      <div className="flex flex-col gap-4">
        {/* Endpoint */}
        <div>
          <p className="text-sm text-[var(--text-muted)] mb-2">{t('metrics.prometheusHint')}</p>
          <div className="bg-canvas rounded-lg px-4 py-3 font-mono text-sm text-[var(--accent)]">GET /api/metrics</div>
        </div>

        {/* Token status — hidden when new token is visible to avoid duplication */}
        {!newToken && (
          <div className="flex items-center gap-3">
            <span className="text-sm text-[var(--text-muted)]">{t('system.prometheusStatus')}:</span>
            {status?.configured
              ? <span className="text-sm font-mono text-[var(--success)]">{status.preview}</span>
              : <span className="text-sm text-[var(--text-muted)]">{t('system.prometheusNotConfigured')}</span>}
          </div>
        )}

        {/* Generate / Regenerate button */}
        {!newToken && (
          <button onClick={() => generate(undefined, { onSuccess: d => setNewToken(d.token) })}
            disabled={generating}
            className="self-start px-4 py-2 text-sm bg-[var(--accent)] text-white rounded-lg disabled:opacity-50 transition-opacity">
            {generating ? '…' : status?.configured ? t('system.prometheusRegenerate') : t('system.prometheusGenerate')}
          </button>
        )}

        {/* New token — shown once */}
        {newToken && (
          <div className="flex flex-col gap-3">
            <div className="p-3 bg-[var(--success)]/5 border border-[var(--success)]/20 rounded-lg">
              <p className="text-xs text-[var(--text-muted)] mb-2">{t('system.prometheusTokenOnce')}</p>
              <div className="flex items-center gap-2">
                <code className="flex-1 text-xs font-mono text-[var(--success)] break-all">{newToken}</code>
                <button onClick={() => handleCopy(newToken, 'token')}
                  className="flex-shrink-0 p-1.5 rounded hover:bg-surface2 transition-colors">
                  {copied === 'token'
                    ? <Check size={14} className="text-[var(--success)]" />
                    : <Copy size={14} className="text-[var(--text-muted)]" />}
                </button>
              </div>
            </div>

            {/* prometheus.yml snippet */}
            <div>
              <p className="text-xs text-[var(--text-muted)] mb-1.5">prometheus.yml:</p>
              <div className="relative">
                <pre className="text-xs font-mono bg-canvas border border-[var(--border-subtle)] rounded-lg p-3 pr-10 overflow-x-auto text-[var(--text-primary)] leading-relaxed">{yamlSnippet(newToken)}</pre>
                <button onClick={() => handleCopy(yamlSnippet(newToken), 'yaml')}
                  className="absolute top-2 right-2 p-1.5 rounded hover:bg-surface2 transition-colors">
                  {copied === 'yaml'
                    ? <Check size={12} className="text-[var(--success)]" />
                    : <Copy size={12} className="text-[var(--text-muted)]" />}
                </button>
              </div>
            </div>

            <button onClick={() => generate(undefined, { onSuccess: d => setNewToken(d.token) })}
              disabled={generating}
              className="self-start px-3 py-1.5 text-xs border border-[var(--border-subtle)] rounded-lg text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
              {t('system.prometheusRegenerate')}
            </button>
          </div>
        )}
      </div>
    </Block>
  );
}
