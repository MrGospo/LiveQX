import React from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, Play, Square, Trash2, Edit2, Check, Plus, Shield } from 'lucide-react';
import { useGateway, usePlayGateway, useStopGateway, useDeleteGateway, usePatchGateway } from '@/api/queries/gateways';
import { useNetworkInterfaces } from '@/api/queries/system';
import type { FecCfg, GatewayConfig, GatewayStatus, NetworkInterface } from '@/api/types';
import { HealthBadge } from '@/components/HealthBadge';
import { Block } from '@/components/Block';
import { fmtBytes } from '@/lib/format';
import { useToast } from '@/hooks/useToast';

type GwMode = NonNullable<GatewayStatus['mode']>;

// fix40 UI-4 — compact rate formatter for live counters. We render with no
// decimals above 100, one decimal below 100, two below 10 — keeps the
// columns narrow while staying readable for low-rate streams.
function fmtRate(r: number): string {
  if (!Number.isFinite(r) || r <= 0) return '0';
  if (r >= 100) return Math.round(r).toLocaleString();
  if (r >= 10)  return r.toFixed(1);
  return r.toFixed(2);
}

const MODE_BADGE_CLS: Record<GwMode, string> = {
  passthrough: 'bg-[var(--text-muted)]/15 text-[var(--text-muted)]',
  demux:       'bg-blue-500/15 text-blue-400',
  remux:       'bg-purple-500/15 text-purple-400',
  transcode:   'bg-orange-500/15 text-orange-400',
};

export default function GatewayDetailPage() {
  const { id } = useParams<{ id: string }>();
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();
  const gwId = Number(id);

  // fix40 UI-4 — detail page polls every 2s for live counters. SSE alone
  // only fires on state transitions; FEC/pkt/byte counters need periodic
  // refresh to feel "live" while operator is watching.
  const { data: gw, isLoading, dataUpdatedAt } = useGateway(gwId, { live: true });
  const { mutate: play,   isPending: playing  } = usePlayGateway();
  const { mutate: stop,   isPending: stopping } = useStopGateway();
  const { mutate: del,    isPending: deleting } = useDeleteGateway();
  const { mutate: patch } = usePatchGateway(gwId);

  const [activeTab, setActiveTab] = React.useState<'overview'|'config'|'fec'>('overview');
  const [showDelete, setShowDelete] = React.useState(false);
  const [nameEdit, setNameEdit] = React.useState('');
  const [editingName, setEditingName] = React.useState(false);

  // fix40 UI-4 — derive pps/kbps from consecutive snapshots. We keep the
  // previous (counters, timestamp) tuple in a ref and compute deltas on
  // each render where the query updated. Counter resets (running flip
  // backend-side, manual /play) appear as negative deltas → clamp to 0.
  const prevSnapRef = React.useRef<{
    t: number; pkt_in: number; pkt_out: number; bytes_in: number; bytes_out: number;
    media_rtp: number; col_fec: number; row_fec: number;
  } | null>(null);
  const [rates, setRates] = React.useState({
    pps_in: 0, pps_out: 0, kbps_in: 0, kbps_out: 0,
    pps_media_rtp: 0, pps_col_fec: 0, pps_row_fec: 0,
  });
  React.useEffect(() => {
    if (!gw) return;
    const now = dataUpdatedAt || Date.now();
    const cur = {
      t:         now,
      pkt_in:    gw.pkt_in    ?? 0,
      pkt_out:   gw.pkt_out   ?? 0,
      bytes_in:  gw.bytes_in  ?? 0,
      bytes_out: gw.bytes_out ?? 0,
      media_rtp: gw.fec?.media_rtp_emitted   ?? 0,
      col_fec:   gw.fec?.column_fec_emitted  ?? 0,
      row_fec:   gw.fec?.row_fec_emitted     ?? 0,
    };
    const prev = prevSnapRef.current;
    if (prev && cur.t > prev.t) {
      const dt = (cur.t - prev.t) / 1000;
      const rate = (a: number, b: number) => Math.max(0, (a - b) / dt);
      setRates({
        pps_in:        rate(cur.pkt_in,    prev.pkt_in),
        pps_out:       rate(cur.pkt_out,   prev.pkt_out),
        kbps_in:       (rate(cur.bytes_in,  prev.bytes_in)  * 8) / 1000,
        kbps_out:      (rate(cur.bytes_out, prev.bytes_out) * 8) / 1000,
        pps_media_rtp: rate(cur.media_rtp, prev.media_rtp),
        pps_col_fec:   rate(cur.col_fec,   prev.col_fec),
        pps_row_fec:   rate(cur.row_fec,   prev.row_fec),
      });
    }
    prevSnapRef.current = cur;
  }, [dataUpdatedAt, gw]);

  React.useEffect(() => { if (gw) setNameEdit(gw.name); }, [gw]);

  if (isLoading) return (
    <div className="p-7 flex flex-col gap-4">
      {Array(3).fill(0).map((_,i) => <div key={i} className="h-24 bg-surface2 rounded-xl animate-pulse" />)}
    </div>
  );
  if (!gw) return <div className="p-7 text-[var(--text-muted)]">{t('gateways.notFound')}</div>;

  const isPlaying = gw.running;

  return (
    <div className="flex flex-col h-full">
      {/* Header */}
      <div className="sticky top-0 z-10 bg-surface border-b border-[var(--border-subtle)] px-7 pt-4">
        <button onClick={() => navigate('/gateways')}
          className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] mb-3 transition-colors">
          <ChevronLeft size={15} /> {t('gateways.title')}
        </button>
        <div className="flex items-center gap-3 mb-3 flex-wrap">
          {editingName ? (
            <div className="flex items-center gap-2">
              <input value={nameEdit} onChange={e => setNameEdit(e.target.value)}
                className="bg-canvas border border-[var(--accent)] rounded-md px-3 py-1.5 text-lg font-bold text-[var(--text-primary)] outline-none w-48" />
              <button onClick={() => { patch({ name: nameEdit }, { onSuccess: () => toast(t('gateways.nameSaved'), 'success') }); setEditingName(false); }}
                className="p-1.5 text-[var(--success)] hover:bg-[var(--success)]/10 rounded transition-colors">
                <Check size={16} />
              </button>
            </div>
          ) : (
            <div className="flex items-center gap-2">
              <h1 className="text-2xl font-bold text-[var(--text-primary)]">{gw.name}</h1>
              <button onClick={() => setEditingName(true)} className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)]">
                <Edit2 size={14} />
              </button>
            </div>
          )}
          <HealthBadge status={gw.running ? 'running' : 'stopped'} />
          {(() => {
            const mode: GwMode = gw.mode ?? 'passthrough';
            return (
              <span className={`text-[10px] uppercase tracking-wider px-1.5 py-0.5 rounded font-semibold ${MODE_BADGE_CLS[mode]}`}>
                {t(`gateways.mode.${mode}`)}
              </span>
            );
          })()}
          {gw.fec?.enabled && (
            <span className="text-[10px] uppercase tracking-wider px-1.5 py-0.5 rounded font-semibold bg-[var(--success)]/15 text-[var(--success)] flex items-center gap-1">
              <Shield size={10} /> FEC {(gw.fec.mode ?? '1d').toUpperCase()}
            </span>
          )}
          <div className="flex-1" />
          <div className="flex gap-2">
            {!isPlaying ? (
              <button disabled={playing} onClick={() => play(gwId, { onSuccess: () => toast(t('gateways.started', { name: gw.name }), 'success') })}
                className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--success)]/15 text-[var(--success)] rounded-md hover:bg-[var(--success)]/25 disabled:opacity-50 transition-colors">
                <Play size={13} /> {t('gateways.start')}
              </button>
            ) : (
              <button disabled={stopping} onClick={() => stop(gwId, { onSuccess: () => toast(t('gateways.stopped', { name: gw.name }), 'info') })}
                className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
                <Square size={13} /> {t('gateways.stop')}
              </button>
            )}
            <button onClick={() => setShowDelete(true)}
              className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--danger)]/40 text-[var(--danger)] rounded-md hover:bg-[var(--danger)]/10 transition-colors">
              <Trash2 size={13} /> {t('common.delete')}
            </button>
          </div>
        </div>
        {/* Tabs */}
        <div className="flex gap-0 -mb-px">
          {(['overview','config','fec'] as const)
            .filter(tab => tab !== 'fec' || (gw.mode && gw.mode !== 'passthrough'))
            .map(tab => (
              <button key={tab} onClick={() => setActiveTab(tab)}
                className={`px-4 py-2.5 text-sm font-medium border-b-2 transition-colors capitalize ${activeTab === tab ? 'text-[var(--accent)] border-[var(--accent)]' : 'text-[var(--text-muted)] border-transparent hover:text-[var(--text-primary)]'}`}>
                {t(`gateways.tab_${tab}`)}
              </button>
            ))}
        </div>
      </div>

      {/* Content */}
      <div className="flex-1 overflow-y-auto p-7">
        {activeTab === 'overview' && (
          <div className="flex flex-col gap-5 max-w-2xl">
            <Block>
              <div className="flex items-center justify-between mb-4">
                <h2 className="text-base font-semibold">{t('gateways.stats')}</h2>
                {gw.running && (
                  <span className="flex items-center gap-1.5 text-[11px] font-medium text-[var(--success)] tabular-nums">
                    <span className="w-1.5 h-1.5 rounded-full bg-[var(--success)] animate-pulse" />
                    LIVE · {fmtRate(rates.kbps_in)} / {fmtRate(rates.kbps_out)} kbps
                  </span>
                )}
              </div>
              <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
                {([
                  [t('gateways.stateLabel'), <HealthBadge status={gw.running ? 'running' : 'stopped'} small />],
                  [t('gateways.bytesIn'),    <><span>{fmtBytes(gw.bytes_in)}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.kbps_in)} kbps</span>}</>],
                  [t('gateways.bytesOut'),   <><span>{fmtBytes(gw.bytes_out)}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.kbps_out)} kbps</span>}</>],
                  [t('gateways.packetsIn'),  <><span>{gw.pkt_in.toLocaleString()}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.pps_in)} pps</span>}</>],
                  [t('gateways.packetsOut'), <><span>{gw.pkt_out.toLocaleString()}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.pps_out)} pps</span>}</>],
                  [t('gateways.drops'),      gw.drops.toLocaleString()],
                ] as Array<[string, React.ReactNode]>).map(([k,v]) => (
                  <div key={k}>
                    <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
                    <dd className="text-[var(--text-primary)] tabular-nums">{v}</dd>
                  </div>
                ))}
              </div>
            </Block>
            <Block>
              <h2 className="text-base font-semibold mb-4">{t('gateways.endpoints')}</h2>
              <div className="grid grid-cols-2 gap-4">
                <div className="bg-canvas rounded-lg p-4 border border-[var(--border-subtle)]">
                  <div className="text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)] mb-2">
                    {(gw.inputs && gw.inputs.length > 1) ? `${t('gateways.input')}s (${gw.inputs.length})` : t('gateways.input')}
                  </div>
                  {(() => {
                    const inputs = gw.inputs && gw.inputs.length ? gw.inputs : (gw.input ? [gw.input] : []);
                    if (!inputs.length) return <div className="text-xs text-[var(--text-muted)]">—</div>;
                    return (
                      <ul className="flex flex-col gap-1.5">
                        {inputs.map((inp, i) => (
                          <li key={i}>
                            <div className="font-mono text-sm text-[var(--text-primary)]">{inp.address}:{inp.port}</div>
                            <div className="text-xs text-[var(--text-muted)]">{inp.interface ?? inp.interface_address ?? '—'}</div>
                          </li>
                        ))}
                      </ul>
                    );
                  })()}
                </div>
                <div className="bg-canvas rounded-lg p-4 border border-[var(--border-subtle)]">
                  <div className="text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)] mb-2">{t('gateways.outputs')} ({gw.outputs.length})</div>
                  <ul className="flex flex-col gap-1.5">
                    {gw.outputs.map((o, i) => (
                      <li key={i} className="font-mono text-sm text-[var(--text-primary)]">
                        {o.address}:{o.port}
                        <span className="text-xs text-[var(--text-muted)] ml-2">ttl={o.ttl}{o.interface ? `, ${o.interface}` : ''}</span>
                      </li>
                    ))}
                  </ul>
                </div>
              </div>
            </Block>
            {gw.fec?.enabled && (
              <Block>
                <h2 className="text-base font-semibold mb-4 flex items-center gap-2">
                  <Shield size={16} className="text-[var(--success)]" />
                  {t('gateways.fec.label')}
                </h2>
                <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
                  {([
                    [t('gateways.fec.mode'),     `${(gw.fec.mode ?? '1d').toUpperCase()} (L=${gw.fec.L ?? 8} D=${gw.fec.D ?? 8})`],
                    [t('gateways.fec.mediaRtp'), <><span>{(gw.fec.media_rtp_emitted ?? 0).toLocaleString()}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.pps_media_rtp)} pps</span>}</>],
                    [t('gateways.fec.colFec'),   <><span>{(gw.fec.column_fec_emitted ?? 0).toLocaleString()}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.pps_col_fec)} pps</span>}</>],
                    [t('gateways.fec.rowFec'),   gw.fec.mode === '2d' ? <><span>{(gw.fec.row_fec_emitted ?? 0).toLocaleString()}</span>{gw.running && <span className="ml-1.5 text-[11px] text-[var(--text-muted)]">{fmtRate(rates.pps_row_fec)} pps</span>}</> : '—'],
                  ] as Array<[string, React.ReactNode]>).map(([k,v]) => (
                    <div key={k}>
                      <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
                      <dd className="text-[var(--text-primary)] tabular-nums">{v}</dd>
                    </div>
                  ))}
                </div>
              </Block>
            )}
          </div>
        )}

        {activeTab === 'fec' && (
          <FecForm gw={gw} />
        )}

        {activeTab === 'config' && (
          <PatchGatewayForm gw={gw} />
        )}
      </div>

      {/* Delete confirm */}
      {showDelete && (
        <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-5">
          <div className="w-full max-w-md bg-surface border border-[var(--border-subtle)] rounded-xl p-7 flex flex-col gap-4">
            <h2 className="text-xl font-semibold text-[var(--text-primary)]">{t('gateways.deleteTitle', { name: gw.name })}</h2>
            <p className="text-sm text-[var(--text-muted)]">{t('gateways.deleteWarning')}</p>
            <div className="flex gap-2 justify-end">
              <button onClick={() => setShowDelete(false)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">{t('common.cancel')}</button>
              <button disabled={deleting} onClick={() => del(gwId, { onSuccess: () => { navigate('/gateways'); toast(t('gateways.deleted', { name: gw.name }), 'warning'); }})}
                className="px-4 py-2 bg-[var(--danger)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {deleting ? t('common.loading') : t('common.delete')}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

// ─── Patch form ───────────────────────────────────────────────────────────────

interface OutputDraft {
  address: string;
  port: string;
  ttl: string;
  send_buffer_kb: string;
  iface: string;
  id: string;
}

function PatchGatewayForm({ gw }: { gw: GatewayStatus }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { mutate: patch, isPending } = usePatchGateway(gw.id);
  const { data: ifaces = [] } = useNetworkInterfaces();

  // GatewayManager::patch (src/gateway/GatewayManager.cpp:271) accepts only
  // `name` and/or `outputs` — input rebinds need delete+recreate.
  const fromGw = (): OutputDraft[] => gw.outputs.map(o => ({
    address: o.address,
    port: String(o.port),
    ttl: String(o.ttl ?? 16),
    send_buffer_kb: String(o.send_buffer_kb ?? 256),
    iface: o.interface ?? '',
    id: o.id ?? '',
  }));

  const [outputs, setOutputs] = React.useState<OutputDraft[]>(fromGw);
  const [errors, setErrors]   = React.useState<string[]>([]);

  // Re-sync if backend snapshot changes (e.g. SSE invalidation).
  React.useEffect(() => { setOutputs(fromGw()); }, [gw.outputs]);

  const setOut = (i: number, p: Partial<OutputDraft>) =>
    setOutputs(arr => arr.map((o, idx) => idx === i ? { ...o, ...p } : o));
  const addOut = () => setOutputs(arr => [...arr, {
    address: '239.0.0.1', port: '5000', ttl: '16',
    send_buffer_kb: '256', iface: '', id: '',
  }]);
  const removeOut = (i: number) => setOutputs(arr => arr.filter((_, idx) => idx !== i));

  const onSave = () => {
    const e: string[] = [];
    if (!outputs.length) e.push(t('gateways.form.validation.outputsRequired'));
    outputs.forEach((o, idx) => {
      if (!o.address.trim()) e.push(`#${idx + 1}: ${t('gateways.form.validation.addressRequired')}`);
      const op = parseInt(o.port, 10);
      if (isNaN(op) || op < 1 || op > 65535) e.push(`#${idx + 1}: ${t('gateways.form.validation.portRange')}`);
      const tt = parseInt(o.ttl, 10);
      if (isNaN(tt) || tt < 1 || tt > 255) e.push(`#${idx + 1}: ${t('gateways.form.validation.ttlRange')}`);
    });
    setErrors(e);
    if (e.length) return;
    patch(
      {
        outputs: outputs.map(o => ({
          address: o.address.trim(),
          port: parseInt(o.port, 10),
          ttl: parseInt(o.ttl, 10),
          send_buffer_kb: parseInt(o.send_buffer_kb, 10) || 256,
          ...(o.iface ? { interface: o.iface } : {}),
          ...(o.id.trim() ? { id: o.id.trim() } : {}),
        })),
      } as Partial<GatewayConfig>,
      {
        onSuccess: () => toast(t('gateways.patch.saved'), 'success'),
        onError: (err) => {
          const msg = err instanceof Error ? err.message : t('common.error');
          toast(msg, 'danger');
        },
      },
    );
  };

  return (
    <div className="max-w-3xl flex flex-col gap-5">
      {/* Input — read-only, rebind requires delete+recreate */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">
          {t('gateways.patch.inputReadonly')}
        </h2>
        <div className="grid grid-cols-3 gap-4 text-sm">
          <FieldRO label={t('gateways.form.address')}>{gw.input?.address ?? '—'}</FieldRO>
          <FieldRO label={t('gateways.form.port')}>{gw.input?.port ?? '—'}</FieldRO>
          <FieldRO label={t('gateways.form.iface')}>
            {gw.input?.interface ?? gw.input?.interface_address ?? '—'}
          </FieldRO>
          <FieldRO label={t('gateways.form.recvBuffer')}>{gw.input?.recv_buffer_kb ?? '—'}</FieldRO>
        </div>
        <p className="text-xs text-[var(--warning)] mt-3">
          {t('gateways.patch.inputRebindHint')}
        </p>
      </Block>

      {/* Outputs — editable */}
      <Block>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-base font-semibold text-[var(--text-primary)]">
            {t('gateways.patch.title')}
          </h2>
          <button type="button" onClick={addOut}
            className="flex items-center gap-1 px-2 py-1 text-xs border border-[var(--border-subtle)] text-[var(--text-muted)] rounded hover:text-[var(--text-primary)] transition-colors">
            <Plus size={12} /> {t('gateways.form.addOutput')}
          </button>
        </div>
        <p className="text-xs text-[var(--text-muted)] mb-3">{t('gateways.patch.outputsHint')}</p>
        <div className="flex flex-col gap-3">
          {outputs.map((o, i) => (
            <div key={i} className="bg-canvas rounded-lg p-3 border border-[var(--border-subtle)]">
              <div className="flex items-center justify-between mb-2">
                <span className="text-xs font-semibold text-[var(--text-muted)]">
                  {t('gateways.form.outputN', { n: i + 1 })}
                </span>
                {outputs.length > 1 && (
                  <button type="button" onClick={() => removeOut(i)}
                    className="text-xs text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors flex items-center gap-1">
                    <Trash2 size={12} /> {t('gateways.form.removeOutput')}
                  </button>
                )}
              </div>
              <div className="grid grid-cols-3 gap-3">
                <PatchField label={t('gateways.form.address')} span={2}>
                  <input value={o.address} onChange={e => setOut(i, { address: e.target.value })}
                    className={patchInputCls} placeholder="239.0.0.1" />
                </PatchField>
                <PatchField label={t('gateways.form.port')}>
                  <input type="number" min={1} max={65535} value={o.port}
                    onChange={e => setOut(i, { port: e.target.value })} className={patchInputCls} />
                </PatchField>
                <PatchField label={t('gateways.form.ttl')}>
                  <input type="number" min={1} max={255} value={o.ttl}
                    onChange={e => setOut(i, { ttl: e.target.value })} className={patchInputCls} />
                </PatchField>
                <PatchField label={t('gateways.form.sendBuffer')}>
                  <input type="number" min={64} value={o.send_buffer_kb}
                    onChange={e => setOut(i, { send_buffer_kb: e.target.value })} className={patchInputCls} />
                </PatchField>
                <PatchField label={t('gateways.form.outputId')}>
                  <input value={o.id} onChange={e => setOut(i, { id: e.target.value })}
                    className={patchInputCls} placeholder="dvb-out-1" />
                </PatchField>
                <PatchField label={t('gateways.form.iface')} span={3}>
                  <PatchNicSelect value={o.iface} onChange={(v) => setOut(i, { iface: v })} ifaces={ifaces} />
                </PatchField>
              </div>
            </div>
          ))}
        </div>

        {errors.length > 0 && (
          <div className="mt-3 bg-[var(--danger)]/10 border border-[var(--danger)]/40 rounded-md p-3 flex flex-col gap-1">
            {errors.map((er, i) => (
              <p key={i} className="text-xs text-[var(--danger)]">• {er}</p>
            ))}
          </div>
        )}

        <button disabled={isPending} onClick={onSave}
          className="mt-4 px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {isPending ? `${t('common.save')}…` : t('common.save')}
        </button>
      </Block>
    </div>
  );
}

// ─── FEC form (PATCH /api/gateways/{id}/fec) ─────────────────────────────────

interface FecDraft {
  enabled: boolean;
  mode:    '1d' | '2d';
  L:       string;
  D:       string;
  ssrc:    string;
  payload_type:       string;
  ts_per_rtp:         string;
  column_port_offset: string;
  row_port_offset:    string;
}

const fromGwFec = (cfg?: FecCfg): FecDraft => ({
  enabled: cfg?.enabled ?? false,
  mode:    cfg?.mode ?? '1d',
  L:       String(cfg?.L ?? 8),
  D:       String(cfg?.D ?? 8),
  ssrc:    String(cfg?.ssrc ?? 0),
  payload_type:       String(cfg?.payload_type ?? 33),
  ts_per_rtp:         String(cfg?.ts_per_rtp ?? 7),
  column_port_offset: String(cfg?.column_port_offset ?? 2),
  row_port_offset:    String(cfg?.row_port_offset ?? 4),
});

function FecForm({ gw }: { gw: GatewayStatus }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { mutate: patch, isPending } = usePatchGateway(gw.id);
  const [draft, setDraft] = React.useState<FecDraft>(fromGwFec(gw.fec));
  const [errors, setErrors] = React.useState<string[]>([]);

  React.useEffect(() => { setDraft(fromGwFec(gw.fec)); }, [gw.fec]);

  const set = (p: Partial<FecDraft>) => setDraft(d => ({ ...d, ...p }));

  const validate = (): string[] => {
    const e: string[] = [];
    if (!draft.enabled) return e;
    const L = parseInt(draft.L, 10);
    const D = parseInt(draft.D, 10);
    if (isNaN(L) || L < 1 || L > 20) e.push(t('gateways.form.validation.fecLRange'));
    if (isNaN(D) || D < 4 || D > 20) e.push(t('gateways.form.validation.fecDRange'));
    if (!isNaN(L) && !isNaN(D) && L * D > 100) e.push(t('gateways.form.validation.fecLD'));
    const co = parseInt(draft.column_port_offset, 10);
    const ro = parseInt(draft.row_port_offset, 10);
    if (isNaN(co) || co < 1) e.push(t('gateways.form.validation.fecColOff'));
    if (draft.mode === '2d') {
      if (isNaN(ro) || ro < 1) e.push(t('gateways.form.validation.fecRowOff'));
      if (co === ro) e.push(t('gateways.form.validation.fecOffsetsDiffer'));
    }
    return e;
  };

  const onSave = () => {
    const errs = validate();
    setErrors(errs);
    if (errs.length) return;
    const body: FecCfg = {
      enabled:            draft.enabled,
      mode:               draft.mode,
      L:                  parseInt(draft.L, 10) || 8,
      D:                  parseInt(draft.D, 10) || 8,
      ssrc:               parseInt(draft.ssrc, 10) || 0,
      payload_type:       parseInt(draft.payload_type, 10) || 33,
      ts_per_rtp:         parseInt(draft.ts_per_rtp, 10) || 7,
      column_port_offset: parseInt(draft.column_port_offset, 10) || 2,
      row_port_offset:    parseInt(draft.row_port_offset, 10) || 4,
    };
    patch({ fec: body } as Partial<GatewayConfig>, {
      onSuccess: () => toast(t('gateways.fec.saved'), 'success'),
      onError: (err) => {
        const msg = err instanceof Error ? err.message : t('common.error');
        toast(msg, 'danger');
      },
    });
  };

  return (
    <div className="max-w-3xl flex flex-col gap-5">
      <Block>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-base font-semibold text-[var(--text-primary)] flex items-center gap-2">
            <Shield size={16} /> {t('gateways.fec.label')}
          </h2>
          <label className="flex items-center gap-2 cursor-pointer">
            <span className="text-xs text-[var(--text-muted)]">{t('gateways.fec.enabledLabel')}</span>
            <div onClick={() => set({ enabled: !draft.enabled })}
              className={`w-9 h-5 rounded-full relative transition-colors ${draft.enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
              <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${draft.enabled ? 'left-4' : 'left-0.5'}`} />
            </div>
          </label>
        </div>
        <p className="text-xs text-[var(--text-muted)] mb-4">{t('gateways.fec.editorHint')}</p>

        {draft.enabled && (
          <div className="grid grid-cols-3 gap-3">
            <PatchField label={t('gateways.form.fecMode')}>
              <select value={draft.mode} onChange={e => set({ mode: e.target.value as '1d'|'2d' })} className={patchInputCls}>
                <option value="1d">{t('gateways.fec.mode1d')}</option>
                <option value="2d">{t('gateways.fec.mode2d')}</option>
              </select>
            </PatchField>
            <PatchField label="L">
              <input type="number" min={1} max={20} value={draft.L}
                onChange={e => set({ L: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label="D">
              <input type="number" min={4} max={20} value={draft.D}
                onChange={e => set({ D: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label={t('gateways.fec.payloadType')}>
              <input type="number" min={0} max={127} value={draft.payload_type}
                onChange={e => set({ payload_type: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label={t('gateways.fec.tsPerRtp')}>
              <input type="number" min={1} max={7} value={draft.ts_per_rtp}
                onChange={e => set({ ts_per_rtp: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label={t('gateways.fec.ssrc')}>
              <input type="number" min={0} value={draft.ssrc}
                onChange={e => set({ ssrc: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label={t('gateways.fec.colOff')}>
              <input type="number" min={1} value={draft.column_port_offset}
                onChange={e => set({ column_port_offset: e.target.value })} className={patchInputCls} />
            </PatchField>
            <PatchField label={t('gateways.fec.rowOff')}>
              <input type="number" min={1} value={draft.row_port_offset}
                onChange={e => set({ row_port_offset: e.target.value })} className={patchInputCls} />
            </PatchField>
          </div>
        )}

        {errors.length > 0 && (
          <div className="mt-3 bg-[var(--danger)]/10 border border-[var(--danger)]/40 rounded-md p-3 flex flex-col gap-1">
            {errors.map((er, i) => (
              <p key={i} className="text-xs text-[var(--danger)]">• {er}</p>
            ))}
          </div>
        )}

        <button disabled={isPending} onClick={onSave}
          className="mt-4 px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {isPending ? `${t('common.save')}…` : t('common.save')}
        </button>

        <p className="text-[11px] text-[var(--warning)] leading-snug mt-3">
          {t('gateways.fec.hotSwapWarn')}
        </p>
      </Block>
    </div>
  );
}

const patchInputCls =
  'bg-surface border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] w-full';

function FieldRO({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-1">
      <span className="text-xs uppercase tracking-wider text-[var(--text-muted)]">{label}</span>
      <span className="font-mono text-sm text-[var(--text-primary)]">{children}</span>
    </div>
  );
}

function PatchField({
  label, children, span,
}: {
  label: string;
  children: React.ReactNode;
  span?: 1 | 2 | 3;
}) {
  const cls = span === 3 ? 'col-span-3' : span === 2 ? 'col-span-2' : '';
  return (
    <div className={`flex flex-col gap-1 ${cls}`}>
      <label className="text-xs font-medium text-[var(--text-primary)]">{label}</label>
      {children}
    </div>
  );
}

function PatchNicSelect({
  value, onChange, ifaces,
}: {
  value: string;
  onChange: (v: string) => void;
  ifaces: NetworkInterface[];
}) {
  const { t } = useTranslation();
  return (
    <select value={value} onChange={e => onChange(e.target.value)} className={patchInputCls}>
      <option value="">{t('gateways.form.ifaceAuto')}</option>
      {ifaces.map(nic => (
        <option key={nic.name} value={nic.name}>
          {nic.name}{nic.addresses.length ? ` — ${nic.addresses.join(', ')}` : ''}
          {!nic.up ? ' [down]' : ''}{!nic.multicast ? ' [no-mcast]' : ''}
        </option>
      ))}
    </select>
  );
}
