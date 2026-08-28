import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { z } from 'zod';
import { ChevronLeft } from 'lucide-react';
import { useCreateChannel } from '@/api/queries/channels';
import { useGpuInfo, useNetworkInterfaces } from '@/api/queries/system';
import { Block } from '@/components/Block';
import { FormErrorBanner } from '@/components/FormErrorBanner';
import { FolderPickerModal } from '@/components/FolderPickerModal';
import { FilePickerModal } from '@/components/FilePickerModal';
import { useToast } from '@/hooks/useToast';
import { Folder, FileText } from 'lucide-react';

const FALLBACK_EXTS = ['.png', '.jpg', '.jpeg', '.webp', '.bmp'];

const createChannelSchema = z.object({
  name: z
    .string()
    .min(1, 'Required')
    .max(64)
    .regex(/^[a-zA-Z0-9_-]+$/, 'Letters, digits, _ and - only'),
  numa_node: z.coerce.number().int().min(0).max(7).default(0),
  encoder_mode: z.enum(['auto', 'cpu', 'nvenc', 'qsv', 'vaapi']).default('auto'),
  gpu_index: z.coerce.number().int().min(0).default(0),
  video_codec: z.enum(['h264', 'mpeg2video']).default('h264'),
  audio_codec: z.enum(['aac', 'mp2']).default('aac'),
  // Audio bitrate and sample rate — Encoder::Config defaults are 128 kbps / 48 kHz.
  // Backend caps applied elsewhere; UI just surfaces knobs the Detail page has.
  audio_bitrate_kbps: z.coerce.number().int().min(32).max(512).default(128),
  audio_sample_rate: z.coerce.number().int().refine(v => [44100, 48000].includes(v)).default(48000),
  resolution: z.string().regex(/^\d+x\d+$/, 'e.g. 1920x1080').default('1920x1080'),
  fps: z.coerce.number().int().min(1).max(120).default(25),
  bitrate_kbps: z.coerce.number().int().min(100).max(100_000).default(4000),
  // Rate-control mode. cbr — DVB / MPEG-TS multicast requirement (HRD-compliant).
  // vbr — average with a peak ceiling; only meaningful for unicast (HLS/RTMP).
  // crf — quality target (x264 only). GPU / mpeg2video backends downgrade CRF
  // to VBR and log a warning; see IVideoEncoder::Config comments.
  bitrate_mode: z.enum(['cbr', 'vbr', 'crf']).default('cbr'),
  // VBR peak in kbps (bps on the wire). 0 = auto (encoder picks 1.5×bitrate).
  // Only meaningful when bitrate_mode == "vbr".
  bitrate_max_kbps: z.coerce.number().int().min(0).max(200_000).default(0),
  // Constant-rate-factor quality target. 0 = libx264 default (23). Sensible
  // 18..28. Only meaningful when bitrate_mode == "crf" (x264).
  crf: z.coerce.number().int().min(0).max(51).default(0),
  // 0..16 — verbatim to backend (no silent clamp). See Encoder::Config::max_b_frames.
  max_b_frames: z.coerce.number().int().min(0).max(16).default(0),
  // 0 = per-backend auto (fps for H.264, ~fps/2 for MPEG-2). Positive
  // values are honored verbatim. Upper bound of 600 matches ChannelInstance.
  gop_size: z.coerce.number().int().min(0).max(600).default(0),
  // H.264 profile hint. "" = let the encoder pick. Ignored by mpeg2video
  // in the backend but harmless to send.
  h264_profile: z
    .enum(['', 'baseline', 'main', 'high', 'high10', 'high422', 'high444'])
    .default(''),
  // AVCC level integer. 0 = auto; 30/31/40/41/50/51/52/etc. Bounds match
  // ChannelInstance's [10..62] validation.
  h264_level: z.coerce.number().int().min(0).max(62).default(0),
  // MPEG-2 profile hint. "" = encoder default (MP@ML for the mpeg2video
  // backend). Ignored by H.264 backends.
  mpeg2_profile: z
    .enum(['', 'simple', 'main', 'high', '422'])
    .default(''),
  // MPEG-2 level ordinal. 0 = auto; LOW=10, MAIN=8, HIGH_1440=6, HIGH=4.
  // Counter-intuitively, lower ordinal = higher capability.
  mpeg2_level: z.coerce.number().int().refine(v => v === 0 || v === 4 || v === 6 || v === 8 || v === 10, {
    message: 'mpeg2_level must be 0, 4, 6, 8, or 10',
  }).default(0),
  preset: z
    .enum(['ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium', 'slow', 'veryslow'])
    .default('veryfast'),
  default_photo_duration: z.coerce.number().min(0.1).default(10),
  fallback_image_path: z.string().optional(),
  // Content source: mode picks payload shape (see buildContentSource). Paths
  // are per-mode — passthrough uses source_path; cache uses share_path + optional
  // cache_path (backend auto-resolves cache_path when empty).
  content_mode: z.enum(['none', 'passthrough', 'cache']).default('none'),
  content_source_path: z.string().default(''),
  content_share_path: z.string().default(''),
  content_cache_path: z.string().default(''),
  scan_interval_ms: z.coerce.number().int().min(100).default(2000),
  // First output
  output_type: z.enum(['srt', 'multicast', 'rtmp', 'hls']).default('srt'),
  output_id: z.string().min(1).default('main'),
  output_address: z.string().default(''),
  output_port: z.coerce.number().int().min(1).max(65535).default(4000),
  output_url: z.string().default(''),
  output_dir: z.string().default(''),
  // Source-NIC bind (srt/multicast/rtmp). Empty = OS default route.
  output_bind_address: z.string().default(''),
  // Multicast hop limit (IP_MULTICAST_TTL). Backend range 1..255, default 16.
  output_ttl: z.coerce.number().int().min(1).max(255).default(16),
  // Playback log sink. 'none' → no playback_log block sent (backend defaults
  // to NullSink). 'file'/'db' emit the block; retention_days only matters
  // for 'db' (SQLite auto-purge).
  log_sink: z.enum(['none', 'file', 'db']).default('none'),
  log_retention_days: z.coerce.number().int().min(0).max(3650).default(90),
  // MPEG-TS IPTV knobs — only surfaced for multicast output in the wizard.
  // Multiple channels sharing a multicast subnet MUST have distinct
  // service_id, or middleware collapses them into one program. Defaults for
  // TSID/ONID/mux_rate/periods mirror Encoder::Config so cfg.json matches
  // the values user actually saw in the wizard (no hidden magic).
  mpegts_service_name: z.string().default(''),
  mpegts_service_provider: z.string().default('LiveQX'),
  mpegts_service_id: z.coerce.number().int().min(1).max(65535).default(1),
  mpegts_transport_stream_id: z.coerce.number().int().min(1).max(65535).default(1),
  mpegts_original_network_id: z.coerce.number().int().min(1).max(65535).default(1),
  // 0 = VBR (backend default). >0 = constant mux rate in kbps → sent as
  // bit/s to backend. Recommended: sum(video+audio) * 1.15.
  mpegts_mux_rate_kbps: z.coerce.number().int().min(0).max(200_000).default(0),
  // 0 = FFmpeg defaults (SDT=500 ms, PAT/PMT=100 ms).
  mpegts_sdt_period_ms: z.coerce.number().int().min(0).max(10_000).default(0),
  mpegts_pat_period_ms: z.coerce.number().int().min(0).max(10_000).default(0),
});

type FormValues = z.infer<typeof createChannelSchema>;

export default function CreateChannelPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();
  const [step, setStep] = React.useState<1 | 2>(1);

  const { mutateAsync: createChannel, isPending } = useCreateChannel();
  const { data: gpuInfo } = useGpuInfo();

  const bestEncoder = React.useMemo(() => {
    if (!gpuInfo) return 'x264';
    if (gpuInfo.nvenc?.built_in && gpuInfo.nvenc?.codec_registered) return 'nvenc';
    if (gpuInfo.qsv?.built_in  && gpuInfo.qsv?.codec_registered)   return 'qsv';
    if (gpuInfo.vaapi?.built_in && gpuInfo.vaapi?.codec_registered) return 'vaapi';
    return 'x264';
  }, [gpuInfo]);

  const { register, handleSubmit, watch, setValue, formState: { errors } } = useForm<FormValues>({
    resolver: zodResolver(createChannelSchema),
    defaultValues: {
      name: '', numa_node: 0, encoder_mode: 'auto', gpu_index: 0, video_codec: 'h264',
      audio_codec: 'aac',
      audio_bitrate_kbps: 128, audio_sample_rate: 48000,
      resolution: '1920x1080', fps: 25, bitrate_kbps: 4000,
      bitrate_mode: 'cbr', bitrate_max_kbps: 0, crf: 0,
      max_b_frames: 0, gop_size: 0,
      h264_profile: '', h264_level: 0,
      mpeg2_profile: '', mpeg2_level: 0,
      preset: 'veryfast',
      default_photo_duration: 10,
      fallback_image_path: '',
      content_mode: 'none', content_source_path: '', content_share_path: '', content_cache_path: '',
      scan_interval_ms: 2000,
      output_type: 'srt', output_id: 'main', output_address: '0.0.0.0',
      output_port: 4000, output_url: '', output_dir: '',
      output_bind_address: '', output_ttl: 16,
      log_sink: 'none', log_retention_days: 90,
      mpegts_service_name: '', mpegts_service_provider: 'LiveQX',
      mpegts_service_id: 1, mpegts_transport_stream_id: 1, mpegts_original_network_id: 1,
      mpegts_mux_rate_kbps: 0, mpegts_sdt_period_ms: 0, mpegts_pat_period_ms: 0,
    },
  });

  const outputType   = watch('output_type');
  const outputDir    = watch('output_dir');
  const fallbackPath = watch('fallback_image_path');
  const outputBind   = watch('output_bind_address');
  const logSink      = watch('log_sink');
  const videoCodec   = watch('video_codec');
  const bitrateMode  = watch('bitrate_mode');
  const contentMode  = watch('content_mode');
  const contentSrc   = watch('content_source_path');
  const contentShare = watch('content_share_path');
  const contentCache = watch('content_cache_path');
  const [pickerFor, setPickerFor] = React.useState<
    'content_source_path' | 'content_share_path' | 'content_cache_path' | 'output_dir' | null
  >(null);
  const [filePickerFor, setFilePickerFor] = React.useState<'fallback_image_path' | null>(null);

  // NIC dropdown source for the first output. Mirrors OutputFormModal: skip
  // loopback, keep an unresolved bind as an "(offline)" entry so the picker
  // never silently reverts to default.
  const nics = useNetworkInterfaces();
  const nicOptions = React.useMemo(() => {
    const live = (nics.data ?? [])
      .filter(n => n.up && !n.loopback)
      .flatMap(n => n.addresses.map(a => ({ value: a, label: `${n.name} — ${a}` })));
    if (outputBind && !live.some(o => o.value === outputBind)) {
      live.push({ value: outputBind, label: `${outputBind} (offline)` });
    }
    return live;
  }, [nics.data, outputBind]);

  // Build content_source block. Backend infers mode from which fields are
  // present, so we send the minimal shape per mode. Empty cache_path in
  // 'cache' mode is intentional — backend auto-resolves to {channel_dir}/cache.
  const buildContentSource = (v: FormValues): Record<string, unknown> | null => {
    if (v.content_mode === 'none') return null;
    if (v.content_mode === 'passthrough') {
      if (!v.content_source_path) return null;
      return { source_path: v.content_source_path, scan_interval_ms: v.scan_interval_ms };
    }
    if (!v.content_share_path) return null;
    return {
      share_path: v.content_share_path,
      ...(v.content_cache_path ? { cache_path: v.content_cache_path } : {}),
      scan_interval_ms: v.scan_interval_ms,
    };
  };

  const buildOutput = (v: FormValues) => {
    const base: Record<string, unknown> = { id: v.output_id, type: v.output_type, enabled: true };
    // bind_address only for transports that honour it, and only when non-empty
    // (empty = kernel default; sending "" would clutter persisted cfg).
    const withBind = (extra: Record<string, unknown>) =>
      v.output_bind_address ? { ...base, ...extra, bind_address: v.output_bind_address }
                            : { ...base, ...extra };
    switch (v.output_type) {
      case 'srt':       return withBind({ address: v.output_address, port: v.output_port });
      case 'multicast': return withBind({ address: v.output_address, port: v.output_port, ttl: v.output_ttl });
      case 'rtmp':      return withBind({ url: v.output_url });
      case 'hls':       return { ...base, dir: v.output_dir };
      default:          return base;
    }
  };

  const onSubmit = handleSubmit(
    async (v) => {
      try {
        // ChannelConfigRequest is flat: ChannelInstance::buildLongLived reads
        // resolution/fps/bitrate/preset/encoder_mode/gpu_index from the top
        // level. Backend expects bitrate in bits/sec, not kbps.
        const contentSource = buildContentSource(v);
        // Audio subobject — send codec + bitrate/sample_rate whenever any
        // audio knob is non-default. Sending the full block matches Detail
        // page semantics and makes cfg.json self-documenting.
        const audioTouched = v.audio_codec !== 'aac'
          || v.audio_bitrate_kbps !== 128
          || v.audio_sample_rate !== 48000;
        const audioBlock = audioTouched
          ? { audio: {
                codec: v.audio_codec,
                bitrate: v.audio_bitrate_kbps * 1000,
                sample_rate: v.audio_sample_rate,
              } }
          : {};
        // MPEG-TS subobject — only when output is multicast AND user touched
        // any field. Send the full block (all knobs user saw) to keep
        // cfg.json honest about the picked identity/mux profile.
        const mpegtsTouched = v.mpegts_service_id !== 1
          || v.mpegts_service_name.trim().length > 0
          || v.mpegts_service_provider !== 'LiveQX'
          || v.mpegts_transport_stream_id !== 1
          || v.mpegts_original_network_id !== 1
          || v.mpegts_mux_rate_kbps !== 0
          || v.mpegts_sdt_period_ms !== 0
          || v.mpegts_pat_period_ms !== 0;
        const mpegtsBlock = (v.output_type === 'multicast' && mpegtsTouched)
          ? { mpegts: {
                service_id: v.mpegts_service_id,
                service_provider: v.mpegts_service_provider,
                transport_stream_id: v.mpegts_transport_stream_id,
                original_network_id: v.mpegts_original_network_id,
                mux_rate: v.mpegts_mux_rate_kbps * 1000,
                sdt_period_ms: v.mpegts_sdt_period_ms,
                pat_period_ms: v.mpegts_pat_period_ms,
                ...(v.mpegts_service_name.trim()
                  ? { service_name: v.mpegts_service_name.trim() }
                  : {}),
              } }
          : {};
        const payload = {
          name: v.name,
          numa_node: v.numa_node,
          encoder_mode: v.encoder_mode,
          gpu_index: v.gpu_index,
          ...(v.video_codec !== 'h264' ? { video_codec: v.video_codec } : {}),
          // preset is an x264-only knob (Mpeg2VideoEncoder discards it) —
          // omit for mpeg2video so cfg.json stays honest about what applies.
          ...(v.video_codec === 'h264' ? { preset: v.preset } : {}),
          resolution: v.resolution,
          fps: v.fps,
          bitrate: v.bitrate_kbps * 1000,
          ...(v.max_b_frames !== 0 ? { max_b_frames: v.max_b_frames } : {}),
          ...(v.gop_size !== 0 ? { gop_size: v.gop_size } : {}),
          // Rate control. Omit at the default (cbr) so cfg.json stays clean;
          // send bitrate_max/crf only when they matter for the picked mode.
          // Backend converts kbps → bps for bitrate_max just like bitrate.
          ...(v.bitrate_mode !== 'cbr' ? { bitrate_mode: v.bitrate_mode } : {}),
          ...(v.bitrate_mode === 'vbr' && v.bitrate_max_kbps > 0
              ? { bitrate_max: v.bitrate_max_kbps * 1000 } : {}),
          ...(v.bitrate_mode === 'crf' && v.crf > 0 ? { crf: v.crf } : {}),
          // profile/level only meaningful for H.264; skip verbatim defaults
          // ("" / 0) so cfg.json omits them and downstream tooling doesn't
          // show phantom overrides.
          ...(v.video_codec === 'h264' && v.h264_profile !== ''
              ? { h264_profile: v.h264_profile } : {}),
          ...(v.video_codec === 'h264' && v.h264_level !== 0
              ? { h264_level: v.h264_level } : {}),
          // Same principle for MPEG-2 — omit when at defaults.
          ...(v.video_codec === 'mpeg2video' && v.mpeg2_profile !== ''
              ? { mpeg2_profile: v.mpeg2_profile } : {}),
          ...(v.video_codec === 'mpeg2video' && v.mpeg2_level !== 0
              ? { mpeg2_level: v.mpeg2_level } : {}),
          ...audioBlock,
          default_photo_duration: v.default_photo_duration,
          ...(v.fallback_image_path ? { fallback: { image_path: v.fallback_image_path } } : {}),
          ...(contentSource ? { content_source: contentSource } : {}),
          ...(v.log_sink !== 'none'
            ? { playback_log: v.log_sink === 'db'
                ? { sink: 'db', retention_days: v.log_retention_days }
                : { sink: 'file' } }
            : {}),
          ...mpegtsBlock,
          outputs: [buildOutput(v)],
        };
        const result = await createChannel(payload);
        toast(t('channels.createSuccess', { name: v.name }), 'success');
        navigate(`/channels/${result.id}`);
      } catch (e: unknown) {
        const msg = (e as { detail?: string; message?: string })?.detail ?? (e as Error)?.message ?? 'Error';
        toast(msg, 'danger');
      }
    },
    (errs) => {
      const first = Object.entries(errs)[0];
      const fieldName = first?.[0] ?? 'form';
      const fieldMsg  = (first?.[1] as { message?: string } | undefined)?.message ?? 'invalid';
      toast(`${fieldName}: ${fieldMsg}`, 'danger');
      if (errs.name || errs.numa_node || errs.bitrate_kbps
          || errs.audio_bitrate_kbps || errs.audio_sample_rate
          || errs.max_b_frames || errs.gop_size
          || errs.h264_profile || errs.h264_level
          || errs.mpeg2_profile || errs.mpeg2_level
          || errs.bitrate_mode || errs.bitrate_max_kbps || errs.crf) setStep(1);
    },
  );

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors w-full';
  const labelCls = 'text-sm font-medium text-[var(--text-primary)]';
  const errCls = 'text-xs text-[var(--danger)] mt-1';

  return (
    <div className="p-7 max-w-2xl flex flex-col gap-5">
      <button onClick={() => navigate('/channels')}
        className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
        <ChevronLeft size={15} /> {t('channels.title')}
      </button>

      <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('channels.create')}</h1>

      {/* Step indicator */}
      <div className="flex items-center gap-3">
        {([1, 2] as const).map((s) => (
          <React.Fragment key={s}>
            <div className={`flex items-center gap-1.5 ${step >= s ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>
              <div className={`w-6 h-6 rounded-full flex items-center justify-center text-xs font-bold transition-colors ${step >= s ? 'bg-[var(--accent)] text-white' : 'bg-surface2 text-[var(--text-muted)]'}`}>{s}</div>
              <span className="text-sm">{s === 1 ? t('channels.createStep1') : t('channels.createStep2')}</span>
            </div>
            {s < 2 && <div className="flex-1 h-px bg-[var(--border-subtle)]" />}
          </React.Fragment>
        ))}
      </div>

      <form onSubmit={onSubmit} className="flex flex-col gap-5">
        <FormErrorBanner errors={errors} />
        {step === 1 && (
          <Block>
            <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('channels.createStep1')}</h2>
            <div className="grid grid-cols-2 gap-4">
              {/* Name */}
              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldName')} *</label>
                <input {...register('name')} className={inputCls} placeholder="sport-hd" />
                {errors.name && <p className={errCls}>{errors.name.message}</p>}
                <p className="text-xs text-[var(--text-muted)]">{t('channels.nameImmutableHint')}</p>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldResolution')}</label>
                <select {...register('resolution')} className={inputCls}>
                  {['1920x1080','3840x2160','1280x720','720x576'].map(v => <option key={v} value={v}>{v}</option>)}
                </select>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldFps')}</label>
                <select {...register('fps')} className={inputCls}>
                  {[1,2,5,10,15,24,25,30,50,60].map(v => <option key={v} value={v}>{v}</option>)}
                </select>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldEncoder')}</label>
                <select {...register('encoder_mode')} className={inputCls}>
                  {['auto','cpu','nvenc','qsv','vaapi'].map(v => <option key={v} value={v}>{v}</option>)}
                </select>
                {gpuInfo && (
                  <p className="text-xs text-[var(--text-muted)] mt-0.5">
                    auto →{' '}
                    <span className={bestEncoder !== 'x264' ? 'text-[var(--success)]' : undefined}>
                      {bestEncoder}
                    </span>
                    {bestEncoder !== 'x264' && ' ✓'}
                  </p>
                )}
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldVideoCodec')}</label>
                <select {...register('video_codec')} className={inputCls}>
                  <option value="h264">H.264 (AVC)</option>
                  <option value="mpeg2video">MPEG-2 Video</option>
                </select>
                <p className="text-xs text-[var(--text-muted)]">{t('channels.config.videoCodecHint')}</p>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldAudioCodec')}</label>
                <select {...register('audio_codec')} className={inputCls}>
                  <option value="aac">AAC-LC</option>
                  <option value="mp2">MPEG-1 Layer II (MP2)</option>
                </select>
                <p className="text-xs text-[var(--text-muted)]">{t('channels.config.audioCodecHint')}</p>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldAudioBitrate')} (kbps)</label>
                <input {...register('audio_bitrate_kbps')} type="number" min={32} max={512} className={inputCls} />
                {errors.audio_bitrate_kbps && <p className={errCls}>{errors.audio_bitrate_kbps.message}</p>}
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldAudioSampleRate')}</label>
                <select {...register('audio_sample_rate')} className={inputCls}>
                  {[44100, 48000].map(r => <option key={r} value={r}>{r} Hz</option>)}
                </select>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldNuma')}</label>
                <input {...register('numa_node')} type="number" min={0} max={7} className={inputCls} />
                {errors.numa_node && <p className={errCls}>{errors.numa_node.message}</p>}
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldBitrate')} (kbps)</label>
                <input {...register('bitrate_kbps')} type="number" className={inputCls} />
                {errors.bitrate_kbps && <p className={errCls}>{errors.bitrate_kbps.message}</p>}
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldBitrateMode')}</label>
                <select {...register('bitrate_mode')} className={inputCls}>
                  <option value="cbr">CBR — {t('channels.config.bitrateModeCbr')}</option>
                  <option value="vbr">VBR — {t('channels.config.bitrateModeVbr')}</option>
                  <option value="crf">CRF — {t('channels.config.bitrateModeCrf')}</option>
                </select>
                <p className="text-xs text-[var(--text-muted)]">
                  {t('channels.config.bitrateModeHint')}
                </p>
              </div>

              {bitrateMode === 'vbr' && (
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.config.fieldBitrateMax')} (kbps)</label>
                  <input {...register('bitrate_max_kbps')} type="number" min={0} max={200_000} className={inputCls} />
                  <p className="text-xs text-[var(--text-muted)]">
                    {t('channels.config.bitrateMaxHint')}
                  </p>
                  {errors.bitrate_max_kbps && <p className={errCls}>{errors.bitrate_max_kbps.message}</p>}
                </div>
              )}

              {bitrateMode === 'crf' && (
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.config.fieldCrf')}</label>
                  <input {...register('crf')} type="number" min={0} max={51} className={inputCls} />
                  <p className="text-xs text-[var(--text-muted)]">
                    {t('channels.config.crfHint')}
                  </p>
                  {errors.crf && <p className={errCls}>{errors.crf.message}</p>}
                </div>
              )}

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldMaxB')}</label>
                <input {...register('max_b_frames')} type="number" min={0} max={16} className={inputCls} />
                <p className="text-xs text-[var(--text-muted)]">
                  {videoCodec === 'mpeg2video'
                    ? t('channels.config.maxBHintMpeg2')
                    : t('channels.config.maxBHintH264')}
                </p>
                {errors.max_b_frames && <p className={errCls}>{errors.max_b_frames.message}</p>}
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.config.fieldGopSize')}</label>
                <input {...register('gop_size')} type="number" min={0} max={600} className={inputCls} />
                <p className="text-xs text-[var(--text-muted)]">
                  {videoCodec === 'mpeg2video'
                    ? t('channels.config.gopSizeHintMpeg2')
                    : t('channels.config.gopSizeHintH264')}
                </p>
                {errors.gop_size && <p className={errCls}>{errors.gop_size.message}</p>}
              </div>

              {videoCodec === 'h264' && (
                <>
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldH264Profile')}</label>
                    <select {...register('h264_profile')} className={inputCls}>
                      <option value="">{t('channels.config.h264ProfileAuto')}</option>
                      <option value="baseline">Baseline</option>
                      <option value="main">Main</option>
                      <option value="high">High</option>
                      <option value="high10">High 10</option>
                      <option value="high422">High 4:2:2</option>
                      <option value="high444">High 4:4:4 Predictive</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {t('channels.config.h264ProfileHint')}
                    </p>
                  </div>
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldH264Level')}</label>
                    <select {...register('h264_level')} className={inputCls}>
                      <option value="0">{t('channels.config.h264LevelAuto')}</option>
                      <option value="30">3.0 (SD 30 fps)</option>
                      <option value="31">3.1 (SD 50/60, 720p 30)</option>
                      <option value="32">3.2 (720p 60)</option>
                      <option value="40">4.0 (1080p 30)</option>
                      <option value="41">4.1 (1080p 30 hi-bitrate)</option>
                      <option value="42">4.2 (1080p 60)</option>
                      <option value="50">5.0 (2K)</option>
                      <option value="51">5.1 (4K 30)</option>
                      <option value="52">5.2 (4K 60)</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {t('channels.config.h264LevelHint')}
                    </p>
                  </div>
                </>
              )}

              {videoCodec === 'mpeg2video' && (
                <>
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldMpeg2Profile')}</label>
                    <select {...register('mpeg2_profile')} className={inputCls}>
                      <option value="">{t('channels.config.mpeg2ProfileAuto')}</option>
                      <option value="simple">Simple (SP)</option>
                      <option value="main">Main (MP)</option>
                      <option value="high">High (HP)</option>
                      <option value="422">4:2:2 (studio)</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {t('channels.config.mpeg2ProfileHint')}
                    </p>
                  </div>
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldMpeg2Level')}</label>
                    <select {...register('mpeg2_level')} className={inputCls}>
                      <option value="0">{t('channels.config.mpeg2LevelAuto')}</option>
                      <option value="10">Low (352×288)</option>
                      <option value="8">Main (720×576, ≤15 Mbps)</option>
                      <option value="6">High-1440 (1440×1152, ≤60 Mbps)</option>
                      <option value="4">High (1920×1152, ≤80 Mbps)</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {t('channels.config.mpeg2LevelHint')}
                    </p>
                  </div>
                </>
              )}

              {videoCodec === 'h264' ? (
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldPreset')}</label>
                  <select {...register('preset')} className={inputCls}>
                    {['ultrafast','superfast','veryfast','faster','fast','medium','slow','veryslow'].map(v => <option key={v} value={v}>{v}</option>)}
                  </select>
                </div>
              ) : (
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldPreset')}</label>
                  <div className={`${inputCls} text-[var(--text-muted)] italic`}>
                    {t('channels.config.presetNotApplicableMpeg2')}
                  </div>
                </div>
              )}

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldPhotoDuration')}</label>
                <input {...register('default_photo_duration')} type="number" step="0.1" min={0.1} className={inputCls} />
                <p className="text-xs text-[var(--text-muted)]">{t('channels.photoDurationHint')}</p>
                {errors.default_photo_duration && <p className={errCls}>{errors.default_photo_duration.message}</p>}
              </div>

              <div className="col-span-2 border-t border-[var(--border-subtle)] pt-4 mt-1">
                <h3 className="text-sm font-semibold text-[var(--text-primary)] mb-3">
                  {t('channels.config.secContent')}
                </h3>
                <div className="grid grid-cols-2 gap-4">
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldContentMode')}</label>
                    <select {...register('content_mode')} className={inputCls}>
                      <option value="none">{t('channels.config.modeNone')}</option>
                      <option value="passthrough">{t('channels.config.modePassthrough')}</option>
                      <option value="cache">{t('channels.config.modeCache')}</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {contentMode === 'passthrough'
                        ? t('channels.config.modePassthroughHint')
                        : contentMode === 'cache'
                          ? t('channels.config.modeCacheHint')
                          : t('channels.config.modeNoneHint')}
                    </p>
                  </div>
                  {contentMode === 'passthrough' && (
                    <div className="col-span-2 flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.fieldContentSource')}</label>
                      <div className="flex gap-2">
                        <input {...register('content_source_path')} className={inputCls} placeholder="/mnt/share/sport" />
                        <button type="button" onClick={() => setPickerFor('content_source_path')}
                          className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                          <Folder size={14} /> {t('folderPicker.openButton')}
                        </button>
                      </div>
                    </div>
                  )}
                  {contentMode === 'cache' && (
                    <>
                      <div className="col-span-2 flex flex-col gap-1">
                        <label className={labelCls}>{t('channels.config.fieldContentSource')}</label>
                        <div className="flex gap-2">
                          <input {...register('content_share_path')} className={inputCls} placeholder="/mnt/share/sport" />
                          <button type="button" onClick={() => setPickerFor('content_share_path')}
                            className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                            <Folder size={14} /> {t('folderPicker.openButton')}
                          </button>
                        </div>
                      </div>
                      <div className="col-span-2 flex flex-col gap-1">
                        <label className={labelCls}>{t('channels.config.fieldContentCache')}</label>
                        <div className="flex gap-2">
                          <input {...register('content_cache_path')} className={inputCls} placeholder="/var/lib/liveqx/cache/sport" />
                          <button type="button" onClick={() => setPickerFor('content_cache_path')}
                            className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                            <Folder size={14} /> {t('folderPicker.openButton')}
                          </button>
                        </div>
                      </div>
                    </>
                  )}
                </div>
              </div>

              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldFallback')}</label>
                <div className="flex gap-2">
                  <input {...register('fallback_image_path')} className={inputCls} placeholder={t('channels.fallbackPlaceholder')} />
                  <button type="button" onClick={() => setFilePickerFor('fallback_image_path')}
                    className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                    <FileText size={14} /> {t('common.browse')}
                  </button>
                </div>
                <p className="text-xs text-[var(--text-muted)]">{t('channels.fallbackHint')}</p>
              </div>

              <div className="col-span-2 border-t border-[var(--border-subtle)] pt-4 mt-1">
                <h3 className="text-sm font-semibold text-[var(--text-primary)] mb-3">
                  {t('channels.config.secPlaybackLog')}
                </h3>
                <div className="grid grid-cols-2 gap-4">
                  <div className="flex flex-col gap-1">
                    <label className={labelCls}>{t('channels.config.fieldSinkType')}</label>
                    <select {...register('log_sink')} className={inputCls}>
                      <option value="none">{t('channels.config.sinkNone')}</option>
                      <option value="file">{t('channels.config.sinkFile')}</option>
                      <option value="db">{t('channels.config.sinkDb')}</option>
                    </select>
                    <p className="text-xs text-[var(--text-muted)]">
                      {logSink === 'file'
                        ? t('channels.config.sinkFileHint')
                        : logSink === 'db'
                          ? t('channels.config.sinkDbHint')
                          : t('channels.config.sinkNoneHint')}
                    </p>
                  </div>
                  {logSink === 'db' && (
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.fieldRetentionDays')}</label>
                      <input {...register('log_retention_days')} type="number" min={0} max={3650} className={inputCls} />
                      {errors.log_retention_days && <p className={errCls}>{errors.log_retention_days.message}</p>}
                    </div>
                  )}
                </div>
              </div>
            </div>

            <div className="flex gap-2 mt-5">
              <button type="button" onClick={() => navigate('/channels')}
                className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
                {t('common.cancel')}
              </button>
              <button type="button" onClick={() => setStep(2)}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md transition-colors">
                {t('common.next')} →
              </button>
            </div>
          </Block>
        )}

        {step === 2 && (
          <Block>
            <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('channels.createStep2')}</h2>
            <div className="grid grid-cols-2 gap-4">
              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldOutputType')}</label>
                <select {...register('output_type')} className={inputCls}>
                  {['srt','multicast','rtmp','hls'].map(v => <option key={v} value={v}>{v.toUpperCase()}</option>)}
                </select>
              </div>

              <div className="flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldOutputId')}</label>
                <input {...register('output_id')} className={inputCls} placeholder="main" />
                {errors.output_id && <p className={errCls}>{errors.output_id.message}</p>}
              </div>

              {(outputType === 'srt' || outputType === 'multicast') && <>
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputAddress')}</label>
                  <input {...register('output_address')} className={inputCls} placeholder="10.0.0.10" />
                </div>
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputPort')}</label>
                  <input {...register('output_port')} type="number" className={inputCls} />
                  {errors.output_port && <p className={errCls}>{errors.output_port.message}</p>}
                </div>
              </>}

              {outputType === 'rtmp' && (
                <div className="col-span-2 flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputUrl')}</label>
                  <input {...register('output_url')} className={inputCls} placeholder="rtmp://a.rtmp.youtube.com/live2/key" />
                </div>
              )}

              {outputType === 'hls' && (
                <div className="col-span-2 flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputDir')}</label>
                  <div className="flex gap-2">
                    <input {...register('output_dir')} className={inputCls} placeholder="/var/www/hls/sport" />
                    <button type="button" onClick={() => setPickerFor('output_dir')}
                      className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                      <Folder size={14} /> {t('folderPicker.openButton')}
                    </button>
                  </div>
                </div>
              )}

              {(outputType === 'srt' || outputType === 'multicast' || outputType === 'rtmp') && (
                <div className="col-span-2 flex flex-col gap-1">
                  <label className={labelCls}>{t('outputs.bindAddress')}</label>
                  <select {...register('output_bind_address')} className={inputCls}
                          disabled={nics.isLoading}>
                    <option value="">{t('outputs.bindAddressDefault')}</option>
                    {nicOptions.map(o => (
                      <option key={o.value} value={o.value}>{o.label}</option>
                    ))}
                  </select>
                  <p className="text-xs text-[var(--text-muted)]">{t('outputs.bindAddressHint')}</p>
                </div>
              )}

              {outputType === 'multicast' && (
                <div className="col-span-2 flex flex-col gap-1">
                  <label className={labelCls}>{t('outputs.ttl')}</label>
                  <input {...register('output_ttl')} type="number" min={1} max={255} className={inputCls} />
                  {errors.output_ttl && <p className={errCls}>{errors.output_ttl.message}</p>}
                  <p className="text-xs text-[var(--text-muted)]">{t('outputs.ttlHint')}</p>
                </div>
              )}

              {outputType === 'multicast' && (
                <div className="col-span-2 border-t border-[var(--border-subtle)] pt-4 mt-1">
                  <h3 className="text-sm font-semibold text-[var(--text-primary)] mb-1">
                    {t('channels.config.secMpegts')}
                  </h3>
                  <p className="text-xs text-[var(--text-muted)] mb-3">{t('channels.config.mpegtsHint')}</p>
                  <div className="grid grid-cols-2 gap-4">
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.serviceId')}</label>
                      <input {...register('mpegts_service_id')} type="number" min={1} max={65535} className={inputCls} />
                      {errors.mpegts_service_id && <p className={errCls}>{errors.mpegts_service_id.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.serviceIdHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.serviceName')}</label>
                      <input {...register('mpegts_service_name')} className={inputCls} placeholder="LiveQX Channel" />
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.serviceNameHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.serviceProvider')}</label>
                      <input {...register('mpegts_service_provider')} className={inputCls} placeholder="LiveQX" />
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.serviceProviderHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.transportStreamId')}</label>
                      <input {...register('mpegts_transport_stream_id')} type="number" min={1} max={65535} className={inputCls} />
                      {errors.mpegts_transport_stream_id && <p className={errCls}>{errors.mpegts_transport_stream_id.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.transportStreamIdHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.originalNetworkId')}</label>
                      <input {...register('mpegts_original_network_id')} type="number" min={1} max={65535} className={inputCls} />
                      {errors.mpegts_original_network_id && <p className={errCls}>{errors.mpegts_original_network_id.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.originalNetworkIdHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.muxRateKbps')}</label>
                      <input {...register('mpegts_mux_rate_kbps')} type="number" min={0} className={inputCls} />
                      {errors.mpegts_mux_rate_kbps && <p className={errCls}>{errors.mpegts_mux_rate_kbps.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.muxRateHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.sdtPeriodMs')}</label>
                      <input {...register('mpegts_sdt_period_ms')} type="number" min={0} className={inputCls} />
                      {errors.mpegts_sdt_period_ms && <p className={errCls}>{errors.mpegts_sdt_period_ms.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.sdtPeriodHint')}</p>
                    </div>
                    <div className="flex flex-col gap-1">
                      <label className={labelCls}>{t('channels.config.patPeriodMs')}</label>
                      <input {...register('mpegts_pat_period_ms')} type="number" min={0} className={inputCls} />
                      {errors.mpegts_pat_period_ms && <p className={errCls}>{errors.mpegts_pat_period_ms.message}</p>}
                      <p className="text-xs text-[var(--text-muted)]">{t('channels.config.patPeriodHint')}</p>
                    </div>
                  </div>
                </div>
              )}
            </div>

            <div className="flex gap-2 mt-5">
              <button type="button" onClick={() => setStep(1)}
                className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
                ← {t('common.back')}
              </button>
              <button type="submit" disabled={isPending}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {isPending ? t('common.loading') : t('channels.createSubmit')}
              </button>
            </div>
          </Block>
        )}
      </form>

      {pickerFor && (
        <FolderPickerModal
          initialPath={
            (pickerFor === 'content_source_path' ? contentSrc
              : pickerFor === 'content_share_path' ? contentShare
              : pickerFor === 'content_cache_path' ? contentCache
              : outputDir) || '/'
          }
          onSelect={(p) => { setValue(pickerFor, p, { shouldValidate: true }); setPickerFor(null); }}
          onCancel={() => setPickerFor(null)}
        />
      )}

      {filePickerFor && (
        <FilePickerModal
          initialPath={fallbackPath?.replace(/\/[^/]*$/, '') || '/'}
          acceptExtensions={FALLBACK_EXTS}
          onSelect={(p) => { setValue(filePickerFor, p, { shouldValidate: true }); setFilePickerFor(null); }}
          onCancel={() => setFilePickerFor(null)}
        />
      )}
    </div>
  );
}
