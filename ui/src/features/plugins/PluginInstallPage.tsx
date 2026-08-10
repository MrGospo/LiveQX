import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { z } from 'zod';
import { ChevronLeft, Upload, Check, AlertTriangle, Loader2 } from 'lucide-react';

const wizardSchema = z.object({
  plugin_name: z.string().min(1, 'Required').regex(/^[A-Za-z0-9_-]+$/, 'Alphanumeric, _ and - only'),
  force_replace: z.boolean().default(false),
  force_unsigned: z.boolean().default(false),
});
type WizardValues = z.infer<typeof wizardSchema>;

type Step = 1 | 2 | 3 | 4;

async function sha256Hex(f: File): Promise<string> {
  const buf = await f.arrayBuffer();
  const hash = await crypto.subtle.digest('SHA-256', buf);
  return Array.from(new Uint8Array(hash))
    .map(b => b.toString(16).padStart(2, '0'))
    .join('');
}

export default function PluginInstallPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const [step, setStep] = React.useState<Step>(1);
  const [file, setFile] = React.useState<File | null>(null);
  const [sha256, setSha256] = React.useState<string>('');
  const [hashing, setHashing] = React.useState(false);
  const [submitting, setSubmitting] = React.useState(false);
  const [result, setResult] = React.useState<{ success: boolean; name?: string; error?: string } | null>(null);

  const { register, handleSubmit, setValue, formState: { errors } } = useForm<WizardValues>({
    resolver: zodResolver(wizardSchema),
  });

  const handleFileDrop = async (f: File) => {
    setFile(f);
    setSha256('');
    setHashing(true);
    const name = f.name.replace(/[-_]plugin.*\.so$/i, '').replace(/\.so$/i, '');
    setValue('plugin_name', name);
    try {
      setSha256(await sha256Hex(f));
    } finally {
      setHashing(false);
    }
  };

  const doSubmit = handleSubmit(async (values) => {
    setSubmitting(true);
    try {
      const form = new FormData();
      if (file) form.append('plugin', file);
      const qs = new URLSearchParams();
      if (values.force_replace)  qs.set('force', 'true');
      if (values.force_unsigned) qs.set('i_understand', 'true');

      const res = await fetch(`/api/plugins/${values.plugin_name}/install?${qs}`, {
        method: 'POST',
        body: form,
      });
      const data = await res.json().catch(() => ({}));
      if (res.ok) {
        setResult({ success: true, name: data.name ?? values.plugin_name });
      } else {
        setResult({ success: false, error: data.detail ?? data.error ?? `HTTP ${res.status}` });
      }
      setStep(4);
    } catch (e) {
      setResult({ success: false, error: String(e) });
      setStep(4);
    } finally {
      setSubmitting(false);
    }
  });

  const stepLabels: Record<Step, string> = {
    1: t('plugins.step1'), 2: t('plugins.step2'), 3: t('plugins.step4'), 4: t('plugins.step5'),
  };

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors';

  return (
    <div className="p-7 max-w-xl flex flex-col gap-5">
      <button onClick={() => navigate('/plugins')}
        className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
        <ChevronLeft size={15} /> {t('plugins.title')}
      </button>

      <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('plugins.install')}</h1>

      <div className="flex items-center gap-1">
        {([1,2,3,4] as Step[]).map((s, i) => (
          <React.Fragment key={s}>
            <div className={`flex items-center gap-1 ${step >= s ? 'text-[var(--text-primary)]' : 'text-[var(--text-muted)]'}`}>
              <div className={`w-6 h-6 rounded-full flex items-center justify-center text-xs font-bold transition-colors ${step > s ? 'bg-[var(--success)] text-white' : step === s ? 'bg-[var(--accent)] text-white' : 'bg-surface2 text-[var(--text-muted)]'}`}>
                {step > s ? '✓' : s}
              </div>
              <span className="text-xs hidden sm:block">{stepLabels[s]}</span>
            </div>
            {i < 3 && <div className="flex-1 h-px bg-[var(--border-subtle)]" />}
          </React.Fragment>
        ))}
      </div>

      <form onSubmit={doSubmit} className="flex flex-col gap-5">
        {step === 1 && (
          <div className="flex flex-col gap-4">
            <div
              onDragOver={e => e.preventDefault()}
              onDrop={e => { e.preventDefault(); const f = e.dataTransfer.files[0]; if (f) handleFileDrop(f); }}
              onClick={() => document.getElementById('plugin-file-input')?.click()}
              className={`border-2 border-dashed rounded-xl p-10 text-center cursor-pointer transition-colors ${file ? 'border-[var(--success)] bg-[var(--success)]/5' : 'border-[var(--border-subtle)] hover:border-[var(--accent)]'}`}>
              <input id="plugin-file-input" type="file" accept=".so" className="hidden"
                onChange={e => { const f = e.target.files?.[0]; if (f) handleFileDrop(f); }} />
              <Upload size={28} className={`mx-auto mb-2 ${file ? 'text-[var(--success)]' : 'text-[var(--text-muted)]'}`} />
              <p className={`text-sm ${file ? 'text-[var(--success)]' : 'text-[var(--text-muted)]'}`}>
                {file ? `${file.name} (${(file.size / 1024 / 1024).toFixed(2)} MB)` : t('plugins.dropHint')}
              </p>
              {hashing && (
                <p className="flex items-center justify-center gap-1.5 text-xs text-[var(--text-muted)] mt-2">
                  <Loader2 size={12} className="animate-spin" /> SHA-256…
                </p>
              )}
              {!hashing && sha256 && (
                <p className="font-mono text-xs text-[var(--text-muted)] mt-2 break-all">sha256: {sha256}</p>
              )}
            </div>
            <div className="flex gap-2 justify-end">
              <button type="button" onClick={() => navigate('/plugins')} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">{t('common.cancel')}</button>
              <button type="button" disabled={!file || hashing} onClick={() => setStep(2)} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">{t('common.next')} →</button>
            </div>
          </div>
        )}

        {step === 2 && (
          <div className="flex flex-col gap-4">
            <div className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t('plugins.nameLabel')}</label>
              <input {...register('plugin_name')} className={inputCls} placeholder="ndi" />
              {errors.plugin_name && <p className="text-xs text-[var(--danger)]">{errors.plugin_name.message}</p>}
              <p className="text-xs text-[var(--text-muted)]">{t('plugins.nameHint')}</p>
            </div>
            <div className="flex gap-2 justify-end">
              <button type="button" onClick={() => setStep(1)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">← {t('common.back')}</button>
              <button type="button" onClick={() => setStep(3)} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md transition-colors">{t('common.next')} →</button>
            </div>
          </div>
        )}

        {step === 3 && (
          <div className="flex flex-col gap-4">
            <div className="flex flex-col gap-3">
              <label className="flex items-start gap-3 cursor-pointer">
                <input type="checkbox" {...register('force_replace')} className="mt-0.5" />
                <div>
                  <p className="text-sm font-medium text-[var(--text-primary)]">{t('plugins.forceReplace')}</p>
                  <p className="text-xs text-[var(--text-muted)]">{t('plugins.forceReplaceHint')}</p>
                </div>
              </label>
              <label className="flex items-start gap-3 cursor-pointer">
                <input type="checkbox" {...register('force_unsigned')} className="mt-0.5" />
                <div>
                  <p className="text-sm font-medium text-[var(--text-primary)]">{t('plugins.forceUnsigned')}</p>
                  <p className="text-xs text-[var(--text-muted)]">{t('plugins.forceUnsignedHint')}</p>
                </div>
              </label>
            </div>
            <div className="flex gap-2 justify-end">
              <button type="button" onClick={() => setStep(2)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">← {t('common.back')}</button>
              <button type="submit" disabled={submitting}
                className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {submitting ? t('common.loading') : t('plugins.submit')}
              </button>
            </div>
          </div>
        )}

        {step === 4 && result && (
          <div className="flex flex-col gap-4">
            <div className={`flex items-start gap-3 p-4 rounded-lg border text-sm ${result.success ? 'bg-[var(--success)]/10 border-[var(--success)]/30' : 'bg-[var(--danger)]/10 border-[var(--danger)]/30'}`}>
              {result.success
                ? <><Check size={16} className="text-[var(--success)] mt-0.5" /><span>{t('plugins.installSuccess', { name: result.name })}</span></>
                : <><AlertTriangle size={16} className="text-[var(--danger)] mt-0.5" /><span>{result.error}</span></>}
            </div>
            <div className="flex gap-2 justify-end">
              {result.success
                ? <button type="button" onClick={() => navigate(`/plugins/${result.name}`)} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md">{t('plugins.viewPlugin')}</button>
                : <><button type="button" onClick={() => setStep(3)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">← {t('common.back')}</button>
                   <button type="button" onClick={() => navigate('/plugins')} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md">{t('common.cancel')}</button></>}
            </div>
          </div>
        )}
      </form>
    </div>
  );
}
