import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useChannels, useDeleteChannel } from '@/api/queries/channels';
import { HealthBadge } from '@/components/HealthBadge';
import { EmptyState } from '@/components/EmptyState';
import { Block } from '@/components/Block';
import { Tv2, Plus, ExternalLink, Trash2 } from 'lucide-react';
import { useAuthStore } from '@/stores/auth';
import { useToast } from '@/hooks/useToast';
import { DeleteChannelModal } from './DeleteChannelModal';

export default function ChannelsListPage() {
  const { t }     = useTranslation();
  const navigate  = useNavigate();
  const role      = useAuthStore(s => s.role);
  const toast     = useToast();

  const { data: channels = [], isLoading } = useChannels();
  const { mutate: deleteCh, isPending: deleting } = useDeleteChannel();
  const [pendingDelete, setPendingDelete] = React.useState<{ id: number; name: string } | null>(null);

  return (
    <div className="p-7 flex flex-col gap-5">
      <div className="flex items-center gap-4">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('channels.title')}</h1>
        <div className="flex-1" />
        {role === 'admin' && (
          <button onClick={() => navigate('/channels/new')}
            className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
            <Plus size={14} /> {t('channels.create')}
          </button>
        )}
      </div>

      <Block padding="p-0">
        {isLoading ? (
          <div className="p-5 flex flex-col gap-3">
            {Array(4).fill(0).map((_,i) => (
              <div key={i} className="h-12 bg-surface2 rounded-lg animate-pulse" />
            ))}
          </div>
        ) : channels.length === 0 ? (
          <EmptyState Icon={Tv2} title={t('dashboard.noChannels')} description={t('dashboard.noChannelsHint')} />
        ) : (
          <table className="w-full text-sm border-collapse">
            <thead>
              <tr className="border-b border-[var(--border-subtle)]">
                {['Channel', 'State', 'FPS', 'Resolution', 'Preset', 'Outputs', 'RAM', ''].map(h => (
                  <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {channels.map(ch => (
                <tr key={ch.id}
                  onClick={() => navigate(`/channels/${ch.id}`)}
                  className="border-b border-[var(--border-subtle)] cursor-pointer hover:bg-surface2 transition-colors">
                  <td className="px-4 py-2.5 font-semibold text-[var(--accent)]">ch{ch.id}‑{ch.name}</td>
                  <td className="px-4 py-2.5"><HealthBadge status={ch.state} small /></td>
                  <td className="px-4 py-2.5 tabular-nums text-[var(--text-muted)]">{(ch.fps_actual ?? 0) > 0 ? (ch.fps_actual ?? 0).toFixed(2) : '—'}</td>
                  <td className="px-4 py-2.5 text-[var(--text-muted)]">{ch.resolution}</td>
                  <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)]">{ch.preset.toUpperCase()}</td>
                  <td className="px-4 py-2.5 tabular-nums text-[var(--text-muted)]">
                    {ch.outputs?.filter(o => o.healthy).length ?? 0}/{ch.outputs?.length ?? 0}
                  </td>
                  <td className="px-4 py-2.5 tabular-nums text-[var(--text-muted)]">
                    {ch.frame_pool_bytes ? `${(ch.frame_pool_bytes / 1024 / 1024).toFixed(0)} MB` : '—'}
                  </td>
                  <td className="px-4 py-2.5">
                    <div className="flex items-center gap-1 justify-end">
                      <button
                        onClick={e => { e.stopPropagation(); navigate(`/channels/${ch.id}`); }}
                        aria-label={t('channels.open')}
                        className="p-1.5 text-[var(--text-muted)] hover:text-[var(--accent)] transition-colors">
                        <ExternalLink size={14} />
                      </button>
                      {role === 'admin' && (
                        <button
                          onClick={e => { e.stopPropagation(); setPendingDelete({ id: ch.id, name: ch.name }); }}
                          aria-label={t('channels.delete')}
                          className="p-1.5 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                          <Trash2 size={14} />
                        </button>
                      )}
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Block>

      {pendingDelete && (
        <DeleteChannelModal
          channelName={pendingDelete.name}
          pending={deleting}
          onCancel={() => setPendingDelete(null)}
          onConfirm={() => {
            const { id, name } = pendingDelete;
            deleteCh(id, {
              onSuccess: () => {
                toast(t('channels.deleted', { name }), 'warning');
                setPendingDelete(null);
              },
              onError: () => toast(t('channels.deleteError'), 'danger'),
            });
          }}
        />
      )}
    </div>
  );
}
