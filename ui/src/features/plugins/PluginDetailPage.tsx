import React from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, Trash2, Check, AlertTriangle, Package } from 'lucide-react';
import { usePlugin, useUninstallPlugin, useAcceptEula } from '@/api/queries/plugins';
import { Block } from '@/components/Block';
import { fmtDate } from '@/lib/format';
import { useToast } from '@/hooks/useToast';

export default function PluginDetailPage() {
  const { name } = useParams<{ name: string }>();
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();

  const { data: plugin, isLoading } = usePlugin(name ?? '');
  const { mutate: uninstall, isPending: uninstalling } = useUninstallPlugin();
  const { mutate: acceptEula, isPending: accepting } = useAcceptEula();

  const [showUninstall, setShowUninstall] = React.useState(false);
  const [forceUninstall, setForceUninstall] = React.useState(false);
  const [showEula, setShowEula] = React.useState(false);

  if (isLoading) return (
    <div className="p-7 flex flex-col gap-4">
      <div className="h-48 bg-surface2 rounded-xl animate-pulse" />
    </div>
  );
  if (!plugin) return <div className="p-7 text-[var(--text-muted)]">{t('plugins.notFound')}</div>;

  return (
    <div className="p-7 flex flex-col gap-5 max-w-2xl">
      <button onClick={() => navigate('/plugins')}
        className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
        <ChevronLeft size={15} /> {t('plugins.title')}
      </button>

      {/* Header */}
      <div className="flex items-start gap-4">
        <div className="w-12 h-12 rounded-xl bg-[var(--accent)]/15 flex items-center justify-center flex-shrink-0">
          <Package size={24} className="text-[var(--accent)]" />
        </div>
        <div className="flex-1">
          <div className="flex items-center gap-2 flex-wrap">
            <h1 className="text-2xl font-bold text-[var(--text-primary)]">{plugin.name}</h1>
            <span className="text-sm text-[var(--text-muted)]">v{plugin.version}</span>
            {plugin.eula_accepted && (
              <span className="text-xs bg-[var(--success)]/10 text-[var(--success)] px-2 py-0.5 rounded-full flex items-center gap-1">
                <Check size={11} /> EULA
              </span>
            )}
            {plugin.pending_unload && (
              <span className="text-xs bg-[var(--warning)]/10 text-[var(--warning)] px-2 py-0.5 rounded-full">
                {t('plugins.pendingUnload')}
              </span>
            )}
            {plugin.forced && (
              <span className="text-xs bg-[var(--danger)]/10 text-[var(--danger)] px-2 py-0.5 rounded-full flex items-center gap-1">
                <AlertTriangle size={11} /> {t('plugins.forceInstalled')}
              </span>
            )}
          </div>
          <p className="font-mono text-xs text-[var(--text-muted)] mt-1">sha256: {plugin.sha256}</p>
        </div>
      </div>

      {/* Info */}
      <Block>
        <h2 className="text-base font-semibold mb-4">{t('plugins.details')}</h2>
        <dl className="grid grid-cols-2 gap-4 text-sm">
          {[
            [t('plugins.version'),         plugin.version],
            [t('plugins.fInstalledAt'),    fmtDate(plugin.installed_at)],
            [t('plugins.fOutputDrivers'),  plugin.output_drivers?.join(', ') || '—'],
            [t('plugins.fInputDrivers'),   plugin.input_drivers?.join(', ') || '—'],
            [t('plugins.fForced'),         String(plugin.forced ?? false)],
            [t('plugins.fEulaAccepted'),   String(plugin.eula_accepted ?? false)],
          ].map(([k,v]) => (
            <div key={k}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
              <dd className="font-mono text-[var(--text-primary)]">{v}</dd>
            </div>
          ))}
        </dl>
      </Block>

      {/* EULA section */}
      {!plugin.eula_accepted && (
        <Block>
          <div className="flex items-start gap-3 mb-4">
            <AlertTriangle size={16} className="text-[var(--warning)] mt-0.5 flex-shrink-0" />
            <p className="text-sm text-[var(--text-primary)]">{t('plugins.eulaRequired')}</p>
          </div>
          <button onClick={() => setShowEula(true)}
            className="px-4 py-2 border border-[var(--accent)] text-[var(--accent)] text-sm rounded-md hover:bg-[var(--accent)]/10 transition-colors">
            {t('plugins.acceptEula')}
          </button>
        </Block>
      )}

      {/* Danger zone */}
      {!plugin.pending_unload && (
        <Block className="border-[var(--danger)]/30">
          <h2 className="text-base font-semibold text-[var(--danger)] mb-3">{t('plugins.dangerZone')}</h2>
          <p className="text-sm text-[var(--text-muted)] mb-4">{t('plugins.uninstallNote')}</p>
          <button onClick={() => setShowUninstall(true)}
            className="flex items-center gap-2 px-4 py-2 bg-[var(--danger)]/10 border border-[var(--danger)]/40 text-[var(--danger)] text-sm rounded-md hover:bg-[var(--danger)]/20 transition-colors">
            <Trash2 size={14} /> {t('plugins.uninstall')}
          </button>
        </Block>
      )}

      {/* Uninstall confirm modal */}
      {showUninstall && (
        <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-5">
          <div className="w-full max-w-md bg-surface border border-[var(--border-subtle)] rounded-xl p-7 flex flex-col gap-4">
            <h2 className="text-xl font-semibold text-[var(--text-primary)]">{t('plugins.uninstallTitle', { name: plugin.name })}</h2>
            <p className="text-sm text-[var(--text-muted)]">{t('plugins.uninstallWarning')}</p>
            <label className="flex items-start gap-2 cursor-pointer">
              <input type="checkbox" checked={forceUninstall} onChange={e => setForceUninstall(e.target.checked)} className="mt-0.5" />
              <span className="text-sm text-[var(--text-primary)]">{t('plugins.forceUninstall')}</span>
            </label>
            <div className="flex gap-2 justify-end">
              <button onClick={() => setShowUninstall(false)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">{t('common.cancel')}</button>
              <button disabled={uninstalling}
                onClick={() => uninstall(plugin.name, { onSuccess: () => { setShowUninstall(false); navigate('/plugins'); toast(t('plugins.uninstalled', { name: plugin.name }), 'warning'); }})}
                className="px-4 py-2 bg-[var(--danger)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {uninstalling ? t('common.loading') : t('plugins.uninstall')}
              </button>
            </div>
          </div>
        </div>
      )}

      {/* EULA modal */}
      {showEula && (
        <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-5">
          <div className="w-full max-w-lg bg-surface border border-[var(--border-subtle)] rounded-xl p-7 flex flex-col gap-4">
            <h2 className="text-xl font-semibold text-[var(--text-primary)]">EULA — {plugin.name}</h2>
            <div className="h-48 overflow-y-auto bg-canvas border border-[var(--border-subtle)] rounded-lg p-4 text-sm text-[var(--text-muted)] leading-relaxed">
              {t('plugins.eulaIntroLong', { name: plugin.name })}
            </div>
            <div className="flex gap-2 justify-end">
              <button onClick={() => setShowEula(false)} className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md">{t('common.cancel')}</button>
              <button disabled={accepting}
                onClick={() => acceptEula(plugin.name, { onSuccess: () => { setShowEula(false); toast(t('plugins.eulaAccepted'), 'success'); }})}
                className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {accepting ? t('common.loading') : t('plugins.acceptEula')}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
