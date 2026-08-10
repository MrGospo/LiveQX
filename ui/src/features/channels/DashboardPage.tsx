import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { RefreshCw, Plus, FolderOpen, ImageOff, MonitorPlay } from 'lucide-react';
import { useChannels, usePlayChannel, useStopChannel, useNextClip } from '@/api/queries/channels';
import { useAuthStore } from '@/stores/auth';
import { HealthBadge } from '@/components/HealthBadge';
import { EmptyState } from '@/components/EmptyState';
import { Block } from '@/components/Block';
import { fmtDuration } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import { Tv2 } from 'lucide-react';

type FilterKey = 'all' | 'running' | 'stopped' | 'failed' | 'degraded';

const basename = (p: string) => p.replace(/\\/g, '/').split('/').filter(Boolean).pop() ?? p;

export default function DashboardPage() {
  const { t } = useTranslation();
  const navigate  = useNavigate();
  const toast     = useToast();
  const role      = useAuthStore(s => s.role);
  const [filter, setFilter] = React.useState<FilterKey>('all');

  const { data: channels = [], isLoading, refetch } = useChannels();
  const { mutate: play }  = usePlayChannel();
  const { mutate: stop }  = useStopChannel();
  const { mutate: next }  = useNextClip();

  const filtered = filter === 'all' ? channels : channels.filter(ch => {
    if (filter === 'degraded') return ch.state === 'degraded';
    if (filter === 'failed')   return ch.state === 'failed';
    return ch.state === filter;
  });

  const stats = {
    total:   channels.length,
    running: channels.filter(c => c.state === 'running').length,
    failed:  channels.filter(c => c.state === 'failed').length,
    outputs: channels.reduce((a, c) => a + (c.outputs?.filter(o => o.healthy).length ?? 0), 0),
  };

  const FILTERS: { key: FilterKey; label: string }[] = [
    { key: 'all',      label: t('dashboard.filter.all') },
    { key: 'running',  label: t('dashboard.filter.running') },
    { key: 'stopped',  label: t('dashboard.filter.stopped') },
    { key: 'failed',   label: t('dashboard.filter.failed') },
    { key: 'degraded', label: t('dashboard.filter.degraded') },
  ];

  return (
    <div className="p-7 flex flex-col gap-6">
      {/* Stats */}
      <div className="grid grid-cols-2 sm:grid-cols-4 gap-4">
        {[
          { label: t('dashboard.stats.total'),   value: stats.total,   color: undefined },
          { label: t('dashboard.stats.active'),  value: stats.running, color: 'text-[var(--success)]' },
          { label: t('dashboard.stats.failed'),  value: stats.failed,  color: stats.failed > 0 ? 'text-[var(--danger)]' : undefined },
          { label: t('dashboard.stats.outputs'), value: stats.outputs, color: 'text-[var(--accent)]' },
        ].map(({ label, value, color }) => (
          <Block key={label} padding="p-5" className="text-center">
            <div className={`text-3xl font-bold tabular-nums ${color ?? 'text-[var(--text-primary)]'}`}>{value}</div>
            <div className="text-sm text-[var(--text-muted)] mt-1">{label}</div>
          </Block>
        ))}
      </div>

      {/* Toolbar */}
      <div className="flex items-center gap-2 flex-wrap">
        {FILTERS.map(f => (
          <button key={f.key} onClick={() => setFilter(f.key)}
            className={`px-3.5 py-1 rounded-full text-sm font-medium transition-all border ${
              filter === f.key
                ? 'border-[var(--accent)] bg-[var(--accent)]/10 text-[var(--accent)]'
                : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
            }`}>
            {f.label}
          </button>
        ))}
        <div className="flex-1" />
        <button onClick={() => refetch()} aria-label={t('common.refresh')}
          className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
          <RefreshCw size={14} /> {t('common.refresh')}
        </button>
        {role === 'admin' && (
          <button onClick={() => navigate('/channels/new')}
            className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
            <Plus size={14} /> {t('channels.create')}
          </button>
        )}
      </div>

      {/* Grid */}
      {isLoading ? (
        <div className="grid grid-cols-[repeat(auto-fill,minmax(280px,360px))] gap-4">
          {Array(6).fill(0).map((_, i) => (
            <div key={i} className="bg-surface border border-[var(--border-subtle)] rounded-xl p-5 animate-pulse">
              <div className="h-4 bg-surface2 rounded w-3/5 mb-3" />
              <div className="h-3 bg-surface2 rounded w-2/5 mb-4" />
              <div className="h-3 bg-surface2 rounded w-4/5 mb-2" />
              <div className="h-3 bg-surface2 rounded w-3/5 mb-4" />
              <div className="flex gap-2">
                <div className="h-8 bg-surface2 rounded w-20" />
                <div className="h-8 bg-surface2 rounded w-16" />
              </div>
            </div>
          ))}
        </div>
      ) : filtered.length === 0 ? (
        <EmptyState Icon={Tv2} title={t('dashboard.noChannels')} description={t('dashboard.noChannelsHint')} />
      ) : (
        <div className="grid grid-cols-[repeat(auto-fill,minmax(280px,360px))] gap-4">
          {filtered.map(ch => {
            const isRunning = ch.state === 'running' || ch.state === 'degraded';
            const hasError  = ch.state === 'failed';
            const fps = ch.fps_actual ?? 0;
            return (
              <Block key={ch.id} padding="p-5"
                className={`flex flex-col gap-3 transition-all ${hasError ? 'border-[var(--danger)]/40' : ''}`}>
                {/* Header */}
                <div className="flex items-start justify-between gap-2">
                  <div>
                    <div className="font-semibold text-[var(--text-primary)] text-sm leading-snug">
                      ch{ch.id}‑{ch.name}
                    </div>
                    <div className="mt-1.5">
                      <HealthBadge status={ch.state} small />
                    </div>
                  </div>
                  <div className="text-right flex-shrink-0">
                    <div className="text-xs text-[var(--text-muted)] tabular-nums">{fps > 0 ? `${fps.toFixed(1)} fps` : '—'}</div>
                    <div className="text-xs text-[var(--text-muted)]">{ch.resolution}</div>
                  </div>
                </div>

                {/* Content source + fallback */}
                {(ch.content_source || ch.fallback?.image_path) && (
                  <div className="flex flex-col gap-0.5">
                    {ch.content_source && (() => {
                      const p = ch.content_source.mode === 'passthrough'
                        ? ch.content_source.source_path
                        : ch.content_source.share_path;
                      return (
                        <div className="flex items-center gap-1.5 min-w-0" title={p}>
                          <FolderOpen size={11} className="flex-shrink-0 text-[var(--text-muted)]" />
                          <span className="text-xs font-mono text-[var(--text-muted)] truncate">{p}</span>
                        </div>
                      );
                    })()}
                    {ch.fallback?.image_path && (
                      <div className="flex items-center gap-1.5 min-w-0" title={ch.fallback.image_path}>
                        <ImageOff size={11} className="flex-shrink-0 text-[var(--text-muted)]" />
                        <span className="text-xs font-mono text-[var(--text-muted)] truncate">
                          {basename(ch.fallback.image_path)}
                        </span>
                      </div>
                    )}
                  </div>
                )}

                {/* Now playing */}
                <div className="text-sm min-h-[2.5rem]">
                  {ch.current_clip_index >= 0 ? (
                    <div className="text-[var(--text-muted)]">
                      <span className="text-[var(--text-primary)] font-medium">▶ </span>
                      <span>clip #{ch.current_clip_index}</span>
                      <span className="ml-1.5 tabular-nums text-xs">
                        {fmtDuration(ch.current_clip_remaining_sec)} remaining
                      </span>
                    </div>
                  ) : (
                    <span className="text-[var(--text-muted)] text-xs">{t('dashboard.noClip')}</span>
                  )}
                </div>

                {/* Outputs — показываем только когда канал запущен; иначе transport пустой и точка вводит в заблуждение */}
                {isRunning && (
                  <div className="flex flex-wrap gap-1.5">
                    {ch.outputs?.map(out => {
                      const label = (out.transport ?? out.mode ?? '?').toUpperCase();
                      return (
                        <span key={out.id} className={`text-xs font-medium rounded-full px-2 py-0.5 flex items-center gap-1 ${
                          !out.healthy
                            ? 'bg-[var(--danger)]/15 text-[var(--danger)]'
                            : 'bg-[var(--border-subtle)]/40 text-[var(--text-muted)]'
                        }`}>
                          <span className={`w-1.5 h-1.5 rounded-full ${out.healthy ? 'bg-[var(--success)]' : 'bg-[var(--danger)]'}`} />
                          {label}
                          {out.transport === 'multicast' && out.address && (
                            <span className="font-mono opacity-70">{out.address}{out.port ? `:${out.port}` : ''}</span>
                          )}
                        </span>
                      );
                    })}
                    {!ch.outputs?.length && <span className="text-xs text-[var(--text-muted)]">no outputs</span>}
                  </div>
                )}

                {/* Actions */}
                <div className="flex items-center gap-2 pt-2 border-t border-[var(--border-subtle)]">
                  <button onClick={() => navigate(`/channels/${ch.id}`)}
                    className="flex items-center gap-1 px-2.5 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                    Open
                  </button>
                  {!isRunning ? (
                    <button onClick={() => play(ch.id, { onSuccess: () => toast(`ch${ch.id} started`, 'success') })}
                      className="flex items-center gap-1 px-2.5 py-1.5 text-xs border border-[var(--success)]/40 text-[var(--success)] rounded hover:bg-[var(--success)]/10 hover:border-[var(--success)] transition-colors">
                      Start
                    </button>
                  ) : (
                    <>
                      <button onClick={() => stop(ch.id, { onSuccess: () => toast(`ch${ch.id} stopped`, 'info') })}
                        className="flex items-center gap-1 px-2.5 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                        Stop
                      </button>
                      <button onClick={() => next(ch.id)}
                        className="flex items-center gap-1 px-2 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                        Next
                      </button>
                    </>
                  )}
                  <div className="flex-1" />
                  {isRunning && (
                    <button onClick={() => navigate(`/channels/${ch.id}/preview`)}
                      title="WebRTC превью"
                      className="flex items-center gap-1.5 px-2.5 py-1.5 text-xs border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--accent)] hover:border-[var(--accent)]/40 transition-colors">
                      <MonitorPlay size={13} /> WebRTC превью
                    </button>
                  )}
                </div>
              </Block>
            );
          })}
        </div>
      )}
    </div>
  );
}
