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
  resolution: z.string().regex(/^\d+x\d+$/, 'e.g. 1920x1080').default('1920x1080'),
  fps: z.coerce.number().int().min(1).max(120).default(25),
  bitrate_kbps: z.coerce.number().int().min(100).max(100_000).default(4000),
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
      name: '', numa_node: 0, encoder_mode: 'auto', gpu_index: 0,
      resolution: '1920x1080', fps: 25, bitrate_kbps: 4000, preset: 'veryfast',
      default_photo_duration: 10,
      fallback_image_path: '',
      content_mode: 'none', content_source_path: '', content_share_path: '', content_cache_path: '',
      scan_interval_ms: 2000,
      output_type: 'srt', output_id: 'main', output_address: '0.0.0.0',
      output_port: 4000, output_url: '', output_dir: '',
      output_bind_address: '', output_ttl: 16,
      log_sink: 'none', log_retention_days: 90,
    },
  });

  const outputType   = watch('output_type');
  const outputDir    = watch('output_dir');
  const fallbackPath = watch('fallback_image_path');
  const outputBind   = watch('output_bind_address');
  const logSink      = watch('log_sink');
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
        const payload = {
          name: v.name,
          numa_node: v.numa_node,
          encoder_mode: v.encoder_mode,
          gpu_index: v.gpu_index,
          preset: v.preset,
          resolution: v.resolution,
          fps: v.fps,
          bitrate: v.bitrate_kbps * 1000,
          default_photo_duration: v.default_photo_duration,
          ...(v.fallback_image_path ? { fallback: { image_path: v.fallback_image_path } } : {}),
          ...(contentSource ? { content_source: contentSource } : {}),
          ...(v.log_sink !== 'none'
            ? { playback_log: v.log_sink === 'db'
                ? { sink: 'db', retention_days: v.log_retention_days }
                : { sink: 'file' } }
            : {}),
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
      if (errs.name || errs.numa_node || errs.bitrate_kbps) setStep(1);
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
                  {[25,30,50,60].map(v => <option key={v} value={v}>{v}</option>)}
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
                <label className={labelCls}>{t('channels.fieldPreset')}</label>
                <select {...register('preset')} className={inputCls}>
                  {['ultrafast','superfast','veryfast','faster','fast','medium','slow','veryslow'].map(v => <option key={v} value={v}>{v}</option>)}
                </select>
              </div>

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
