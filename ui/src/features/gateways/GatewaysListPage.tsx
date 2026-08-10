import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useGateways, usePlayGateway, useStopGateway, useDeleteGateway } from '@/api/queries/gateways';
import type { GatewayStatus } from '@/api/types';
import { HealthBadge } from '@/components/HealthBadge';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { fmtBytes } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import { Network, Plus, Play, Square, Trash2, Shield } from 'lucide-react';

type GwMode = NonNullable<GatewayStatus['mode']>;

const MODE_BADGE_CLS: Record<GwMode, string> = {
  passthrough: 'bg-[var(--text-muted)]/15 text-[var(--text-muted)]',
  demux:       'bg-blue-500/15 text-blue-400',
  remux:       'bg-purple-500/15 text-purple-400',
  transcode:   'bg-orange-500/15 text-orange-400',
};

export default function GatewaysListPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const toast = useToast();
  const { data: gws = [], isLoading } = useGateways();
  const { mutate: playGw  } = usePlayGateway();
  const { mutate: stopGw  } = useStopGateway();
  const { mutate: deleteGw} = useDeleteGateway();

  const goCreate = () => navigate('/gateways/new');

  return (
    <div className="p-7 flex flex-col gap-5">
      <div className="flex items-center gap-4">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('gateways.title')}</h1>
        <div className="flex-1" />
        <button onClick={goCreate}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
          <Plus size={14} /> {t('gateways.newGateway')}
        </button>
      </div>

      {isLoading ? (
        <div className="flex flex-col gap-3">{Array(2).fill(0).map((_,i) => <div key={i} className="h-20 bg-surface2 rounded-xl animate-pulse" />)}</div>
      ) : gws.length === 0 ? (
        <EmptyState Icon={Network} title={t('gateways.noGateways')} description={t('gateways.noGatewaysDesc')}
          action={<button onClick={goCreate} className="px-4 py-2 bg-[var(--accent)] text-white text-sm rounded-md">{t('gateways.createFirst')}</button>} />
      ) : (
        <div className="flex flex-col gap-3">
          {gws.map(gw => {
            const mode: GwMode = gw.mode ?? 'passthrough';
            const fecOn = gw.fec?.enabled === true;
            const inputs = gw.inputs && gw.inputs.length
              ? gw.inputs
              : (gw.input ? [gw.input] : []);
            return (
            <Block key={gw.id} padding="p-4">
              <div className="flex items-center gap-3">
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 mb-1 flex-wrap">
                    <span className="font-semibold text-[var(--text-primary)]">{gw.name}</span>
                    <HealthBadge status={gw.running ? 'running' : 'stopped'} small />
                    <span className={`text-[10px] uppercase tracking-wider px-1.5 py-0.5 rounded font-semibold ${MODE_BADGE_CLS[mode]}`}>
                      {t(`gateways.mode.${mode}`)}
                    </span>
                    {fecOn && (
                      <span className="text-[10px] uppercase tracking-wider px-1.5 py-0.5 rounded font-semibold bg-[var(--success)]/15 text-[var(--success)] flex items-center gap-1"
                            title={t('gateways.fec.tooltip', { mode: gw.fec?.mode ?? '1d', L: gw.fec?.L ?? 8, D: gw.fec?.D ?? 8 })}>
                        <Shield size={10} /> FEC {(gw.fec?.mode ?? '1d').toUpperCase()}
                      </span>
                    )}
                  </div>
                  <div className="font-mono text-xs text-[var(--text-muted)] flex gap-4 flex-wrap">
                    <span>{t('gateways.in')}: {inputs.length
                      ? inputs.map(i => `${i.address}:${i.port}`).join(', ')
                      : '—'}</span>
                    <span>→</span>
                    <span>{t('gateways.out')}: {gw.outputs.map(o => `${o.address}:${o.port}`).join(', ')}</span>
                    {gw.running && <span className="text-[var(--success)]">↑{fmtBytes(gw.bytes_out)}</span>}
                  </div>
                </div>
                <div className="flex gap-2">
                  {!gw.running ? (
                    <button onClick={() => playGw(gw.id, { onSuccess: () => toast(t('gateways.started', { name: gw.name }), 'success') })}
                      className="flex items-center gap-1 px-2.5 py-1.5 text-xs bg-[var(--success)]/15 text-[var(--success)] rounded hover:bg-[var(--success)]/25 transition-colors">
                      <Play size={12} /> {t('channels.start')}
                    </button>
                  ) : (
                    <button onClick={() => stopGw(gw.id, { onSuccess: () => toast(t('gateways.stopped', { name: gw.name }), 'info') })}
                      className="flex items-center gap-1 px-2.5 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                      <Square size={12} /> {t('channels.stop')}
                    </button>
                  )}
                  <button onClick={() => deleteGw(gw.id, { onSuccess: () => toast(t('gateways.deleted', { name: gw.name }), 'warning') })}
                    className="p-1.5 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                    <Trash2 size={14} />
                  </button>
                </div>
              </div>
            </Block>
            );
          })}
        </div>
      )}
    </div>
  );
}
