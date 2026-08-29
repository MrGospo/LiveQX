/**
 * ServerPage — /observability/server
 *
 * Real-time host resource dashboard (CPU / RAM / NIC / FS / disk I/O).
 * Admin-only, live via SSE with per-page lifetime: the /proc sampler on
 * the backend only runs while this page is mounted, mirroring the
 * Windows Task Manager model. For historical data / alerting, scrape
 * Prometheus on /api/metrics.
 */
import { useTranslation } from 'react-i18next';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { fmtBytes, fmtBitrate, fmtUptime } from '@/lib/format';
import { OBSERVABILITY_NAV } from './nav';
import { useHostMetricsStream, type HostMetricsFrame } from './useHostMetricsStream';

// Bytes-per-second → bits-per-second, then the existing fmtBitrate helper
// (used everywhere else in the UI) formats to kbps/Mbps for a familiar look.
function fmtByteRate(bps: number | null | undefined): string {
  if (bps == null) return '—';
  return fmtBitrate(Math.round(bps * 8));
}

function pct(part: number, whole: number): number {
  if (whole <= 0) return 0;
  return Math.min(100, (part / whole) * 100);
}

function barColor(p: number): string {
  if (p >= 90) return 'bg-[var(--danger)]';
  if (p >= 75) return 'bg-[var(--warning)]';
  return 'bg-[var(--accent)]';
}

function ProgressBar({ value, className = '' }: { value: number; className?: string }) {
  return (
    <div className={`h-1.5 bg-surface2 rounded-full overflow-hidden ${className}`}>
      <div className={`h-full rounded-full transition-all duration-500 ${barColor(value)}`}
           style={{ width: `${value}%` }} />
    </div>
  );
}

export default function ServerPage() {
  const { t } = useTranslation();
  const { data, connected } = useHostMetricsStream();

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={OBSERVABILITY_NAV} />
      <div className="flex items-center gap-3">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('server.title')}</h1>
        <span className={`text-xs px-2 py-0.5 rounded-full ${
          connected
            ? 'bg-[var(--success)]/10 text-[var(--success)]'
            : 'bg-[var(--text-muted)]/10 text-[var(--text-muted)]'
        }`}>
          {connected ? t('server.live') : t('server.connecting')}
        </span>
        <span className="text-xs text-[var(--text-muted)]">{t('server.hint')}</span>
      </div>

      {!data ? (
        <div className="flex flex-col gap-4">
          {[0, 1, 2].map(i => <div key={i} className="h-32 bg-surface2 rounded-xl animate-pulse" />)}
        </div>
      ) : (
        <>
          <CpuBlock d={data} />
          <MemoryBlock d={data} />
          <NicBlock d={data} />
          <FsBlock d={data} />
          <DiskBlock d={data} />
        </>
      )}
    </div>
  );
}

function CpuBlock({ d }: { d: HostMetricsFrame }) {
  const { t } = useTranslation();
  const total = d.cpu.total_pct;
  return (
    <Block>
      <div className="flex items-baseline justify-between mb-4">
        <h2 className="text-base font-semibold">{t('server.cpu')}</h2>
        <div className="flex items-baseline gap-4 text-sm text-[var(--text-muted)]">
          <span>load: <span className="font-mono text-[var(--text-primary)]">{d.load1.toFixed(2)} / {d.load5.toFixed(2)} / {d.load15.toFixed(2)}</span></span>
          <span>uptime: <span className="font-mono text-[var(--text-primary)]">{fmtUptime(d.uptime_seconds)}</span></span>
        </div>
      </div>

      <div className="flex items-center gap-4 mb-5">
        <div className="text-4xl font-bold tabular-nums">
          {total != null ? `${total.toFixed(0)}%` : '—'}
        </div>
        <div className="flex-1">
          <ProgressBar value={total ?? 0} />
        </div>
      </div>

      {d.cpu.per_core.length > 0 && (
        <div className="grid grid-cols-4 sm:grid-cols-6 md:grid-cols-8 gap-3">
          {d.cpu.per_core.map(c => (
            <div key={c.name} className="flex flex-col gap-1">
              <div className="flex items-baseline justify-between text-xs">
                <span className="text-[var(--text-muted)]">{c.name}</span>
                <span className="font-mono">{c.pct != null ? `${c.pct.toFixed(0)}%` : '—'}</span>
              </div>
              <ProgressBar value={c.pct ?? 0} />
            </div>
          ))}
        </div>
      )}
    </Block>
  );
}

function MemoryBlock({ d }: { d: HostMetricsFrame }) {
  const { t } = useTranslation();
  const mem = d.mem;
  const usedPct = pct(mem.used_bytes, mem.total_bytes);
  const swapPct = pct(mem.swap_used_bytes, mem.swap_total_bytes);
  return (
    <Block>
      <h2 className="text-base font-semibold mb-4">{t('server.memory')}</h2>

      <div className="flex flex-col gap-2 mb-4">
        <div className="flex items-baseline justify-between text-sm">
          <span className="text-[var(--text-muted)]">{t('server.ram')}</span>
          <span className="font-mono">{fmtBytes(mem.used_bytes)} / {fmtBytes(mem.total_bytes)} <span className="text-[var(--text-muted)]">({usedPct.toFixed(0)}%)</span></span>
        </div>
        <ProgressBar value={usedPct} />
      </div>

      <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
        {[
          [t('server.available'), fmtBytes(mem.available_bytes)],
          [t('server.free'),      fmtBytes(mem.free_bytes)],
          [t('server.buffers'),   fmtBytes(mem.buffers_bytes)],
          [t('server.cached'),    fmtBytes(mem.cached_bytes)],
        ].map(([k, v]) => (
          <div key={String(k)}>
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
            <dd className="font-mono">{v}</dd>
          </div>
        ))}
      </div>

      {mem.swap_total_bytes > 0 && (
        <div className="flex flex-col gap-2 mt-5 pt-4 border-t border-[var(--border-subtle)]">
          <div className="flex items-baseline justify-between text-sm">
            <span className="text-[var(--text-muted)]">{t('server.swap')}</span>
            <span className="font-mono">{fmtBytes(mem.swap_used_bytes)} / {fmtBytes(mem.swap_total_bytes)} <span className="text-[var(--text-muted)]">({swapPct.toFixed(0)}%)</span></span>
          </div>
          <ProgressBar value={swapPct} />
        </div>
      )}
    </Block>
  );
}

function NicBlock({ d }: { d: HostMetricsFrame }) {
  const { t } = useTranslation();
  return (
    <Block>
      <h2 className="text-base font-semibold mb-4">{t('server.network')}</h2>
      {d.nics.length === 0 ? (
        <p className="text-sm text-[var(--text-muted)]">{t('server.noNics')}</p>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-sm">
            <thead className="text-xs text-[var(--text-muted)] uppercase tracking-wider">
              <tr className="border-b border-[var(--border-subtle)]">
                <th className="text-left  py-2 font-normal">{t('server.iface')}</th>
                <th className="text-left  py-2 font-normal">{t('server.state')}</th>
                <th className="text-right py-2 font-normal">RX</th>
                <th className="text-right py-2 font-normal">TX</th>
                <th className="text-right py-2 font-normal">RX total</th>
                <th className="text-right py-2 font-normal">TX total</th>
                <th className="text-right py-2 font-normal">{t('server.errdrop')}</th>
              </tr>
            </thead>
            <tbody>
              {d.nics.map(n => (
                <tr key={n.name} className="border-b border-[var(--border-subtle)] last:border-0">
                  <td className="py-2 font-mono">{n.name}</td>
                  <td className="py-2">
                    <span className={`text-xs px-2 py-0.5 rounded-full ${
                      n.is_up
                        ? 'bg-[var(--success)]/10 text-[var(--success)]'
                        : 'bg-[var(--text-muted)]/10 text-[var(--text-muted)]'
                    }`}>
                      {n.is_up ? 'up' : 'down'}
                    </span>
                  </td>
                  <td className="py-2 text-right font-mono tabular-nums">{fmtByteRate(n.rx_bps)}</td>
                  <td className="py-2 text-right font-mono tabular-nums">{fmtByteRate(n.tx_bps)}</td>
                  <td className="py-2 text-right font-mono tabular-nums text-[var(--text-muted)]">{fmtBytes(n.rx_bytes)}</td>
                  <td className="py-2 text-right font-mono tabular-nums text-[var(--text-muted)]">{fmtBytes(n.tx_bytes)}</td>
                  <td className="py-2 text-right font-mono tabular-nums text-[var(--text-muted)]">{n.rx_errs + n.rx_drop + n.tx_errs + n.tx_drop}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </Block>
  );
}

function FsBlock({ d }: { d: HostMetricsFrame }) {
  const { t } = useTranslation();
  return (
    <Block>
      <h2 className="text-base font-semibold mb-4">{t('server.filesystems')}</h2>
      {d.fs.length === 0 ? (
        <p className="text-sm text-[var(--text-muted)]">{t('server.noFs')}</p>
      ) : (
        <div className="flex flex-col gap-3">
          {d.fs.map(f => {
            const usedPct = pct(f.used_bytes, f.total_bytes);
            return (
              <div key={f.mount} className="flex flex-col gap-1.5">
                <div className="flex items-baseline justify-between text-sm">
                  <span className="font-mono truncate">{f.mount} <span className="text-xs text-[var(--text-muted)]">({f.fstype})</span></span>
                  <span className="font-mono">{fmtBytes(f.used_bytes)} / {fmtBytes(f.total_bytes)} <span className="text-[var(--text-muted)]">({usedPct.toFixed(0)}%)</span></span>
                </div>
                <ProgressBar value={usedPct} />
              </div>
            );
          })}
        </div>
      )}
    </Block>
  );
}

function DiskBlock({ d }: { d: HostMetricsFrame }) {
  const { t } = useTranslation();
  if (d.disks.length === 0) return null;
  return (
    <Block>
      <h2 className="text-base font-semibold mb-4">{t('server.diskio')}</h2>
      <div className="overflow-x-auto">
        <table className="w-full text-sm">
          <thead className="text-xs text-[var(--text-muted)] uppercase tracking-wider">
            <tr className="border-b border-[var(--border-subtle)]">
              <th className="text-left  py-2 font-normal">{t('server.device')}</th>
              <th className="text-right py-2 font-normal">{t('server.read')}</th>
              <th className="text-right py-2 font-normal">{t('server.write')}</th>
            </tr>
          </thead>
          <tbody>
            {d.disks.map(dk => (
              <tr key={dk.name} className="border-b border-[var(--border-subtle)] last:border-0">
                <td className="py-2 font-mono">{dk.name}</td>
                <td className="py-2 text-right font-mono tabular-nums">{fmtByteRate(dk.read_bps)}</td>
                <td className="py-2 text-right font-mono tabular-nums">{fmtByteRate(dk.write_bps)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </Block>
  );
}
