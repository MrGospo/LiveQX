import React from 'react';
import { useTranslation } from 'react-i18next';
import { ChevronDown, ChevronRight, Radar, RefreshCw } from 'lucide-react';
import { useProbeStream, type ProbeStreamResult } from '@/api/queries/streams';
import { ApiClientError } from '@/api/client';
import { fmtBitrate } from '@/lib/format';

interface Props {
  address: string;
  port: string;
  interfaceName?: string;
}

export default function ProbeStreamPanel({ address, port, interfaceName }: Props) {
  const { t } = useTranslation();
  const [open, setOpen] = React.useState(false);
  const [result, setResult] = React.useState<ProbeStreamResult | null>(null);
  const [errMsg, setErrMsg] = React.useState<string | null>(null);
  const { mutate, isPending } = useProbeStream();

  const portNum = parseInt(port, 10);
  const canScan =
    address.trim().length > 0 &&
    !isNaN(portNum) && portNum >= 1 && portNum <= 65535 &&
    !isPending;

  const onScan = () => {
    setErrMsg(null);
    mutate(
      {
        address: address.trim(),
        port: portNum,
        interface_name: interfaceName?.trim() || undefined,
        duration_ms: 3000,
      },
      {
        onSuccess: (data) => {
          setResult(data);
          setOpen(true);
        },
        onError: (e) => {
          setResult(null);
          setOpen(true);
          setErrMsg(
            e instanceof ApiClientError
              ? (e.detail ?? e.code)
              : e instanceof Error ? e.message : t('common.error'),
          );
        },
      },
    );
  };

  const hasResult = result !== null || errMsg !== null;

  return (
    <div className="mt-3 border border-[var(--border-subtle)] rounded-md">
      <div className="flex items-center gap-2 px-3 py-2">
        <button
          type="button"
          onClick={onScan}
          disabled={!canScan}
          title={!canScan ? t('gateways.probe.needAddress') : ''}
          className="flex items-center gap-1.5 px-3 py-1.5 text-xs rounded-md bg-[var(--accent)]/15 border border-[var(--accent)]/40 text-[var(--accent)] hover:bg-[var(--accent)]/25 disabled:opacity-40 disabled:cursor-not-allowed transition-colors">
          {isPending
            ? <RefreshCw size={12} className="animate-spin" />
            : <Radar size={12} />}
          {isPending ? t('gateways.probe.scanning') : t('gateways.probe.scan')}
        </button>
        <span className="text-[11px] text-[var(--text-muted)]">
          {t('gateways.probe.hint')}
        </span>
        <div className="flex-1" />
        {hasResult && (
          <button
            type="button"
            onClick={() => setOpen(v => !v)}
            className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)]">
            {open ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
          </button>
        )}
      </div>

      {open && hasResult && (
        <div className="border-t border-[var(--border-subtle)] px-3 py-3">
          {errMsg && (
            <p className="text-xs text-[var(--danger)]">
              {t('gateways.probe.error')}: {errMsg}
            </p>
          )}
          {result && <ProbeResultView data={result} />}
        </div>
      )}
    </div>
  );
}

function ProbeResultView({ data }: { data: ProbeStreamResult }) {
  const { t } = useTranslation();
  if (data.programs.length === 0) {
    return (
      <div className="flex flex-col gap-1">
        <p className="text-xs text-[var(--warning)]">{t('gateways.probe.noPrograms')}</p>
        <p className="text-[11px] text-[var(--text-muted)]">
          {t('gateways.probe.duration')}: {data.duration_ms} ms •{' '}
          {t('gateways.probe.packets')}: {data.packets_received.toLocaleString()} •{' '}
          {t('gateways.probe.bitrate')}: {fmtBitrate(data.total_bitrate_bps)}
        </p>
      </div>
    );
  }
  return (
    <div className="flex flex-col gap-3">
      <div className="flex flex-wrap gap-x-4 gap-y-1 text-[11px] text-[var(--text-muted)]">
        <span>{t('gateways.probe.tsid')}: <span className="text-[var(--text-primary)] font-mono">{data.transport_stream_id}</span></span>
        <span>{t('gateways.probe.onid')}: <span className="text-[var(--text-primary)] font-mono">{data.original_network_id}</span></span>
        <span>{t('gateways.probe.bitrate')}: <span className="text-[var(--text-primary)] font-mono">{fmtBitrate(data.total_bitrate_bps)}</span></span>
        <span>{t('gateways.probe.packets')}: <span className="text-[var(--text-primary)] font-mono">{data.packets_received.toLocaleString()}</span></span>
        <span>{t('gateways.probe.duration')}: <span className="text-[var(--text-primary)] font-mono">{data.duration_ms} ms</span></span>
      </div>

      <div className="flex flex-col gap-2">
        {data.programs.map(p => (
          <div key={p.program_number}
            className="bg-canvas rounded-md border border-[var(--border-subtle)] p-2.5">
            <div className="flex items-baseline gap-2 flex-wrap">
              <span className="text-sm font-semibold text-[var(--text-primary)]">
                {p.service_name || `program ${p.program_number}`}
              </span>
              {p.provider_name && (
                <span className="text-[11px] text-[var(--text-muted)]">
                  · {p.provider_name}
                </span>
              )}
              <span className="text-[11px] text-[var(--text-muted)] font-mono">
                sid={p.program_number} pmt=0x{p.pmt_pid.toString(16)} pcr=0x{p.pcr_pid.toString(16)}
              </span>
            </div>
            <div className="mt-1.5 grid grid-cols-[auto_auto_auto_1fr_auto] gap-x-3 gap-y-0.5 text-[11px]">
              {p.streams.map(s => (
                <React.Fragment key={s.pid}>
                  <span className="font-mono text-[var(--text-muted)]">
                    pid 0x{s.pid.toString(16)}
                  </span>
                  <span className="font-mono text-[var(--text-muted)]">
                    type 0x{s.stream_type.toString(16).padStart(2, '0')}
                  </span>
                  <span className="text-[var(--text-primary)]">{s.codec}</span>
                  <span className="text-[var(--text-muted)]">
                    {s.language ? `[${s.language}]` : ''}
                  </span>
                  <span className="font-mono text-[var(--text-muted)]">
                    {s.bitrate_bps > 0 ? fmtBitrate(s.bitrate_bps) : '—'}
                  </span>
                </React.Fragment>
              ))}
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}
