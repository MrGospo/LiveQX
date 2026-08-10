import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { usePlugins, useUninstallPlugin, useAcceptEula } from '@/api/queries/plugins';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { useToast } from '@/hooks/useToast';
import { Package, Upload, Trash2 } from 'lucide-react';

export default function PluginsListPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const navigate = useNavigate();
  const { data: plugins = [], isLoading } = usePlugins();
  const { mutate: uninstall } = useUninstallPlugin();
  const { mutate: acceptEula } = useAcceptEula();

  const goInstall = () => navigate('/plugins/install');

  return (
    <div className="p-7 flex flex-col gap-5">
      <div className="flex items-center gap-4">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('plugins.title')}</h1>
        <div className="flex-1" />
        <button onClick={goInstall}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
          <Upload size={14} /> {t('plugins.install')}
        </button>
      </div>

      {isLoading ? (
        <div className="flex flex-col gap-3">{Array(2).fill(0).map((_,i) => <div key={i} className="h-24 bg-surface2 rounded-xl animate-pulse" />)}</div>
      ) : plugins.length === 0 ? (
        <EmptyState Icon={Package} title={t('plugins.noPlugins')} description={t('plugins.noPluginsDesc')}
          action={<button onClick={goInstall} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md">{t('plugins.installFirst')}</button>} />
      ) : (
        <div className="flex flex-col gap-3">
          {plugins.map(p => (
            <Block key={p.name} padding="p-4">
              <div className="flex items-start gap-3">
                <div className="w-11 h-11 rounded-lg bg-[var(--accent)]/15 flex items-center justify-center flex-shrink-0">
                  <Package size={22} className="text-[var(--accent)]" />
                </div>
                <div className="flex-1">
                  <div className="flex items-center gap-2 mb-1 flex-wrap">
                    <span className="font-bold text-[var(--text-primary)]">{p.name}</span>
                    <span className="text-xs text-[var(--text-muted)]">v{p.version}</span>
                    {p.eula_accepted && <span className="text-xs bg-[var(--success)]/10 text-[var(--success)] px-2 py-0.5 rounded-full">{t('plugins.eulaAcceptedBadge')}</span>}
                    {p.pending_unload && <span className="text-xs bg-[var(--warning)]/10 text-[var(--warning)] px-2 py-0.5 rounded-full">{t('plugins.pendingUnload')}</span>}
                  </div>
                  <div className="font-mono text-xs text-[var(--text-muted)] mb-2">sha256: {p.sha256.slice(0,20)}…</div>
                  <div className="flex gap-1.5 flex-wrap">
                    {p.output_drivers?.map(d => <span key={d} className="text-xs bg-[var(--accent)]/10 text-[var(--accent)] px-1.5 py-0.5 rounded">out:{d}</span>)}
                    {p.input_drivers?.map(d => <span key={d} className="text-xs bg-[var(--info)]/10 text-[var(--info)] px-1.5 py-0.5 rounded">in:{d}</span>)}
                  </div>
                </div>
                <div className="flex gap-2 flex-shrink-0">
                  {!p.eula_accepted && (
                    <button onClick={() => acceptEula(p.name, { onSuccess: () => toast(t('plugins.eulaAcceptedToast'), 'success') })}
                      className="px-2.5 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                      {t('plugins.acceptEula')}
                    </button>
                  )}
                  {!p.pending_unload && (
                    <button onClick={() => uninstall(p.name, { onSuccess: () => toast(t('plugins.markedForUnload', { name: p.name }), 'warning') })}
                      className="p-1.5 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                      <Trash2 size={14} />
                    </button>
                  )}
                </div>
              </div>
            </Block>
          ))}
        </div>
      )}
    </div>
  );
}
