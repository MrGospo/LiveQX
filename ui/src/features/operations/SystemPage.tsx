/**
 * SystemPage — /operations/system
 * Full implementation per § 19.4 ui-spec.md
 */
import { useTranslation } from 'react-i18next';
import { useNetworkInterfaces, useGpuInfo } from '@/api/queries/system';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { HealthBadge } from '@/components/HealthBadge';
import { SubNav } from '@/components/SubNav';
import { OPERATIONS_NAV } from './nav';
import { Cpu, RefreshCw } from 'lucide-react';

export default function SystemPage() {
  const { t } = useTranslation();

  const { data: ifaces = [], isLoading: ifaceInitLoading, isFetching: ifaceFetching, refetch: refetchIfaces } = useNetworkInterfaces();
  const { data: gpu,       isLoading: gpuInitLoading,  isFetching: gpuFetching,   refetch: refetchGpu   } = useGpuInfo();

  const gpuEntries = Object.entries(gpu ?? {});
  const hasAnyGpu = gpuEntries.some(([, v]) => v.built_in && v.codec_registered);

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OPERATIONS_NAV} />
      <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('operations.system')}</h1>

      {/* Network interfaces */}
      <Block padding="p-0">
        <div className="px-5 py-3 border-b border-[var(--border-subtle)] flex items-center justify-between">
          <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('system.interfaces')}</h2>
          <button onClick={() => refetchIfaces()} disabled={ifaceFetching}
            className="flex items-center gap-1.5 text-xs text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
            <RefreshCw size={12} className={ifaceFetching ? 'animate-spin' : ''} /> {t('common.refresh')}
          </button>
        </div>
        {ifaceInitLoading ? (
          <div className="p-5 flex flex-col gap-3">
            {Array(3).fill(0).map((_,i) => <div key={i} className="h-10 bg-surface2 rounded animate-pulse" />)}
          </div>
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {[t('system.ifaceName'), t('system.ifaceStatus'), t('system.ifaceAddresses'), t('system.ifaceFlags')].map(h => (
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {ifaces.map(iface => (
                <tr key={iface.name} className="border-b last:border-0 border-[var(--border-subtle)] hover:bg-surface2 transition-colors">
                  <td className="px-4 py-2.5 font-mono font-semibold text-[var(--text-primary)]">{iface.name}</td>
                  <td className="px-4 py-2.5">
                    <HealthBadge status={iface.up ? 'running' : 'stopped'} small />
                  </td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)]">
                    {iface.addresses?.join(', ') || '—'}
                  </td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)]">
                    {[iface.up && 'UP', iface.loopback && 'LOOPBACK'].filter(Boolean).join(' ') || '—'}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Block>

      {/* GPU info */}
      <Block>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('system.gpuEncoders')}</h2>
          <button onClick={() => refetchGpu()} disabled={gpuFetching}
            className="flex items-center gap-1.5 text-xs text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
            <RefreshCw size={12} className={gpuFetching ? 'animate-spin' : ''} /> {t('common.refresh')}
          </button>
        </div>

        {gpuInitLoading ? (
          <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
            {Array(4).fill(0).map((_,i) => <div key={i} className="h-24 bg-surface2 rounded-lg animate-pulse" />)}
          </div>
        ) : !hasAnyGpu ? (
          <EmptyState Icon={Cpu} title={t('system.noGpu')} description={t('system.noGpuHint')} />
        ) : (
          <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
            {gpuEntries.map(([name, info]) => {
              const available = info.built_in && info.codec_registered;
              const misconfigured = info.built_in && !info.codec_registered;
              return (
                <div key={name} className={`p-4 rounded-xl border transition-colors ${available ? 'border-[var(--success)]/30 bg-[var(--success)]/5' : misconfigured ? 'border-[var(--warning)]/30 bg-[var(--warning)]/5' : 'border-[var(--border-subtle)]'}`}>
                  <div className="flex items-center justify-between mb-3">
                    <span className="font-bold text-sm uppercase text-[var(--text-primary)]">{name}</span>
                    <HealthBadge status={available ? 'running' : misconfigured ? 'degraded' : 'stopped'} small />
                  </div>
                  <div className="flex flex-col gap-1.5 text-xs">
                    <div className="flex items-center gap-1.5">
                      <span className={info.built_in ? 'text-[var(--success)]' : 'text-[var(--text-muted)]'}>
                        {info.built_in ? '✓' : '✗'}
                      </span>
                      <span className="text-[var(--text-muted)]">built_in</span>
                    </div>
                    <div className="flex items-center gap-1.5">
                      <span className={info.codec_registered ? 'text-[var(--success)]' : 'text-[var(--text-muted)]'}>
                        {info.codec_registered ? '✓' : '✗'}
                      </span>
                      <span className="text-[var(--text-muted)]">codec_registered</span>
                    </div>
                  </div>
                  {misconfigured && (
                    <p className="text-xs text-[var(--warning)] mt-2">
                      {t('system.gpuMisconfigured')}
                    </p>
                  )}
                </div>
              );
            })}
          </div>
        )}
        <p className="text-xs text-[var(--text-muted)] mt-3">{t('system.gpuHint')}</p>
      </Block>
    </div>
  );
}
