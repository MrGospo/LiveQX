import React from 'react';
import { useTranslation } from 'react-i18next';
import { Check, AlertCircle, Download, RotateCw, Upload, X } from 'lucide-react';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { useToast } from '@/hooks/useToast';
import {
  useTlsInfo,
  useRegenerateServerCert,
  useImportTls,
  downloadCaBundle,
} from '@/api/queries/tls';
import type { TlsCertInfo } from '@/api/types';
import { useEscClose } from '@/hooks/useEscClose';

function fmtUnix(unixSec: number | null | undefined): string {
  if (unixSec == null || unixSec === 0) return '—';
  return new Date(unixSec * 1000).toLocaleString();
}

function isCertInfo(v: unknown): v is TlsCertInfo {
  return !!v && typeof v === 'object' && 'subject' in (v as Record<string, unknown>);
}

function expiryTone(daysRemaining: number): 'ok' | 'warn' | 'crit' | 'expired' {
  if (daysRemaining < 0)  return 'expired';
  if (daysRemaining < 14) return 'crit';
  if (daysRemaining < 30) return 'warn';
  return 'ok';
}

function CertCard({ title, info }: { title: string; info: TlsCertInfo }) {
  const { t } = useTranslation();
  const tone = expiryTone(info.days_remaining);
  const toneColor =
    tone === 'expired' ? 'var(--danger)' :
    tone === 'crit'    ? 'var(--danger)' :
    tone === 'warn'    ? 'var(--warning)' : 'var(--success)';
  return (
    <Block>
      <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{title}</h2>
      <dl className="grid grid-cols-2 gap-4 text-sm">
        <div className="col-span-2">
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.subject')}</dt>
          <dd className="text-[var(--text-primary)] font-mono break-all">{info.subject}</dd>
        </div>
        <div className="col-span-2">
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.issuer')}</dt>
          <dd className="text-[var(--text-primary)] font-mono break-all">{info.issuer}</dd>
        </div>
        <div className="col-span-2">
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.fingerprint')}</dt>
          <dd className="text-[var(--text-primary)] font-mono break-all text-xs">{info.fingerprint_sha256}</dd>
        </div>
        <div>
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.signatureAlgorithm')}</dt>
          <dd className="text-[var(--text-primary)] font-mono">{info.signature_algorithm}</dd>
        </div>
        <div>
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.publicKeyAlgorithm')}</dt>
          <dd className="text-[var(--text-primary)] font-mono">{info.public_key_algorithm}</dd>
        </div>
        <div>
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.notBefore')}</dt>
          <dd className="text-[var(--text-primary)] font-mono">{fmtUnix(info.not_before_unix)}</dd>
        </div>
        <div>
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.notAfter')}</dt>
          <dd className="text-[var(--text-primary)] font-mono">{fmtUnix(info.not_after_unix)}</dd>
        </div>
        <div className="col-span-2">
          <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.daysRemaining')}</dt>
          <dd className="font-mono font-semibold" style={{ color: toneColor }}>
            {info.days_remaining < 0
              ? t('tls.expiredAgo', { n: -info.days_remaining })
              : t('tls.expiresIn', { n: info.days_remaining })}
          </dd>
        </div>
        {info.san_dns.length > 0 && (
          <div className="col-span-2">
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.sanDns')}</dt>
            <dd className="text-[var(--text-primary)] font-mono text-xs flex flex-wrap gap-1">
              {info.san_dns.map(d => (
                <span key={d} className="bg-surface2 px-2 py-0.5 rounded">{d}</span>
              ))}
            </dd>
          </div>
        )}
        {info.san_ip.length > 0 && (
          <div className="col-span-2">
            <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.sanIp')}</dt>
            <dd className="text-[var(--text-primary)] font-mono text-xs flex flex-wrap gap-1">
              {info.san_ip.map(d => (
                <span key={d} className="bg-surface2 px-2 py-0.5 rounded">{d}</span>
              ))}
            </dd>
          </div>
        )}
      </dl>
    </Block>
  );
}

interface ImportDialogProps {
  onCancel: () => void;
  onImported: () => void;
}

function ImportDialog({ onCancel, onImported }: ImportDialogProps) {
  const { t } = useTranslation();
  const toast = useToast();
  const { mutateAsync, isPending } = useImportTls();
  const [cert, setCert] = React.useState<File | null>(null);
  const [key, setKey] = React.useState<File | null>(null);
  const [ca, setCa] = React.useState<File | null>(null);

  const submit = async () => {
    if (!cert || !key) return;
    try {
      await mutateAsync({ cert, key, ca: ca ?? undefined });
      toast(t('tls.importSucceeded'), 'success');
      onImported();
    } catch (e) {
      toast((e as Error).message, 'danger');
    }
  };

  const fileRow = (
    label: string,
    file: File | null,
    setter: (f: File | null) => void,
    required: boolean,
    hint?: string,
  ) => (
    <div className="flex flex-col gap-1.5">
      <label className="text-sm font-medium text-[var(--text-primary)]">
        {label}{required && <span className="text-[var(--danger)] ml-1">*</span>}
      </label>
      <input
        type="file"
        accept=".pem,.crt,.cer,.key"
        onChange={e => setter(e.target.files?.[0] ?? null)}
        className="text-sm text-[var(--text-primary)] file:mr-3 file:px-3 file:py-1.5 file:rounded file:border-0 file:bg-surface2 file:text-[var(--text-primary)] hover:file:bg-[var(--border-subtle)] file:cursor-pointer cursor-pointer"
      />
      {file && (
        <div className="text-xs text-[var(--text-muted)] font-mono">{file.name} ({Math.round(file.size / 1024)} KB)</div>
      )}
      {hint && <div className="text-xs text-[var(--text-muted)]">{hint}</div>}
    </div>
  );

  useEscClose(onCancel, !isPending);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-lg shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('tls.importTitle')}</h2>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>
        <div className="p-5 flex flex-col gap-4">
          <p className="text-sm text-[var(--text-muted)]">{t('tls.importHint')}</p>
          {fileRow(t('tls.serverCert'),  cert, setCert, true, t('tls.serverCertHint'))}
          {fileRow(t('tls.serverKey'),   key,  setKey,  true, t('tls.serverKeyHint'))}
          {fileRow(t('tls.caBundle'),    ca,   setCa,   false, t('tls.importCaHint'))}
        </div>
        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={submit}
                  disabled={!cert || !key || isPending}
                  className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors flex items-center gap-2">
            <Upload size={14} />
            {isPending ? t('tls.importing') : t('tls.importSubmit')}
          </button>
        </div>
      </div>
    </div>
  );
}

export default function TlsPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { data, isLoading, error } = useTlsInfo();
  const { mutateAsync: regenerate, isPending: regenerating } = useRegenerateServerCert();
  const [downloading, setDownloading] = React.useState(false);
  const [importOpen, setImportOpen] = React.useState(false);
  const [extras, setExtras] = React.useState('');

  const onDownloadCa = async () => {
    setDownloading(true);
    try {
      const { blob, filename } = await downloadCaBundle();
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
      toast(t('tls.caDownloaded'), 'success');
    } catch (e) {
      toast((e as Error).message, 'danger');
    } finally {
      setDownloading(false);
    }
  };

  const onRegenerate = async () => {
    if (!confirm(t('tls.regenerateConfirm'))) return;
    const list = extras.split(/[\s,]+/).map(s => s.trim()).filter(Boolean);
    try {
      await regenerate(list);
      toast(t('tls.regenerateSucceeded'), 'success');
      setExtras('');
    } catch (e) {
      toast((e as Error).message, 'danger');
    }
  };

  const mode = data?.mode;
  const isAuto       = mode === 'auto';
  const isProvided   = mode === 'provided';
  const isProxy      = mode === 'behind_proxy';
  const isDisabled   = mode === 'disabled';

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />

      {/* ── Mode banner ─────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{t('tls.modeTitle')}</h2>
        {isLoading ? (
          <div className="h-12 bg-surface2 rounded-md animate-pulse" />
        ) : error ? (
          <div className="text-sm text-[var(--danger)]">{(error as Error).message}</div>
        ) : data ? (
          <>
            <div className="flex items-center gap-3">
              <div className={`px-2.5 py-0.5 rounded-md text-xs font-mono uppercase tracking-wider ${
                data.tls_enabled ? 'bg-[var(--success)]/15 text-[var(--success)]' : 'bg-[var(--warning)]/15 text-[var(--warning)]'
              }`}>
                {data.mode}
              </div>
              <div className="flex items-center gap-1.5 text-sm">
                {data.tls_enabled ? (
                  <>
                    <Check size={14} className="text-[var(--success)]" />
                    <span className="text-[var(--text-primary)]">{t('tls.httpsActive')}</span>
                  </>
                ) : (
                  <>
                    <AlertCircle size={14} className="text-[var(--warning)]" />
                    <span className="text-[var(--text-primary)]">{t('tls.httpInsecure')}</span>
                  </>
                )}
              </div>
            </div>
            <p className="text-sm text-[var(--text-muted)] mt-3">
              {isAuto && t('tls.modeAutoDesc')}
              {isProvided && t('tls.modeProvidedDesc')}
              {isProxy && t('tls.modeBehindProxyDesc')}
              {isDisabled && t('tls.modeDisabledDesc')}
            </p>
            <dl className="grid grid-cols-2 gap-4 mt-4 text-sm">
              <div>
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.bind')}</dt>
                <dd className="text-[var(--text-primary)] font-mono">{data.bind}</dd>
              </div>
              <div>
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.tlsDir')}</dt>
                <dd className="text-[var(--text-primary)] font-mono break-all">{data.tls_dir || '—'}</dd>
              </div>
              <div className="col-span-2">
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.certPath')}</dt>
                <dd className="text-[var(--text-primary)] font-mono break-all">{data.cert_path || '—'}</dd>
              </div>
              {data.san_extra.length > 0 && (
                <div className="col-span-2">
                  <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('tls.sanExtra')}</dt>
                  <dd className="text-[var(--text-primary)] font-mono text-xs flex flex-wrap gap-1">
                    {data.san_extra.map(s => (
                      <span key={s} className="bg-surface2 px-2 py-0.5 rounded">{s}</span>
                    ))}
                  </dd>
                </div>
              )}
            </dl>
          </>
        ) : null}
      </Block>

      {/* ── Server cert ─────────────────────────────────────────────── */}
      {data && isCertInfo(data.server) && (
        <CertCard title={t('tls.serverCertTitle')} info={data.server} />
      )}

      {/* ── CA cert + download ──────────────────────────────────────── */}
      {data && isCertInfo(data.ca) && (
        <Block>
          <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{t('tls.caTitle')}</h2>
          <p className="text-sm text-[var(--text-muted)] mb-4">{t('tls.caHint')}</p>
          <button onClick={onDownloadCa}
                  disabled={downloading}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-primary)] hover:bg-surface2 rounded-md flex items-center gap-2 disabled:opacity-50 transition-colors">
            <Download size={14} />
            {downloading ? t('tls.downloading') : t('tls.downloadCa')}
          </button>
          <div className="mt-5 pt-4 border-t border-[var(--border-subtle)]">
            <CertCard title={t('tls.caDetails')} info={data.ca} />
          </div>
        </Block>
      )}

      {/* ── Operations ──────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-3">{t('tls.operations')}</h2>

        <div className="flex flex-col gap-4">
          {/* Regenerate (auto only) */}
          <div className={`flex flex-col gap-3 ${!isAuto ? 'opacity-50' : ''}`}>
            <h3 className="text-sm font-semibold text-[var(--text-primary)]">{t('tls.regenerateTitle')}</h3>
            <p className="text-sm text-[var(--text-muted)]">
              {isAuto ? t('tls.regenerateHint') : t('tls.regenerateUnavailable', { mode })}
            </p>
            <div className="flex flex-col gap-1.5">
              <label className="text-xs text-[var(--text-muted)] uppercase tracking-wider">{t('tls.sanExtraInput')}</label>
              <input value={extras}
                     onChange={e => setExtras(e.target.value)}
                     disabled={!isAuto || regenerating}
                     placeholder={t('tls.sanExtraPlaceholder')}
                     className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] disabled:opacity-50" />
            </div>
            <button onClick={onRegenerate}
                    disabled={!isAuto || regenerating}
                    className="self-start px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors flex items-center gap-2">
              <RotateCw size={14} className={regenerating ? 'animate-spin' : ''} />
              {regenerating ? t('tls.regenerating') : t('tls.regenerateSubmit')}
            </button>
          </div>

          <div className="border-t border-[var(--border-subtle)]" />

          {/* Import */}
          <div className={`flex flex-col gap-3 ${isProxy || isDisabled ? 'opacity-50' : ''}`}>
            <h3 className="text-sm font-semibold text-[var(--text-primary)]">{t('tls.importHeader')}</h3>
            <p className="text-sm text-[var(--text-muted)]">
              {isProxy || isDisabled
                ? t('tls.importUnavailable', { mode })
                : t('tls.importDescription')}
            </p>
            <button onClick={() => setImportOpen(true)}
                    disabled={isProxy || isDisabled}
                    className="self-start px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-primary)] hover:bg-surface2 rounded-md flex items-center gap-2 disabled:opacity-50 transition-colors">
              <Upload size={14} />
              {t('tls.importOpen')}
            </button>
          </div>
        </div>
      </Block>

      {importOpen && (
        <ImportDialog onCancel={() => setImportOpen(false)}
                      onImported={() => setImportOpen(false)} />
      )}
    </div>
  );
}
