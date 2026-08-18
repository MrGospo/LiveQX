import React from 'react';
import { useTranslation } from 'react-i18next';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { z } from 'zod';
import { X, Folder } from 'lucide-react';
import type { OutputStatus } from '@/api/types';
import { FolderPickerModal } from '@/components/FolderPickerModal';
import { useNetworkInterfaces } from '@/api/queries/system';
import { useEscClose } from '@/hooks/useEscClose';

// Schema reused from CreateChannelPage. Fields are deliberately broad — per-type
// requirement enforcement happens via toBackend(): empty strings are dropped.
export const outputFormSchema = z.object({
  type: z.enum(['srt', 'multicast', 'rtmp', 'hls']),
  id: z.string().min(1, 'Required').max(64).regex(/^[a-zA-Z0-9_-]+$/, 'Letters, digits, _ and - only'),
  enabled: z.boolean().default(true),
  address: z.string().default(''),
  port: z.coerce.number().int().min(1).max(65535).default(4000),
  url: z.string().default(''),
  dir: z.string().default(''),
  queue_mb: z.coerce.number().int().min(1).max(256).default(4),
  // Source-NIC bind. Empty = OS default route. Surfaced only for transports
  // that honour it (srt, multicast, rtmp); ignored on HLS.
  bind_address: z.string().default(''),
  // Multicast hop limit (IP_MULTICAST_TTL). Backend range 1..255, default 16.
  ttl: z.coerce.number().int().min(1).max(255).default(16),
});

export type OutputFormValues = z.infer<typeof outputFormSchema>;

export type OutputFormMode = 'create' | 'edit';

interface Props {
  mode: OutputFormMode;
  initialValues?: OutputStatus;
  onSubmit: (payload: Record<string, unknown>) => void | Promise<void>;
  onCancel: () => void;
  submitting?: boolean;
}

// Drop empty optional fields, return per-type backend payload.
function toBackend(v: OutputFormValues): Record<string, unknown> {
  const base: Record<string, unknown> = {
    id: v.id, type: v.type, enabled: v.enabled,
    queue_bytes_limit: v.queue_mb * 1024 * 1024,
  };
  // bind_address goes through only for transports that honour it AND only
  // when non-empty (empty = OS default route; sending "" would just clutter
  // persisted cfg and break a future round-trip equality check).
  const withBind = (extra: Record<string, unknown>) =>
    v.bind_address ? { ...base, ...extra, bind_address: v.bind_address }
                   : { ...base, ...extra };
  switch (v.type) {
    case 'srt':
      return withBind({ address: v.address, port: v.port });
    case 'multicast':
      return withBind({ address: v.address, port: v.port, ttl: v.ttl });
    case 'rtmp':
      return withBind({ url: v.url });
    case 'hls':
      return { ...base, dir: v.dir };
  }
}

function fromStatus(out?: OutputStatus): OutputFormValues {
  if (!out) {
    return { type: 'srt', id: 'main', enabled: true, address: '0.0.0.0', port: 4000, url: '', dir: '', queue_mb: 4, bind_address: '', ttl: 16 };
  }
  // Resolve type: running channels expose `transport` from IOutput::statusJson();
  // stopped channels stage raw cfg whose discriminator is `type`. Read both so
  // Edit modal stays correct regardless of channel state.
  const raw = (out as { transport?: string; type?: string; bind_address?: string; interface?: string; ttl?: number });
  const t = (raw.transport ?? raw.type ?? 'srt') as OutputFormValues['type'];
  return {
    type: ['srt', 'multicast', 'rtmp', 'hls'].includes(t) ? t : 'srt',
    id: out.id,
    enabled: true,
    address: out.address ?? '',
    port: out.port ?? 4000,
    url: out.host ?? '',
    dir: out.output_dir ?? '',
    queue_mb: out.queue_bytes_limit ? Math.round(out.queue_bytes_limit / 1024 / 1024) : 4,
    // Prefer canonical `bind_address`; fall back to legacy `interface` so an
    // older multicast cfg still pre-fills the dropdown in Edit mode.
    bind_address: raw.bind_address ?? raw.interface ?? '',
    ttl: raw.ttl ?? 16,
  };
}

export function OutputFormModal({ mode, initialValues, onSubmit, onCancel, submitting }: Props) {
  const { t } = useTranslation();
  const { register, handleSubmit, watch, setValue, formState: { errors } } = useForm<OutputFormValues>({
    resolver: zodResolver(outputFormSchema),
    defaultValues: fromStatus(initialValues),
  });
  const outputType  = watch('type');
  const dir         = watch('dir');
  const bindAddress = watch('bind_address');
  const [pickerOpen, setPickerOpen] = React.useState(false);

  // NIC dropdown: list IPv4 addresses across all up, non-loopback interfaces.
  // Loopback is filtered out — pinning a push output to 127.x is almost
  // always a config mistake (the data wouldn't leave the box). If the current
  // bind_address points at a NIC that's no longer present (e.g. cfg was set
  // earlier and the NIC went away), we still render it as an extra option so
  // Edit mode doesn't silently flip it to "default".
  const nics = useNetworkInterfaces();
  const nicOptions = React.useMemo(() => {
    const live = (nics.data ?? [])
      .filter(n => n.up && !n.loopback)
      .flatMap(n => n.addresses.map(a => ({ value: a, label: `${n.name} — ${a}` })));
    if (bindAddress && !live.some(o => o.value === bindAddress)) {
      live.push({ value: bindAddress, label: `${bindAddress} (offline)` });
    }
    return live;
  }, [nics.data, bindAddress]);

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors w-full';
  const labelCls = 'text-sm font-medium text-[var(--text-primary)]';
  const errCls = 'text-xs text-[var(--danger)] mt-1';

  const submit = handleSubmit(async (v) => {
    await onSubmit(toBackend(v));
  });

  useEscClose(onCancel);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-lg p-6 shadow-2xl">
        <div className="flex items-center justify-between mb-5">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">
            {mode === 'create' ? t('outputs.addTitle') : t('outputs.editTitle')}
          </h2>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>

        <form onSubmit={submit} className="flex flex-col gap-4">
          <div className="grid grid-cols-2 gap-4">
            <div className="flex flex-col gap-1">
              <label className={labelCls}>{t('channels.fieldOutputType')}</label>
              <select {...register('type')} disabled={mode === 'edit'} className={inputCls}>
                {(['srt', 'multicast', 'rtmp', 'hls'] as const).map(v => (
                  <option key={v} value={v}>{v.toUpperCase()}</option>
                ))}
              </select>
              {mode === 'edit' && <p className="text-xs text-[var(--text-muted)]">{t('outputs.typeImmutable')}</p>}
            </div>

            <div className="flex flex-col gap-1">
              <label className={labelCls}>{t('channels.fieldOutputId')}</label>
              <input {...register('id')} disabled={mode === 'edit'} className={inputCls} placeholder="main" />
              {errors.id && <p className={errCls}>{errors.id.message}</p>}
            </div>

            {(outputType === 'srt' || outputType === 'multicast') && (
              <>
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputAddress')}</label>
                  <input {...register('address')} className={inputCls} placeholder="10.0.0.10" />
                </div>
                <div className="flex flex-col gap-1">
                  <label className={labelCls}>{t('channels.fieldOutputPort')}</label>
                  <input {...register('port')} type="number" className={inputCls} />
                  {errors.port && <p className={errCls}>{errors.port.message}</p>}
                </div>
              </>
            )}

            {outputType === 'rtmp' && (
              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldOutputUrl')}</label>
                <input {...register('url')} className={inputCls} placeholder="rtmp://a.rtmp.youtube.com/live2/key" />
              </div>
            )}

            {outputType === 'hls' && (
              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('channels.fieldOutputDir')}</label>
                <div className="flex gap-2">
                  <input {...register('dir')} className={inputCls} placeholder="/var/www/hls/sport" />
                  <button type="button" onClick={() => setPickerOpen(true)}
                    className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                    <Folder size={14} /> {t('folderPicker.openButton')}
                  </button>
                </div>
              </div>
            )}

            {(outputType === 'srt' || outputType === 'multicast' || outputType === 'rtmp') && (
              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('outputs.bindAddress')}</label>
                <select {...register('bind_address')} className={inputCls}
                        disabled={nics.isLoading}>
                  <option value="">{t('outputs.bindAddressDefault')}</option>
                  {nicOptions.map(o => (
                    <option key={o.value} value={o.value}>{o.label}</option>
                  ))}
                </select>
                <p className="text-xs text-[var(--text-muted)]">
                  {t('outputs.bindAddressHint')}
                </p>
              </div>
            )}

            {outputType === 'multicast' && (
              <div className="col-span-2 flex flex-col gap-1">
                <label className={labelCls}>{t('outputs.ttl')}</label>
                <input {...register('ttl')} type="number" min={1} max={255} className={inputCls} />
                {errors.ttl && <p className={errCls}>{errors.ttl.message}</p>}
                <p className="text-xs text-[var(--text-muted)]">
                  {t('outputs.ttlHint')}
                </p>
              </div>
            )}

            <div className="flex flex-col gap-1">
              <label className={labelCls}>{t('outputs.queueLimitMb')}</label>
              <input {...register('queue_mb')} type="number" min={1} max={256} className={inputCls} />
              {errors.queue_mb && <p className={errCls}>{errors.queue_mb.message}</p>}
            </div>

            <div className="flex items-end pb-2">
              <div className="flex items-center gap-2">
                <input type="checkbox" id="enabled" {...register('enabled')} />
                <label htmlFor="enabled" className={labelCls}>{t('outputs.enabled')}</label>
              </div>
            </div>
          </div>

          <div className="flex gap-2 justify-end mt-2">
            <button type="button" onClick={onCancel}
              className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
              {t('common.cancel')}
            </button>
            <button type="submit" disabled={submitting}
              className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
              {submitting ? t('common.loading') : mode === 'create' ? t('common.add') : t('common.save')}
            </button>
          </div>
        </form>
      </div>

      {pickerOpen && (
        <FolderPickerModal
          initialPath={dir || '/'}
          onSelect={(p) => { setValue('dir', p, { shouldValidate: true }); setPickerOpen(false); }}
          onCancel={() => setPickerOpen(false)}
        />
      )}
    </div>
  );
}
