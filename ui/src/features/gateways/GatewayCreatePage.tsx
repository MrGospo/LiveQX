import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, Plus, Trash2, Shield } from 'lucide-react';
import { useCreateGateway } from '@/api/queries/gateways';
import { useNetworkInterfaces } from '@/api/queries/system';
import type { GatewayConfig, GatewayStatus, NetworkInterface } from '@/api/types';
import { useToast } from '@/hooks/useToast';
import ProbeStreamPanel from './ProbeStreamPanel';

type GwMode = NonNullable<GatewayStatus['mode']>;

// ─── Draft shapes (mirror GatewayConfig; strings so inputs bind cleanly) ───

interface InputDraft {
  address: string;
  port: string;
  iface: string;
  recv_buffer_kb: string;
}
const emptyInput = (addr = '0.0.0.0'): InputDraft => ({
  address: addr, port: '5000', iface: '', recv_buffer_kb: '1024',
});

interface OutputDraft {
  address: string;
  port: string;
  ttl: string;
  send_buffer_kb: string;
  iface: string;
  id: string;
}
const emptyOutput = (): OutputDraft => ({
  address: '239.0.0.1', port: '5000', ttl: '16',
  send_buffer_kb: '256', iface: '', id: '',
});

interface DemuxRouteDraft { service_id: string; output_id: string; }

interface RemuxDraft {
  transport_stream_id: string;
  original_network_id: string;
  target_bitrate_bps:  string;
}
const emptyRemux = (): RemuxDraft => ({
  transport_stream_id: '1', original_network_id: '1', target_bitrate_bps: '0',
});

interface TranscodeDraft {
  video_bitrate_bps: string;
  audio_bitrate_bps: string;
  service_id:        string;
  service_name:      string;
  provider_name:     string;
}
const emptyTranscode = (): TranscodeDraft => ({
  video_bitrate_bps: '4000000', audio_bitrate_bps: '128000',
  service_id: '1', service_name: 'Transcoded', provider_name: 'streaming-core',
});

interface FecDraft {
  enabled: boolean;
  mode:    '1d' | '2d';
  L:       string;
  D:       string;
}
const emptyFec = (): FecDraft => ({ enabled: false, mode: '1d', L: '8', D: '8' });

type TabId = 'general' | 'input' | 'output' | 'mode' | 'fec';

export default function GatewayCreatePage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();
  const { mutate: create, isPending } = useCreateGateway();
  const { data: ifaces = [] } = useNetworkInterfaces();

  const [tab, setTab] = React.useState<TabId>('general');

  const [mode, setMode]           = React.useState<GwMode>('passthrough');
  const [name, setName]           = React.useState('');
  const [autostart, setAutostart] = React.useState(false);
  const [inputs, setInputs]       = React.useState<InputDraft[]>([emptyInput()]);
  const [outputs, setOutputs]     = React.useState<OutputDraft[]>([emptyOutput()]);
  const [routes, setRoutes]       = React.useState<DemuxRouteDraft[]>([]);
  const [remux, setRemux]         = React.useState<RemuxDraft>(emptyRemux());
  const [transcode, setTranscode] = React.useState<TranscodeDraft>(emptyTranscode());
  const [fec, setFec]             = React.useState<FecDraft>(emptyFec());
  const [errors, setErrors]       = React.useState<string[]>([]);

  const setIn  = (i: number, p: Partial<InputDraft>) =>
    setInputs(arr => arr.map((x, idx) => idx === i ? { ...x, ...p } : x));
  const setOut = (i: number, p: Partial<OutputDraft>) =>
    setOutputs(arr => arr.map((o, idx) => idx === i ? { ...o, ...p } : o));
  const addIn      = () => setInputs(arr  => [...arr, emptyInput()]);
  const removeIn   = (i: number) => setInputs(arr => arr.filter((_, idx) => idx !== i));
  const addOut     = () => setOutputs(arr => [...arr, emptyOutput()]);
  const removeOut  = (i: number) => setOutputs(arr => arr.filter((_, idx) => idx !== i));
  const addRoute   = () => setRoutes(arr  => [...arr, { service_id: '1', output_id: '' }]);
  const setRoute   = (i: number, p: Partial<DemuxRouteDraft>) =>
    setRoutes(arr => arr.map((r, idx) => idx === i ? { ...r, ...p } : r));
  const removeRoute = (i: number) => setRoutes(arr => arr.filter((_, idx) => idx !== i));

  // Remux needs ≥2 inputs; other modes collapse to 1.
  React.useEffect(() => {
    setInputs(arr => {
      if (mode === 'remux')
        return arr.length >= 2 ? arr : [...arr, ...Array(2 - arr.length).fill(0).map(() => emptyInput('0.0.0.0'))];
      return arr.length >= 1 ? [arr[0]] : [emptyInput()];
    });
  }, [mode]);

  const validate = (): string[] => {
    const e: string[] = [];
    inputs.forEach((inp, idx) => {
      const tag = mode === 'remux' ? `${t('gateways.form.inputN', { n: idx + 1 })}: ` : '';
      if (!inp.address.trim()) e.push(`${tag}${t('gateways.form.validation.addressRequired')}`);
      const ip = parseInt(inp.port, 10);
      if (isNaN(ip) || ip < 1 || ip > 65535) e.push(`${tag}${t('gateways.form.validation.portRange')}`);
    });
    if (!outputs.length) e.push(t('gateways.form.validation.outputsRequired'));
    if ((mode === 'remux' || mode === 'transcode') && outputs.length !== 1)
      e.push(t('gateways.form.validation.singleOutputRequired'));
    outputs.forEach((o, idx) => {
      if (!o.address.trim()) e.push(`#${idx + 1}: ${t('gateways.form.validation.addressRequired')}`);
      const op = parseInt(o.port, 10);
      if (isNaN(op) || op < 1 || op > 65535) e.push(`#${idx + 1}: ${t('gateways.form.validation.portRange')}`);
      const tt = parseInt(o.ttl, 10);
      if (isNaN(tt) || tt < 1 || tt > 255) e.push(`#${idx + 1}: ${t('gateways.form.validation.ttlRange')}`);
    });
    if (mode === 'demux') {
      if (!routes.length) e.push(t('gateways.form.validation.routesRequired'));
      routes.forEach((r, idx) => {
        const sid = parseInt(r.service_id, 10);
        if (isNaN(sid) || sid < 1 || sid > 65535)
          e.push(`route #${idx + 1}: ${t('gateways.form.validation.serviceIdRange')}`);
        if (!r.output_id.trim())
          e.push(`route #${idx + 1}: ${t('gateways.form.validation.outputIdRequired')}`);
      });
    }
    if (fec.enabled) {
      const L = parseInt(fec.L, 10);
      const D = parseInt(fec.D, 10);
      if (isNaN(L) || L < 1 || L > 20) e.push(t('gateways.form.validation.fecLRange'));
      if (isNaN(D) || D < 4 || D > 20) e.push(t('gateways.form.validation.fecDRange'));
      if (!isNaN(L) && !isNaN(D) && L * D > 100) e.push(t('gateways.form.validation.fecLD'));
    }
    return e;
  };

  const buildInputJson = (inp: InputDraft) => ({
    address: inp.address.trim(),
    port: parseInt(inp.port, 10),
    recv_buffer_kb: parseInt(inp.recv_buffer_kb, 10) || 1024,
    ...(inp.iface ? { interface: inp.iface } : {}),
  });

  const onSubmit = () => {
    const errs = validate();
    setErrors(errs);
    if (errs.length) {
      // Jump to first tab that has an issue so users don't hunt.
      const inputBad = errs.some(x => x.toLowerCase().includes('адрес') || x.toLowerCase().includes('address') || x.toLowerCase().includes('порт') || x.toLowerCase().includes('port'));
      if (inputBad) setTab('input');
      return;
    }
    const body: GatewayConfig = {
      name: name.trim() || undefined,
      autostart,
      ...(mode !== 'passthrough' ? { mode } : {}),
      ...(mode === 'remux'
          ? { inputs: inputs.map(buildInputJson) }
          : { input: buildInputJson(inputs[0]) }),
      outputs: outputs.map(o => ({
        address: o.address.trim(),
        port: parseInt(o.port, 10),
        ttl: parseInt(o.ttl, 10),
        send_buffer_kb: parseInt(o.send_buffer_kb, 10) || 256,
        ...(o.iface ? { interface: o.iface } : {}),
        ...(o.id.trim() ? { id: o.id.trim() } : {}),
      })),
    };
    if (mode === 'demux') {
      body.demux = {
        routes: routes.map(r => ({
          service_id: parseInt(r.service_id, 10),
          output_id:  r.output_id.trim(),
        })),
      };
    } else if (mode === 'remux') {
      const tbps = parseInt(remux.target_bitrate_bps, 10);
      body.remux = {
        transport_stream_id: parseInt(remux.transport_stream_id, 10) || 1,
        original_network_id: parseInt(remux.original_network_id, 10) || 1,
        ...(tbps > 0 ? { target_bitrate_bps: tbps } : {}),
      };
    } else if (mode === 'transcode') {
      body.transcode = {
        video_bitrate_bps: parseInt(transcode.video_bitrate_bps, 10) || 4_000_000,
        audio_bitrate_bps: parseInt(transcode.audio_bitrate_bps, 10) || 128_000,
        service_id:        parseInt(transcode.service_id, 10) || 1,
        service_name:      transcode.service_name.trim() || 'Transcoded',
        provider_name:     transcode.provider_name.trim() || 'streaming-core',
      };
    }
    if (fec.enabled) {
      body.fec = {
        enabled: true,
        mode:    fec.mode,
        L:       parseInt(fec.L, 10) || 8,
        D:       parseInt(fec.D, 10) || 8,
      };
    }
    create(body, {
      onSuccess: () => {
        toast(t('gateways.created'), 'success');
        navigate('/gateways');
      },
      onError: (err) => {
        const msg = err instanceof Error ? err.message : t('common.error');
        toast(msg, 'danger');
      },
    });
  };

  const tabs: { id: TabId; label: string }[] = [
    { id: 'general', label: t('gateways.tabs.general') },
    { id: 'input',   label: mode === 'remux' ? t('gateways.tabs.inputs') : t('gateways.tabs.input') },
    { id: 'output',  label: t('gateways.tabs.outputs') },
    ...(mode !== 'passthrough' ? [{ id: 'mode' as TabId, label: t(`gateways.tabs.mode_${mode}`) }] : []),
    { id: 'fec',     label: t('gateways.tabs.fec') },
  ];

  return (
    <div className="p-7 flex flex-col gap-5 max-w-4xl">
      <div className="flex items-center gap-3">
        <button onClick={() => navigate('/gateways')}
          className="p-1.5 text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
          <ChevronLeft size={18} />
        </button>
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('gateways.createTitle')}</h1>
      </div>

      {/* Tabs */}
      <div className="flex gap-1 border-b border-[var(--border-subtle)]">
        {tabs.map(tb => {
          const active = tab === tb.id;
          return (
            <button key={tb.id} type="button" onClick={() => setTab(tb.id)}
              className={`px-4 py-2 text-sm font-medium border-b-2 -mb-px transition-colors ${
                active
                  ? 'border-[var(--accent)] text-[var(--accent)]'
                  : 'border-transparent text-[var(--text-muted)] hover:text-[var(--text-primary)]'
              }`}>
              {tb.label}
            </button>
          );
        })}
      </div>

      <div className="flex flex-col gap-5">
        {tab === 'general' && (
          <>
            <div className="border border-[var(--border-subtle)] rounded-lg p-4 grid grid-cols-2 gap-4">
              <Field label={t('gateways.name')}>
                <input value={name} onChange={e => setName(e.target.value)}
                  placeholder={t('gateways.namePlaceholder')} className={inputCls} />
              </Field>
              <Field label={t('gateways.autostart')} hint={t('gateways.form.autostartHint')}>
                <label className="flex items-center gap-2 cursor-pointer h-10">
                  <div onClick={() => setAutostart(v => !v)}
                    className={`w-9 h-5 rounded-full relative transition-colors ${autostart ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
                    <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${autostart ? 'left-4' : 'left-0.5'}`} />
                  </div>
                  <span className="text-sm text-[var(--text-primary)]">
                    {autostart ? t('common.yes') : t('common.no')}
                  </span>
                </label>
              </Field>
            </div>

            <div className="border border-[var(--border-subtle)] rounded-lg p-4">
              <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] mb-3 font-semibold">
                {t('gateways.form.modeSection')}
              </div>
              <div className="grid grid-cols-4 gap-2 mb-3">
                {(['passthrough','demux','remux','transcode'] as GwMode[]).map(m => (
                  <button key={m} type="button" onClick={() => setMode(m)}
                    className={`px-3 py-2 text-xs rounded-md border transition-colors ${
                      mode === m
                        ? 'bg-[var(--accent)]/15 border-[var(--accent)] text-[var(--accent)]'
                        : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
                    }`}>
                    {t(`gateways.mode.${m}`)}
                  </button>
                ))}
              </div>
              <div className="bg-canvas rounded-md p-3 border border-[var(--border-subtle)]">
                <p className="text-sm text-[var(--text-primary)] font-medium mb-1">
                  {t(`gateways.form.modeTitle.${mode}`)}
                </p>
                <p className="text-[13px] text-[var(--text-muted)] leading-relaxed">
                  {t(`gateways.form.modePlain.${mode}`)}
                </p>
              </div>
            </div>
          </>
        )}

        {tab === 'input' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="flex items-center justify-between mb-3">
              <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] font-semibold">
                {mode === 'remux'
                  ? t('gateways.form.inputsSection')
                  : t('gateways.form.inputSection')}
              </div>
              {mode === 'remux' && (
                <button type="button" onClick={addIn}
                  className="flex items-center gap-1 px-2 py-1 text-xs border border-[var(--border-subtle)] text-[var(--text-muted)] rounded hover:text-[var(--text-primary)] transition-colors">
                  <Plus size={12} /> {t('gateways.form.addInput')}
                </button>
              )}
            </div>
            <div className="flex flex-col gap-3">
              {inputs.map((inp, i) => (
                <div key={i} className={mode === 'remux' ? 'bg-canvas rounded-lg p-3 border border-[var(--border-subtle)]' : ''}>
                  {mode === 'remux' && (
                    <div className="flex items-center justify-between mb-2">
                      <span className="text-xs font-semibold text-[var(--text-muted)]">
                        {t('gateways.form.inputN', { n: i + 1 })}
                      </span>
                      {inputs.length > 2 && (
                        <button type="button" onClick={() => removeIn(i)}
                          className="text-xs text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors flex items-center gap-1">
                          <Trash2 size={12} /> {t('gateways.form.removeInput')}
                        </button>
                      )}
                    </div>
                  )}
                  <div className="grid grid-cols-3 gap-3">
                    <Field label={t('gateways.form.address')} hint={i === 0 ? t('gateways.form.addressInputHint') : undefined} span={2}>
                      <input value={inp.address} onChange={e => setIn(i, { address: e.target.value })}
                        placeholder="0.0.0.0 / 239.0.0.1" className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.port')}>
                      <input type="number" min={1} max={65535} value={inp.port}
                        onChange={e => setIn(i, { port: e.target.value })} className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.iface')} hint={i === 0 ? t('gateways.form.ifaceHint') : undefined} span={2}>
                      <NicSelect value={inp.iface} onChange={(v) => setIn(i, { iface: v })} ifaces={ifaces} />
                    </Field>
                    <Field label={t('gateways.form.recvBuffer')}>
                      <input type="number" min={64} value={inp.recv_buffer_kb}
                        onChange={e => setIn(i, { recv_buffer_kb: e.target.value })} className={inputCls} />
                    </Field>
                  </div>
                  <ProbeStreamPanel
                    address={inp.address}
                    port={inp.port}
                    interfaceName={inp.iface}
                  />
                </div>
              ))}
            </div>
          </div>
        )}

        {tab === 'output' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="flex items-center justify-between mb-3">
              <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] font-semibold">
                {t('gateways.form.outputsSection')}
              </div>
              <button type="button" onClick={addOut}
                className="flex items-center gap-1 px-2 py-1 text-xs border border-[var(--border-subtle)] text-[var(--text-muted)] rounded hover:text-[var(--text-primary)] transition-colors">
                <Plus size={12} /> {t('gateways.form.addOutput')}
              </button>
            </div>
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
                    <Field label={t('gateways.form.address')} hint={i === 0 ? t('gateways.form.multicastHint') : undefined} span={2}>
                      <input value={o.address} onChange={e => setOut(i, { address: e.target.value })}
                        placeholder="239.0.0.1" className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.port')}>
                      <input type="number" min={1} max={65535} value={o.port}
                        onChange={e => setOut(i, { port: e.target.value })} className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.ttl')}>
                      <input type="number" min={1} max={255} value={o.ttl}
                        onChange={e => setOut(i, { ttl: e.target.value })} className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.sendBuffer')}>
                      <input type="number" min={64} value={o.send_buffer_kb}
                        onChange={e => setOut(i, { send_buffer_kb: e.target.value })} className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.outputId')}>
                      <input value={o.id} onChange={e => setOut(i, { id: e.target.value })}
                        placeholder="dvb-out-1" className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.iface')} span={3}>
                      <NicSelect value={o.iface} onChange={(v) => setOut(i, { iface: v })} ifaces={ifaces} />
                    </Field>
                  </div>
                </div>
              ))}
            </div>
          </div>
        )}

        {tab === 'mode' && mode === 'demux' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="flex items-center justify-between mb-3">
              <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] font-semibold">
                {t('gateways.form.demuxSection')}
              </div>
              <button type="button" onClick={addRoute}
                className="flex items-center gap-1 px-2 py-1 text-xs border border-[var(--border-subtle)] text-[var(--text-muted)] rounded hover:text-[var(--text-primary)] transition-colors">
                <Plus size={12} /> {t('gateways.form.addRoute')}
              </button>
            </div>
            <p className="text-[13px] text-[var(--text-muted)] leading-relaxed mb-3">
              {t('gateways.form.demuxPlain')}
            </p>
            {routes.length === 0 ? (
              <div className="text-xs text-[var(--text-muted)] italic">
                {t('gateways.form.demuxEmpty')}
              </div>
            ) : (
              <div className="flex flex-col gap-2">
                {routes.map((r, i) => (
                  <div key={i} className="flex items-end gap-2">
                    <Field label={t('gateways.form.serviceId')}>
                      <input type="number" min={1} max={65535} value={r.service_id}
                        onChange={e => setRoute(i, { service_id: e.target.value })} className={inputCls} />
                    </Field>
                    <Field label={t('gateways.form.outputIdLink')}>
                      <select value={r.output_id} onChange={e => setRoute(i, { output_id: e.target.value })} className={inputCls}>
                        <option value="">—</option>
                        {outputs.map((o, idx) => {
                          const fallbackId = `out${idx}`;
                          const id = o.id.trim() || fallbackId;
                          return (
                            <option key={idx} value={id}>
                              {id} ({o.address}:{o.port})
                            </option>
                          );
                        })}
                      </select>
                    </Field>
                    <button type="button" onClick={() => removeRoute(i)}
                      className="p-2 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors mb-[2px]">
                      <Trash2 size={14} />
                    </button>
                  </div>
                ))}
              </div>
            )}
          </div>
        )}

        {tab === 'mode' && mode === 'remux' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] mb-3 font-semibold">
              {t('gateways.form.remuxSection')}
            </div>
            <p className="text-[13px] text-[var(--text-muted)] leading-relaxed mb-3">
              {t('gateways.form.remuxPlain')}
            </p>
            <div className="grid grid-cols-3 gap-3">
              <Field label={t('gateways.form.transportStreamId')}>
                <input type="number" min={0} max={65535} value={remux.transport_stream_id}
                  onChange={e => setRemux(s => ({ ...s, transport_stream_id: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.originalNetworkId')}>
                <input type="number" min={0} max={65535} value={remux.original_network_id}
                  onChange={e => setRemux(s => ({ ...s, original_network_id: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.targetBitrate')} hint={t('gateways.form.targetBitrateHint')}>
                <input type="number" min={0} value={remux.target_bitrate_bps}
                  onChange={e => setRemux(s => ({ ...s, target_bitrate_bps: e.target.value }))} className={inputCls} />
              </Field>
            </div>
          </div>
        )}

        {tab === 'mode' && mode === 'transcode' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] mb-3 font-semibold">
              {t('gateways.form.transcodeSection')}
            </div>
            <p className="text-[13px] text-[var(--text-muted)] leading-relaxed mb-3">
              {t('gateways.form.transcodePlain')}
            </p>
            <div className="grid grid-cols-2 gap-3">
              <Field label={t('gateways.form.videoBitrate')}>
                <input type="number" min={1} value={transcode.video_bitrate_bps}
                  onChange={e => setTranscode(s => ({ ...s, video_bitrate_bps: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.audioBitrate')}>
                <input type="number" min={1} value={transcode.audio_bitrate_bps}
                  onChange={e => setTranscode(s => ({ ...s, audio_bitrate_bps: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.serviceId')}>
                <input type="number" min={0} max={65535} value={transcode.service_id}
                  onChange={e => setTranscode(s => ({ ...s, service_id: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.serviceName')}>
                <input value={transcode.service_name}
                  onChange={e => setTranscode(s => ({ ...s, service_name: e.target.value }))} className={inputCls} />
              </Field>
              <Field label={t('gateways.form.providerName')} span={2}>
                <input value={transcode.provider_name}
                  onChange={e => setTranscode(s => ({ ...s, provider_name: e.target.value }))} className={inputCls} />
              </Field>
            </div>
            <p className="text-[11px] text-[var(--text-muted)] leading-snug mt-3">
              {t('gateways.form.transcodeHint')}
            </p>
          </div>
        )}

        {tab === 'fec' && (
          <div className="border border-[var(--border-subtle)] rounded-lg p-4">
            <div className="flex items-center gap-2 mb-3">
              <Shield size={14} className="text-[var(--text-muted)]" />
              <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] font-semibold">
                {t('gateways.form.fecSection')}
              </div>
              <div className="flex-1" />
              <label className="flex items-center gap-2 cursor-pointer">
                <div onClick={() => setFec(s => ({ ...s, enabled: !s.enabled }))}
                  className={`w-9 h-5 rounded-full relative transition-colors ${fec.enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
                  <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${fec.enabled ? 'left-4' : 'left-0.5'}`} />
                </div>
                <span className="text-xs text-[var(--text-primary)]">
                  {fec.enabled ? t('common.yes') : t('common.no')}
                </span>
              </label>
            </div>
            <p className="text-[13px] text-[var(--text-muted)] leading-relaxed mb-3">
              {t('gateways.form.fecPlain')}
            </p>
            {mode === 'passthrough' && (
              <p className="text-[11px] text-[var(--warning)] leading-snug mb-2">
                {t('gateways.form.fecPassthroughWarn')}
              </p>
            )}
            {fec.enabled && (
              <div className="grid grid-cols-3 gap-3">
                <Field label={t('gateways.form.fecMode')}>
                  <select value={fec.mode} onChange={e => setFec(s => ({ ...s, mode: e.target.value as '1d' | '2d' }))} className={inputCls}>
                    <option value="1d">{t('gateways.fec.mode1d')}</option>
                    <option value="2d">{t('gateways.fec.mode2d')}</option>
                  </select>
                </Field>
                <Field label="L" hint={t('gateways.form.fecLHint')}>
                  <input type="number" min={1} max={20} value={fec.L}
                    onChange={e => setFec(s => ({ ...s, L: e.target.value }))} className={inputCls} />
                </Field>
                <Field label="D" hint={t('gateways.form.fecDHint')}>
                  <input type="number" min={4} max={20} value={fec.D}
                    onChange={e => setFec(s => ({ ...s, D: e.target.value }))} className={inputCls} />
                </Field>
              </div>
            )}
          </div>
        )}
      </div>

      {errors.length > 0 && (
        <div className="bg-[var(--danger)]/10 border border-[var(--danger)]/40 rounded-md p-3 flex flex-col gap-1">
          {errors.map((er, i) => (
            <p key={i} className="text-xs text-[var(--danger)]">• {er}</p>
          ))}
        </div>
      )}

      <div className="flex gap-2 justify-end sticky bottom-0 bg-canvas py-3">
        <button onClick={() => navigate('/gateways')} disabled={isPending}
          className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
          {t('common.cancel')}
        </button>
        <button disabled={isPending} onClick={onSubmit}
          className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {isPending ? `${t('gateways.create')}…` : t('gateways.createSubmit')}
        </button>
      </div>
    </div>
  );
}

// ─── Shared primitives (local — layout differs from channels' Field) ─────

const inputCls =
  'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] w-full';

function Field({
  label, hint, children, span,
}: {
  label: string;
  hint?: string;
  children: React.ReactNode;
  span?: 1 | 2 | 3;
}) {
  const cls = span === 3 ? 'col-span-3' : span === 2 ? 'col-span-2' : '';
  return (
    <div className={`flex flex-col gap-1 ${cls}`}>
      <label className="text-xs font-medium text-[var(--text-primary)]">{label}</label>
      {children}
      {hint && <p className="text-[11px] text-[var(--text-muted)] leading-snug">{hint}</p>}
    </div>
  );
}

function NicSelect({
  value, onChange, ifaces,
}: {
  value: string;
  onChange: (v: string) => void;
  ifaces: NetworkInterface[];
}) {
  const { t } = useTranslation();
  return (
    <select value={value} onChange={e => onChange(e.target.value)} className={inputCls}>
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
