import React from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ChevronLeft, ChevronRight, Play, Square, SkipForward, MoreVertical, Pencil, Trash2, RotateCw, ChevronUp, ChevronDown, Radio, Camera } from 'lucide-react';
import {
  useChannel, usePlayChannel, useStopChannel, useNextClip, useDeleteChannel,
  useAddOutput, usePatchOutput, useDeleteOutput, useRestartOutput,
} from '@/api/queries/channels';
import { HealthBadge } from '@/components/HealthBadge';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { fmtDuration } from '@/lib/format';
import { useToast } from '@/hooks/useToast';
import { useEscClose } from '@/hooks/useEscClose';
import { useAuthStore } from '@/stores/auth';
import { OutputFormModal } from './OutputFormModal';
import { DeleteChannelModal } from './DeleteChannelModal';
import { RowActionsMenu } from '@/components/RowActionsMenu';
import { ScheduleEntryModal } from './ScheduleEntryModal';
import { FilePickerModal } from '@/components/FilePickerModal';
import { FolderPickerModal } from '@/components/FolderPickerModal';
import { FileText, Folder, FolderPlus, FilePlus } from 'lucide-react';
import { api } from '@/api/client';
import { useQueryClient } from '@tanstack/react-query';
import { useUsers } from '@/api/queries/auth';
import { useEventStream } from '@/hooks/useEventStream';
import type { BrowseResponse } from '@/api/queries/system';

const FALLBACK_EXTS = ['.png', '.jpg', '.jpeg', '.webp', '.bmp'];
const MEDIA_EXTS = [
  '.png', '.jpg', '.jpeg', '.webp', '.bmp',
  '.mp4', '.mov', '.mkv', '.avi', '.ts', '.m2ts', '.webm',
];

// ─── Shared formatters & primitives ──────────────────────────────────────────

// Formats bytes as KB/MB/GB depending on magnitude.
function fmtBytes(b: number): string {
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  if (b < 1024 * 1024 * 1024) return `${(b / 1024 / 1024).toFixed(1)} MB`;
  return `${(b / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

// Backend last_share_ok_ns is a monotonic clock value, not wall-clock —
// labelled honestly so users don't try to interpret it as a timestamp.
function fmtScanAgo(lastNs: number | undefined): string {
  if (lastNs == null || lastNs === 0) return '—';
  return `monotonic ns=${lastNs}`;
}

// Compact label/value pair for dense counter grids in Outputs/Log/Watcher tabs.
function Counter({ label, value, tone }: { label: string; value: string; tone?: 'warn' | 'danger' }) {
  const valueClr = tone === 'warn'   ? 'text-[var(--warning)]'
                 : tone === 'danger' ? 'text-[var(--danger)]'
                 :                     'text-[var(--text-primary)]';
  return (
    <div>
      <div className="text-xs text-[var(--text-muted)] uppercase tracking-wider">{label}</div>
      <div className={`font-mono tabular-nums ${valueClr}`}>{value}</div>
    </div>
  );
}

// ─── Tab nav ─────────────────────────────────────────────────────────────────
const TAB_IDS = [
  'overview', 'playlist', 'schedule', 'outputs', 'log', 'config', 'watcher', 'permissions',
] as const;

export default function ChannelDetailPage() {
  const { id }   = useParams<{ id: string }>();
  const navigate = useNavigate();
  const toast    = useToast();
  const { t }    = useTranslation();
  const chId     = Number(id);
  const [activeTab, setActiveTab] = React.useState('overview');

  const role     = useAuthStore(s => s.role);
  const { data: ch, isLoading } = useChannel(chId);
  const { mutate: play, isPending: playing } = usePlayChannel();
  const { mutate: stop, isPending: stopping } = useStopChannel();
  const { mutate: next, isPending: nexting }  = useNextClip();
  const { mutate: deleteCh, isPending: deletingCh } = useDeleteChannel();
  const [confirmDelete, setConfirmDelete] = React.useState(false);

  if (isLoading) return (
    <div className="p-7 flex flex-col gap-4">
      {Array(3).fill(0).map((_,i) => <div key={i} className="h-16 bg-surface2 rounded-xl animate-pulse" />)}
    </div>
  );

  if (!ch) return (
    <div className="p-7">
      <EmptyState Icon={ChevronLeft} title="Channel not found" action={
        <button onClick={() => navigate('/dashboard')} className="px-4 py-2 bg-[var(--accent)] text-white rounded-md text-sm">Back</button>
      } />
    </div>
  );

  const isRunning = ch.state === 'running' || ch.state === 'degraded';
  const isStopped = ch.state === 'stopped' || ch.state === 'failed';

  return (
    <div className="flex flex-col h-full">
      {/* Sticky channel header */}
      <div className="sticky top-0 z-10 bg-surface border-b border-[var(--border-subtle)] px-7 pt-4">
        {/* Breadcrumb + title (one row) */}
        <div className="flex items-center gap-3 flex-wrap mb-3">
          <button onClick={() => navigate('/dashboard')}
            className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
            <ChevronLeft size={15} /> {t('nav.dashboard')}
          </button>
          <span className="text-[var(--text-muted)]">/</span>
          <h1 className="text-base font-semibold text-[var(--text-primary)]">ch{ch.id}-{ch.name}</h1>
          <HealthBadge status={ch.state} />
          <div className="flex-1" />
          {/* Actions */}
          <div className="flex items-center gap-2">
            {isStopped && (
              <button disabled={playing} onClick={() => play(ch.id, { onSuccess: () => toast('Channel started', 'success') })}
                className="flex items-center gap-1.5 px-3 py-1.5 text-sm bg-[var(--success)] text-[#111] rounded-md font-medium disabled:opacity-50 transition-colors">
                <Play size={14} /> {t('channels.start')}
              </button>
            )}
            {isRunning && (
              <>
                <button disabled={stopping} onClick={() => stop(ch.id, { onSuccess: () => toast('Channel stopped', 'info') })}
                  className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
                  <Square size={14} /> {t('channels.stop')}
                </button>
                <button disabled={nexting} onClick={() => next(ch.id)}
                  className="flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
                  <SkipForward size={14} /> {t('channels.next')}
                </button>
              </>
            )}
            <RowActionsMenu
              ariaLabel={t('channels.title')}
              items={[
                {
                  key:   'preview',
                  label: t('channels.webrtcPreview'),
                  icon:  <Camera size={14} />,
                  onClick: () => navigate(`/channels/${ch.id}/preview`),
                },
                {
                  key:   'editConfig',
                  label: t('channels.editConfig'),
                  icon:  <Pencil size={14} />,
                  onClick: () => setActiveTab('config'),
                },
                ...(role === 'admin' ? [{
                  key:   'delete',
                  label: t('channels.delete'),
                  icon:  <Trash2 size={14} />,
                  danger: true,
                  onClick: () => setConfirmDelete(true),
                }] : []),
              ]}
            />
          </div>
        </div>

        {/* Meta row */}
        <div className="flex gap-4 text-xs text-[var(--text-muted)] mb-0 pb-0">
          <span>{ch.resolution}@{ch.fps_target}fps</span>
          <span>encoder: {String(ch.encoder_mode ?? 'x264').toUpperCase()}</span>
          <span>NUMA: {ch.numa_node ?? 0}</span>
          <span>{(ch.bitrate / 1_000_000).toFixed(1)} Mbps</span>
        </div>

        {/* Tabs */}
        <div className="flex gap-0 mt-2 -mb-px border-t border-[var(--border-subtle)]">
          {TAB_IDS.map(tab => (
            <button key={tab} onClick={() => setActiveTab(tab)}
              className={`px-4 py-2.5 text-sm font-medium border-b-2 transition-colors ${
                activeTab === tab
                  ? 'text-[var(--accent)] border-[var(--accent)]'
                  : 'text-[var(--text-muted)] border-transparent hover:text-[var(--text-primary)]'
              }`}>
              {t(`channels.tabs.${tab}`)}
            </button>
          ))}
        </div>
      </div>

      {/* Tab content */}
      <div className="flex-1 overflow-y-auto p-7">
        {activeTab === 'overview' && <OverviewTab ch={ch} />}
        {activeTab === 'playlist' && <PlaylistTab channelId={chId} contentSource={ch.content_source ?? null} />}
        {activeTab === 'outputs'  && <OutputsTab ch={ch} />}
        {activeTab === 'config'   && <ConfigTab ch={ch} />}
        {activeTab === 'schedule' && <ScheduleTab channelId={chId} />}
        {activeTab === 'log'      && <LogTab channelId={chId} />}
        {activeTab === 'watcher'  && <WatcherPlaceholder channelId={chId} />}
        {activeTab === 'permissions' && <PermissionsPlaceholder channelId={chId} />}
      </div>

      {confirmDelete && (
        <DeleteChannelModal
          channelName={ch.name}
          pending={deletingCh}
          onCancel={() => setConfirmDelete(false)}
          onConfirm={() => {
            const name = ch.name;
            deleteCh(ch.id, {
              onSuccess: () => {
                toast(t('channels.deleted', { name }), 'warning');
                setConfirmDelete(false);
                navigate('/dashboard');
              },
              onError: () => toast(t('channels.deleteError'), 'danger'),
            });
          }}
        />
      )}
    </div>
  );
}

// ─── Overview Tab ─────────────────────────────────────────────────────────────
import type { ChannelStatus } from '@/api/types';

// Производное health-состояние из ch.state per A3.0 (нет отдельного ch.health).
function deriveHealth(state: ChannelStatus['state']): 'ok' | 'degraded' | 'fail' | 'idle' {
  switch (state) {
    case 'running':  return 'ok';
    case 'degraded': return 'degraded';
    case 'failed':   return 'fail';
    case 'stopped':  return 'idle';
  }
}

// Endpoint-string из per-driver полей OutputStatus (transport-discriminated).
function outputEndpoint(out: ChannelStatus['outputs'][number]): string {
  if (out.transport === 'rtmp')      return out.host ?? '—';
  if (out.transport === 'multicast') return out.address ? `${out.address}:${out.port ?? '?'}` : '—';
  if (out.transport === 'srt')       return out.port != null ? `:${out.port}` : '—';
  if (out.transport === 'hls')       return out.playlist_path ?? out.output_dir ?? '—';
  return out.address ?? out.host ?? out.output_dir ?? '—';
}

function outputType(out: ChannelStatus['outputs'][number]): string {
  return (out.transport ?? out.mode ?? 'unknown').toUpperCase();
}

function OverviewTab({ ch }: { ch: ChannelStatus }) {
  const { t } = useTranslation();
  const health = deriveHealth(ch.state);
  const fps = ch.fps_actual ?? 0;
  const fpsClass = fps > 0 && fps < ch.fps_target * 0.95 ? 'text-[var(--warning)]' : 'text-[var(--success)]';

  // Resolve current clip path/type by indexing into playlist. Cheap + cached.
  const { data: playlist = [] } = usePlaylist(ch.id);
  const currentEntry = ch.current_clip_index >= 0 ? playlist[ch.current_clip_index] : undefined;
  const currentPath = currentEntry && 'path' in currentEntry ? currentEntry.path : undefined;
  const currentIsLive = currentEntry && 'type' in currentEntry && currentEntry.type === 'live';
  const liveSource = currentIsLive
    ? (currentEntry as { input?: { type?: string; url?: string; source_name?: string } }).input
    : undefined;

  // Recent events scoped to this channel — last 10 from SSE bus.
  const { events } = useEventStream({ channelId: ch.id });
  const recent = events.slice(0, 10);

  return (
    <div className="flex flex-col gap-5">
      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">{t('overview.status')}</h2>
        <div className="grid grid-cols-2 sm:grid-cols-4 lg:grid-cols-8 gap-4">
          {[
            { label: t('overview.state'),      value: <HealthBadge status={ch.state} small /> },
            { label: t('overview.health'),     value: <HealthBadge status={health} small /> },
            { label: t('overview.fps'),        value: <span className={`font-mono tabular-nums text-sm ${fpsClass}`}>{fps > 0 ? fps.toFixed(2) : '—'}</span> },
            { label: t('overview.target'),     value: <span className="font-mono text-sm">{ch.fps_target} fps</span> },
            { label: t('overview.resolution'), value: ch.resolution },
            { label: t('overview.preset'),     value: ch.preset.toUpperCase() },
            { label: t('overview.bitrate'),    value: `${(ch.bitrate / 1_000_000).toFixed(1)} Mbps` },
            { label: t('overview.numa'),       value: `Node ${ch.numa_node ?? 0}` },
          ].map(({ label, value }) => (
            <div key={label}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{label}</dt>
              <dd className="text-sm text-[var(--text-primary)]">{value ?? '—'}</dd>
            </div>
          ))}
        </div>
      </Block>

      {ch.current_clip_index >= 0 && (() => {
        // Total duration derivation:
        //  - live: ∞ (длительность не определена для входящего потока)
        //  - image entry: явный duration в playlist
        //  - video / unknown: undefined (backend currently не отдаёт current_clip_duration)
        const fileEntry = !currentIsLive && currentEntry && 'duration' in currentEntry
          ? (currentEntry as { duration?: number })
          : undefined;
        const totalSec = currentIsLive ? Infinity : fileEntry?.duration;
        const remainingSec = ch.current_clip_remaining_sec;
        const elapsedSec = totalSec != null && isFinite(totalSec)
          ? Math.max(0, totalSec - remainingSec)
          : undefined;
        const progressPct = elapsedSec != null && totalSec != null && isFinite(totalSec) && totalSec > 0
          ? Math.min(100, Math.round((elapsedSec / totalSec) * 100))
          : undefined;

        // Render the clip label cell — для live это «<type live input>»,
        // для файла — путь, для не-известного — fallback.
        const clipLabel = currentIsLive
          ? t('overview.liveInputPlaceholder', { type: liveSource?.type ?? 'live' })
          : currentPath ?? null;

        const gridCols = currentIsLive
          ? 'grid grid-cols-2 sm:grid-cols-3 gap-4'
          : 'grid grid-cols-2 sm:grid-cols-4 gap-4';
        return (
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">{t('overview.nowPlaying')}</h2>
            <div className={gridCols}>
              <div>
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('overview.clip')}</dt>
                <dd className="text-sm font-mono text-[var(--text-primary)] truncate">
                  {clipLabel ?? <span className="text-[var(--text-muted)] font-sans">{t('overview.clipUnknown')}</span>}
                </dd>
              </div>
              <div>
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('overview.elapsed')}</dt>
                <dd className="text-sm tabular-nums text-[var(--text-primary)]">
                  {elapsedSec != null ? fmtDuration(elapsedSec) : '—'}
                </dd>
              </div>
              <div>
                <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('overview.duration')}</dt>
                <dd className="text-sm tabular-nums text-[var(--text-primary)]">
                  {currentIsLive ? '∞' : (totalSec != null ? fmtDuration(totalSec) : '—')}
                </dd>
              </div>
              {!currentIsLive && (
                <div>
                  <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{t('overview.progress')}</dt>
                  <dd className="text-sm tabular-nums text-[var(--text-muted)]">
                    {progressPct != null ? `${progressPct}%` : '—'}
                  </dd>
                </div>
              )}
            </div>
          </Block>
        );
      })()}

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">{t('overview.outputs')}</h2>
        {!ch.outputs?.length ? (
          <p className="text-sm text-[var(--text-muted)]">{t('overview.noOutputs')}</p>
        ) : (
          <div className="flex flex-col gap-2">
            {ch.outputs.map(out => (
              <div key={out.id} className="flex items-center gap-3 px-4 py-3 bg-canvas rounded-lg border border-[var(--border-subtle)]">
                <HealthBadge status={out.healthy ? 'ok' : 'fail'} small />
                <span className="text-sm font-bold text-[var(--text-primary)] min-w-[88px] flex-shrink-0">{outputType(out)}</span>
                <span className="text-sm font-mono text-[var(--text-muted)] flex-1 min-w-0 truncate">{outputEndpoint(out)}</span>
                <span className="text-xs text-[var(--text-muted)] flex-shrink-0">{out.id}</span>
              </div>
            ))}
          </div>
        )}
      </Block>

      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">{t('overview.recentEvents')}</h2>
        {recent.length === 0 ? (
          <p className="text-sm text-[var(--text-muted)]">{t('overview.noEvents')}</p>
        ) : (
          <div className="flex flex-col gap-1">
            {recent.map((ev, i) => (
              <div key={i} className="flex items-baseline gap-3 text-xs py-1 border-b last:border-0 border-[var(--border-subtle)]">
                <span className="font-mono tabular-nums text-[var(--text-muted)] w-20 flex-shrink-0">
                  {ev.ts ? new Date(ev.ts).toLocaleTimeString() : '—'}
                </span>
                <span className="font-mono text-[var(--accent)] w-44 flex-shrink-0 truncate">{ev.type}</span>
                <span className="text-[var(--text-muted)] truncate flex-1">
                  {(() => {
                    const { type: _t, ts: _ts, channel_id: _c, ...rest } = ev;
                    void _t; void _ts; void _c;
                    return Object.keys(rest).length === 0 ? '' : JSON.stringify(rest);
                  })()}
                </span>
              </div>
            ))}
          </div>
        )}
      </Block>
    </div>
  );
}

// ─── Playlist Tab ─────────────────────────────────────────────────────────────
import { usePlaylist, useAppendPlaylist, useDeletePlaylistItem, useClearPlaylist, useNotifyDeleted, useReplacePlaylist } from '@/api/queries/playlist';
import { List, AlertTriangle } from 'lucide-react';
import type { Playlist } from '@/api/types';

// Per-item editor modal: edits path/duration + transition_in/transition_out
// for a file entry, or live params + transitions for a live entry. The
// modal commits via PUT /playlist (replacePlaylist) so the entire list is
// re-uploaded with one item swapped.
function PlaylistItemEditor({
  item, onSubmit, onCancel, submitting,
}: {
  item: Playlist[number];
  onSubmit: (next: Playlist[number]) => void;
  onCancel: () => void;
  submitting: boolean;
}) {
  const { t } = useTranslation();
  const isLive = 'type' in item && item.type === 'live';
  // Local working copy — committed only on submit.
  const [draft, setDraft] = React.useState(() => JSON.parse(JSON.stringify(item)) as Playlist[number]);
  const [pickFile, setPickFile] = React.useState(false);

  // Helpers — typed updates without losing JSON catch-all.
  const setPath = (path: string) =>
    setDraft(d => ({ ...(d as { path?: string }), path }) as Playlist[number]);
  const setDuration = (s: string) => {
    const n = parseFloat(s);
    setDraft(d => {
      const next = { ...(d as { duration?: number }) };
      if (Number.isFinite(n) && n > 0) next.duration = n;
      else delete next.duration;
      return next as Playlist[number];
    });
  };
  const setTransition = (key: 'transition_in' | 'transition_out',
                        patch: Partial<{ type: string; duration_sec: number; easing: string }>) => {
    setDraft(d => {
      const next = { ...d } as Record<string, unknown>;
      const prev = (next[key] as Record<string, unknown> | undefined) ?? {};
      const merged = { ...prev, ...patch };
      // Drop the field entirely when no values remain (avoid sending {}).
      if (!merged.type && !merged.duration_sec && !merged.easing) delete next[key];
      else next[key] = merged;
      return next as Playlist[number];
    });
  };
  const setLive = (patch: Partial<{ duration_sec: number; warm_up_sec: number; loss_threshold_sec: number }>) =>
    setDraft(d => {
      const next = { ...d } as Record<string, unknown>;
      next.live = { ...(next.live as Record<string, unknown> | undefined), ...patch };
      return next as Playlist[number];
    });
  const setLiveInput = (patch: Partial<{ type: string; url: string; source_name: string }>) =>
    setDraft(d => {
      const next = { ...d } as Record<string, unknown>;
      next.input = { ...(next.input as Record<string, unknown> | undefined), ...patch };
      return next as Playlist[number];
    });

  const trIn  = (draft as { transition_in?:  { type?: string; duration_sec?: number; easing?: string } }).transition_in  ?? {};
  const trOut = (draft as { transition_out?: { type?: string; duration_sec?: number; easing?: string } }).transition_out ?? {};

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]';
  const labelCls = 'text-xs font-medium text-[var(--text-muted)] uppercase tracking-wider';

  useEscClose(onCancel, !submitting);

  return (
    <div className="fixed inset-0 z-50 bg-black/60 flex items-center justify-center p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-2xl max-h-[90vh] overflow-y-auto">
        <div className="px-6 py-4 border-b border-[var(--border-subtle)]">
          <h2 className="text-base font-semibold text-[var(--text-primary)]">{t('playlist.editItem')}</h2>
        </div>
        <div className="px-6 py-5 flex flex-col gap-4">
          {!isLive && (
            <>
              <div>
                <label className={labelCls}>{t('playlist.filePathLabel')}</label>
                <div className="flex gap-2 mt-1.5">
                  <input value={(draft as { path?: string }).path ?? ''}
                    onChange={e => setPath(e.target.value)}
                    className={`${inputCls} flex-1`} />
                  <button type="button" onClick={() => setPickFile(true)}
                    className="flex items-center gap-1 px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)]">
                    <FileText size={14} /> {t('common.browse')}
                  </button>
                </div>
              </div>
              <div>
                <label className={labelCls}>{t('playlist.durationLabel')}</label>
                <input value={(draft as { duration?: number }).duration ?? ''}
                  onChange={e => setDuration(e.target.value)}
                  type="number" step="0.1" min={0.1}
                  placeholder={t('channels.photoDurationPlaceholder')}
                  className={`${inputCls} mt-1.5 w-32`} />
                <p className="text-xs text-[var(--text-muted)] mt-1">{t('channels.photoDurationItemHint')}</p>
              </div>
            </>
          )}
          {isLive && (
            <>
              <div>
                <label className={labelCls}>{t('playlist.live.type')}</label>
                <select value={(draft as { input?: { type?: string } }).input?.type ?? ''}
                  onChange={e => setLiveInput({ type: e.target.value })}
                  className={`${inputCls} mt-1.5`}>
                  <option value="rtmp">RTMP</option>
                  <option value="rtsp">RTSP</option>
                  <option value="multicast">Multicast</option>
                  <option value="ndi">NDI</option>
                </select>
              </div>
              {(draft as { input?: { type?: string } }).input?.type === 'ndi' ? (
                <div>
                  <label className={labelCls}>{t('playlist.live.sourceName')}</label>
                  <input value={(draft as { input?: { source_name?: string } }).input?.source_name ?? ''}
                    onChange={e => setLiveInput({ source_name: e.target.value })}
                    className={`${inputCls} mt-1.5 w-full`} />
                </div>
              ) : (
                <div>
                  <label className={labelCls}>{t('playlist.live.url')}</label>
                  <input value={(draft as { input?: { url?: string } }).input?.url ?? ''}
                    onChange={e => setLiveInput({ url: e.target.value })}
                    className={`${inputCls} mt-1.5 w-full`}
                    placeholder="rtmp://, rtsp://, udp://group:port" />
                </div>
              )}
              <div className="grid grid-cols-3 gap-3">
                <div>
                  <label className={labelCls}>{t('playlist.live.durationSec')}</label>
                  <input type="number" step="0.1" min={0}
                    value={(draft as { live?: { duration_sec?: number } }).live?.duration_sec ?? ''}
                    onChange={e => setLive({ duration_sec: parseFloat(e.target.value) || 0 })}
                    className={`${inputCls} mt-1.5 w-full`} />
                </div>
                <div>
                  <label className={labelCls}>{t('playlist.live.warmUp')}</label>
                  <input type="number" step="0.1" min={0}
                    value={(draft as { live?: { warm_up_sec?: number } }).live?.warm_up_sec ?? ''}
                    onChange={e => setLive({ warm_up_sec: parseFloat(e.target.value) || 0 })}
                    className={`${inputCls} mt-1.5 w-full`} />
                </div>
                <div>
                  <label className={labelCls}>{t('playlist.live.lossThreshold')}</label>
                  <input type="number" step="0.1" min={0}
                    value={(draft as { live?: { loss_threshold_sec?: number } }).live?.loss_threshold_sec ?? ''}
                    onChange={e => setLive({ loss_threshold_sec: parseFloat(e.target.value) || 0 })}
                    className={`${inputCls} mt-1.5 w-full`} />
                </div>
              </div>
            </>
          )}

          {/* Transition_in / transition_out — same shape for file & live entries */}
          {(['transition_in', 'transition_out'] as const).map(key => {
            const tr = key === 'transition_in' ? trIn : trOut;
            return (
              <div key={key} className="border-t border-[var(--border-subtle)] pt-4">
                <div className={labelCls}>{t(`playlist.${key}`)}</div>
                <div className="grid grid-cols-3 gap-3 mt-2">
                  <select value={tr.type ?? ''}
                    onChange={e => setTransition(key, { type: e.target.value })}
                    className={inputCls}>
                    <option value="">{t('common.none') ?? '— none —'}</option>
                    <option value="crossfade">crossfade</option>
                    <option value="push_left">push_left</option>
                    <option value="push_right">push_right</option>
                    <option value="push_up">push_up</option>
                    <option value="push_down">push_down</option>
                  </select>
                  <input type="number" step="0.1" min={0}
                    value={tr.duration_sec ?? ''}
                    onChange={e => setTransition(key, { duration_sec: parseFloat(e.target.value) || 0 })}
                    placeholder="duration_sec"
                    className={inputCls} />
                  <select value={tr.easing ?? ''}
                    onChange={e => setTransition(key, { easing: e.target.value })}
                    className={inputCls}>
                    <option value="">linear</option>
                    <option value="ease_in">ease_in</option>
                    <option value="ease_out">ease_out</option>
                    <option value="ease_in_out">ease_in_out</option>
                  </select>
                </div>
              </div>
            );
          })}
        </div>
        <div className="px-6 py-4 border-t border-[var(--border-subtle)] flex justify-end gap-2">
          <button onClick={onCancel}
            className="px-4 py-1.5 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)]">
            {t('common.cancel')}
          </button>
          <button onClick={() => onSubmit(draft)} disabled={submitting}
            className="px-4 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md disabled:opacity-40">
            {submitting ? t('common.loading') : t('common.save')}
          </button>
        </div>
      </div>
      {pickFile && (
        <FilePickerModal
          initialPath={(((draft as { path?: string }).path ?? '/').replace(/\/[^/]*$/, '')) || '/'}
          acceptExtensions={MEDIA_EXTS}
          onSelect={(p) => { setPath(p); setPickFile(false); }}
          onCancel={() => setPickFile(false)}
        />
      )}
    </div>
  );
}

function PlaylistTab({
  channelId,
  contentSource,
}: {
  channelId: number;
  contentSource: ChannelStatus['content_source'];
}) {
  const toast = useToast();
  const { t } = useTranslation();
  const { data: items = [], isLoading } = usePlaylist(channelId);
  const { mutateAsync: appendAsync } = useAppendPlaylist(channelId);
  const { mutateAsync: replaceAsync, isPending: replacing } = useReplacePlaylist(channelId);
  const { mutate: remove }       = useDeletePlaylistItem(channelId);
  const { mutate: clearAll }     = useClearPlaylist(channelId);
  const { mutate: notifyDeleted }= useNotifyDeleted(channelId);
  // ContentSync bound channel: backend rejects all manual mutations with
  // 409 managed_by_content_sync. Disable add/clear buttons and surface a
  // banner so users understand why.
  const isManaged = contentSource !== null;
  const managedPath = contentSource?.mode === 'cache'
    ? contentSource.share_path
    : contentSource?.mode === 'passthrough'
      ? contentSource.source_path
      : '';
  // Slide-out add panels: 'file' = add single file, 'folder' = batch-add from folder, 'live' = add live source.
  const [panel, setPanel] = React.useState<null | 'file' | 'folder' | 'live'>(null);
  const [filePath, setFilePath] = React.useState('');
  const [fileDur,  setFileDur]  = React.useState('');
  const [folderPath, setFolderPath] = React.useState('');
  const [folderBusy, setFolderBusy] = React.useState(false);
  const [pickFile, setPickFile] = React.useState(false);
  const [pickFolder, setPickFolder] = React.useState(false);
  // Live-source form fields.
  const [liveType,   setLiveType]   = React.useState<'rtmp' | 'rtsp' | 'multicast' | 'ndi'>('rtmp');
  const [liveId,     setLiveId]     = React.useState('');
  const [liveUrl,    setLiveUrl]    = React.useState('');
  const [liveSource, setLiveSource] = React.useState('');
  const [liveDur,    setLiveDur]    = React.useState('');
  // Edit-item modal: holds the index of the entry being edited (-1 = closed).
  const [editingIdx, setEditingIdx] = React.useState<number>(-1);
  // Q4.2: deleted-paths CTA state (populated via watcher_status_change SSE or manual)
  const [deletedPaths, setDeletedPaths] = React.useState<string[]>([]);

  const closePanels = () => {
    setPanel(null);
    setFilePath(''); setFileDur('');
    setFolderPath('');
    setLiveType('rtmp'); setLiveId(''); setLiveUrl(''); setLiveSource(''); setLiveDur('');
  };

  // Reorder helper: swap idx with idx±1 and PUT the resulting list.
  const move = async (from: number, to: number) => {
    if (to < 0 || to >= items.length) return;
    const next = [...items];
    [next[from], next[to]] = [next[to], next[from]];
    try {
      await replaceAsync(next);
    } catch (e: unknown) {
      const msg = (e as { detail?: string })?.detail ?? (e as Error)?.message ?? 'Error';
      toast(msg, 'danger');
    }
  };

  const onEditSubmit = async (next: Playlist[number]) => {
    if (editingIdx < 0) return;
    const updated = items.map((it, i) => (i === editingIdx ? next : it));
    try {
      await replaceAsync(updated);
      toast(t('playlist.itemUpdated'), 'success');
      setEditingIdx(-1);
    } catch (e: unknown) {
      const msg = (e as { detail?: string })?.detail ?? (e as Error)?.message ?? 'Error';
      toast(msg, 'danger');
    }
  };

  const onAddLive = async () => {
    if (!liveId.trim()) { toast(t('playlist.live.idRequired'), 'warning'); return; }
    const wantsUrl = liveType !== 'ndi';
    if (wantsUrl && !liveUrl.trim()) { toast(t('playlist.live.urlRequired'), 'warning'); return; }
    if (!wantsUrl && !liveSource.trim()) { toast(t('playlist.live.sourceRequired'), 'warning'); return; }
    const entry: Record<string, unknown> = {
      type: 'live',
      id: liveId.trim(),
      input: wantsUrl
        ? { type: liveType, url: liveUrl.trim() }
        : { type: 'ndi',  source_name: liveSource.trim() },
    };
    const dur = parseFloat(liveDur);
    if (Number.isFinite(dur) && dur >= 0) entry.live = { duration_sec: dur };
    try {
      await appendAsync([entry as Playlist[number]]);
      toast(t('playlist.liveAdded'), 'success');
      closePanels();
    } catch (e: unknown) {
      const msg = (e as { detail?: string })?.detail ?? (e as Error)?.message ?? 'Error';
      toast(msg, 'danger');
    }
  };

  // Backend ignores `duration` for video; only image-entries use it. Empty
  // input → omit field → channel falls back to default_photo_duration.
  const buildFileEntry = (path: string, durStr: string) => {
    const dur = parseFloat(durStr);
    const entry: { type: 'file'; path: string; duration?: number } = { type: 'file', path };
    if (Number.isFinite(dur) && dur > 0) entry.duration = dur;
    return entry;
  };

  const onAddFile = async () => {
    if (!filePath.trim()) return;
    try {
      await appendAsync([buildFileEntry(filePath.trim(), fileDur)]);
      toast('Item added', 'success');
      closePanels();
    } catch (e: unknown) {
      const msg = (e as { detail?: string; message?: string })?.detail
        ?? (e as Error)?.message ?? 'Error';
      toast(msg, 'danger');
    }
  };

  // Folder add: enumerate via /api/system/browse (non-recursive), filter by
  // media extensions, append all matches in one batch mutation.
  const onAddFolder = async () => {
    const p = folderPath.trim();
    if (!p) return;
    setFolderBusy(true);
    try {
      const qs = new URLSearchParams({ path: p, include_files: 'true' });
      const data = await api.get<BrowseResponse>(`/api/system/browse?${qs.toString()}`);
      const matches = data.entries
        .filter(e => !e.is_dir)
        .filter(e => MEDIA_EXTS.some(ext => e.name.toLowerCase().endsWith(ext)));
      if (matches.length === 0) {
        toast(t('playlist.folderEmpty'), 'warning');
        return;
      }
      const entries = matches.map(m => ({ type: 'file' as const, path: m.full_path }));
      await appendAsync(entries);
      toast(t('playlist.folderAdded', { count: matches.length }), 'success');
      closePanels();
    } catch (e: unknown) {
      const msg = (e as { detail?: string; message?: string })?.detail
        ?? (e as Error)?.message ?? 'Error';
      toast(msg, 'danger');
    } finally {
      setFolderBusy(false);
    }
  };

  if (isLoading) return <div className="h-32 bg-surface2 rounded-xl animate-pulse" />;

  const inputCls = 'flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]';
  const labelCls = 'text-xs font-medium text-[var(--text-muted)] uppercase tracking-wider';
  const browseBtnCls = 'flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap';

  return (
    <div className="flex flex-col gap-4">
      {/* Q4.2: notify-deleted CTA — shown when ContentSync reports removed files */}
      {deletedPaths.length > 0 && (
        <div className="flex items-start gap-3 p-3 bg-[var(--warning)]/10 border border-[var(--warning)]/30 rounded-lg text-sm">
          <AlertTriangle size={15} className="text-[var(--warning)] mt-0.5 flex-shrink-0" />
          <div className="flex-1">
            <p className="font-medium text-[var(--text-primary)]">Files removed from share:</p>
            <p className="text-xs text-[var(--text-muted)] font-mono mt-0.5">{deletedPaths.join(', ')}</p>
          </div>
          <button
            onClick={() => notifyDeleted(deletedPaths, { onSuccess: () => { toast('Channel notified of deleted files', 'info'); setDeletedPaths([]); } })}
            className="px-2.5 py-1 text-xs bg-[var(--warning)] text-black rounded font-medium hover:bg-[var(--warning)]/80 transition-colors flex-shrink-0">
            Notify channel
          </button>
          <button onClick={() => setDeletedPaths([])} className="text-[var(--text-muted)] hover:text-[var(--text-primary)]">✕</button>
        </div>
      )}

      {isManaged && (
        <div className="flex items-start gap-3 p-3 bg-[var(--accent)]/10 border border-[var(--accent)]/30 rounded-lg text-sm">
          <AlertTriangle size={15} className="text-[var(--accent)] mt-0.5 flex-shrink-0" />
          <div className="flex-1">
            <p className="font-medium text-[var(--text-primary)] mb-0.5">{t('playlist.managedByContentSyncTitle')}</p>
            <p className="text-xs text-[var(--text-muted)]">{t('playlist.managedByContentSyncBody', { path: managedPath })}</p>
          </div>
        </div>
      )}

      <div className="flex gap-2 items-center flex-wrap">
        <button onClick={() => setPanel(panel === 'file' ? null : 'file')}
          disabled={isManaged}
          title={isManaged ? t('playlist.managedByContentSyncTitle') : undefined}
          className={`flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-md transition-colors disabled:opacity-40 disabled:cursor-not-allowed ${
            panel === 'file'
              ? 'bg-[var(--accent)] text-white'
              : 'bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white'
          }`}>
          <FilePlus size={14} /> {t('playlist.addFile')}
        </button>
        <button onClick={() => setPanel(panel === 'folder' ? null : 'folder')}
          disabled={isManaged}
          title={isManaged ? t('playlist.managedByContentSyncTitle') : undefined}
          className={`flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-md transition-colors disabled:opacity-40 disabled:cursor-not-allowed ${
            panel === 'folder'
              ? 'bg-[var(--accent)] text-white'
              : 'border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
          }`}>
          <FolderPlus size={14} /> {t('playlist.addFolder')}
        </button>
        <button onClick={() => setPanel(panel === 'live' ? null : 'live')}
          disabled={isManaged}
          title={isManaged ? t('playlist.managedByContentSyncTitle') : undefined}
          className={`flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-md transition-colors disabled:opacity-40 disabled:cursor-not-allowed ${
            panel === 'live'
              ? 'bg-[var(--danger)] text-white'
              : 'border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
          }`}>
          <Radio size={14} /> {t('playlist.addLive')}
        </button>
        <div className="flex-1" />
        <button onClick={() => { clearAll(); toast('Playlist cleared', 'warning'); }}
          disabled={isManaged}
          title={isManaged ? t('playlist.managedByContentSyncTitle') : undefined}
          className="px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors disabled:opacity-40 disabled:cursor-not-allowed disabled:hover:text-[var(--text-muted)]">
          {t('playlist.clear')}
        </button>
      </div>

      {panel === 'file' && (
        <Block padding="p-4">
          <label className={labelCls}>{t('playlist.filePathLabel')}</label>
          <div className="flex gap-2 mt-1.5">
            <input value={filePath} onChange={e => setFilePath(e.target.value)}
              placeholder={t('playlist.filePathPlaceholder')}
              className={inputCls}
              onKeyDown={e => { if (e.key === 'Enter') onAddFile(); }} />
            <button type="button" onClick={() => setPickFile(true)} className={browseBtnCls}>
              <FileText size={14} /> {t('common.browse')}
            </button>
            <input value={fileDur} onChange={e => setFileDur(e.target.value)}
              type="number" step="0.1" min={0.1}
              placeholder={t('channels.photoDurationPlaceholder')}
              title={t('channels.photoDurationItemHint')}
              className="w-24 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
            <button onClick={onAddFile} disabled={!filePath.trim()}
              className="px-4 py-2 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md disabled:opacity-40 transition-colors">
              {t('common.add')}
            </button>
            <button onClick={closePanels}
              className="px-3 py-2 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
              {t('common.cancel')}
            </button>
          </div>
        </Block>
      )}

      {panel === 'folder' && (
        <Block padding="p-4">
          <label className={labelCls}>{t('playlist.folderPathLabel')}</label>
          <div className="flex gap-2 mt-1.5">
            <input value={folderPath} onChange={e => setFolderPath(e.target.value)}
              placeholder={t('playlist.folderPathPlaceholder')}
              className={inputCls}
              onKeyDown={e => { if (e.key === 'Enter') onAddFolder(); }} />
            <button type="button" onClick={() => setPickFolder(true)} className={browseBtnCls}>
              <Folder size={14} /> {t('common.browse')}
            </button>
            <button onClick={onAddFolder} disabled={!folderPath.trim() || folderBusy}
              className="px-4 py-2 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md disabled:opacity-40 transition-colors">
              {folderBusy ? t('common.loading') : t('common.add')}
            </button>
            <button onClick={closePanels}
              className="px-3 py-2 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
              {t('common.cancel')}
            </button>
          </div>
        </Block>
      )}

      {panel === 'live' && (
        <Block padding="p-4">
          <div className="flex flex-col gap-3">
            <div className="grid grid-cols-2 sm:grid-cols-4 gap-3">
              <div>
                <label className={labelCls}>{t('playlist.live.type')}</label>
                <select value={liveType} onChange={e => setLiveType(e.target.value as typeof liveType)}
                  className={inputCls}>
                  <option value="rtmp">RTMP</option>
                  <option value="rtsp">RTSP</option>
                  <option value="multicast">Multicast</option>
                  <option value="ndi">NDI</option>
                </select>
              </div>
              <div>
                <label className={labelCls}>{t('playlist.live.id')}</label>
                <input value={liveId} onChange={e => setLiveId(e.target.value)}
                  placeholder="stream-1" className={inputCls} />
              </div>
              {liveType !== 'ndi' ? (
                <div className="col-span-2">
                  <label className={labelCls}>{t('playlist.live.url')}</label>
                  <input value={liveUrl} onChange={e => setLiveUrl(e.target.value)}
                    placeholder="rtmp://, rtsp://, udp://group:port" className={inputCls} />
                </div>
              ) : (
                <div className="col-span-2">
                  <label className={labelCls}>{t('playlist.live.sourceName')}</label>
                  <input value={liveSource} onChange={e => setLiveSource(e.target.value)}
                    placeholder="STUDIO (CAM-1)" className={inputCls} />
                </div>
              )}
            </div>
            <div className="flex gap-3 items-end">
              <div>
                <label className={labelCls}>{t('playlist.live.durationSec')}</label>
                <input type="number" step="0.1" min={0}
                  value={liveDur} onChange={e => setLiveDur(e.target.value)}
                  placeholder="0 = until loss"
                  className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm w-32" />
              </div>
              <div className="flex-1" />
              <button onClick={onAddLive}
                className="px-4 py-2 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md">
                {t('common.add')}
              </button>
              <button onClick={closePanels}
                className="px-3 py-2 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)]">
                {t('common.cancel')}
              </button>
            </div>
          </div>
        </Block>
      )}

      {pickFile && (
        <FilePickerModal
          initialPath={filePath?.replace(/\/[^/]*$/, '') || '/'}
          acceptExtensions={MEDIA_EXTS}
          onSelect={(p) => { setFilePath(p); setPickFile(false); }}
          onCancel={() => setPickFile(false)}
        />
      )}
      {pickFolder && (
        <FolderPickerModal
          initialPath={folderPath || '/'}
          onSelect={(p) => { setFolderPath(p); setPickFolder(false); }}
          onCancel={() => setPickFolder(false)}
        />
      )}

      {items.length === 0 ? (
        <EmptyState Icon={List} title={t('playlist.emptyTitle')} description={t('playlist.emptyDesc')} />
      ) : (
        <Block padding="p-0">
          {items.map((item, idx) => (
            <div key={idx} className="flex items-center gap-3 px-5 py-3 border-b last:border-0 border-[var(--border-subtle)]">
              <span className="text-xs text-[var(--text-muted)] w-5 text-right tabular-nums">{idx + 1}</span>
              <div className="flex-1 min-w-0">
                <div className="text-sm text-[var(--text-primary)] font-mono break-all">
                  {'type' in item && item.type === 'live'
                    ? `⚡ ${item.input?.url ?? item.input?.source_name ?? item.input?.type}`
                    : (item as { path?: string }).path ?? ''}
                </div>
                {(item as { transition_in?: { type: string; duration_sec?: number } }).transition_in && (
                  <div className="text-xs text-[var(--text-muted)] mt-0.5">
                    in: {(item as { transition_in?: { type: string; duration_sec?: number } }).transition_in?.type}
                    {(item as { transition_in?: { type: string; duration_sec?: number } }).transition_in?.duration_sec ? ` ${(item as { transition_in?: { type: string; duration_sec?: number } }).transition_in?.duration_sec}s` : ''}
                  </div>
                )}
              </div>
              <span className={`text-xs px-2 py-0.5 rounded-full ${'type' in item && item.type === 'live' ? 'bg-[var(--danger)]/15 text-[var(--danger)]' : 'bg-[var(--accent)]/10 text-[var(--accent)]'}`}>
                {'type' in item ? item.type ?? 'file' : 'file'}
              </span>
              <div className="flex items-center gap-0.5 flex-shrink-0">
                <button onClick={() => move(idx, idx - 1)}
                  disabled={isManaged || idx === 0 || replacing}
                  title={t('playlist.moveUp')}
                  className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 disabled:cursor-not-allowed transition-colors">
                  <ChevronUp size={14} />
                </button>
                <button onClick={() => move(idx, idx + 1)}
                  disabled={isManaged || idx === items.length - 1 || replacing}
                  title={t('playlist.moveDown')}
                  className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 disabled:cursor-not-allowed transition-colors">
                  <ChevronDown size={14} />
                </button>
                <button onClick={() => setEditingIdx(idx)}
                  disabled={isManaged}
                  title={t('playlist.editItem')}
                  className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 disabled:cursor-not-allowed transition-colors">
                  <Pencil size={13} />
                </button>
                <button onClick={() => remove(idx)}
                  disabled={isManaged}
                  title={isManaged ? t('playlist.managedByContentSyncTitle') : t('common.delete')}
                  className="p-1 text-[var(--text-muted)] hover:text-[var(--danger)] disabled:opacity-30 disabled:cursor-not-allowed disabled:hover:text-[var(--text-muted)] transition-colors">✕</button>
              </div>
            </div>
          ))}
        </Block>
      )}

      {editingIdx >= 0 && items[editingIdx] && (
        <PlaylistItemEditor
          item={items[editingIdx]}
          onSubmit={onEditSubmit}
          onCancel={() => setEditingIdx(-1)}
          submitting={replacing}
        />
      )}
    </div>
  );
}

// ─── Outputs Tab ──────────────────────────────────────────────────────────────
function OutputsTab({ ch }: { ch: ChannelStatus }) {
  const { t } = useTranslation();
  const toast = useToast();
  const [modalState, setModalState] = React.useState<
    { mode: 'create' } | { mode: 'edit'; out: ChannelStatus['outputs'][number] } | null
  >(null);
  const [confirmDelete, setConfirmDelete] = React.useState<string | null>(null);

  const { mutateAsync: addOutput, isPending: adding } = useAddOutput(ch.id);
  const { mutateAsync: patchOutput, isPending: patching } = usePatchOutput(ch.id);
  const { mutateAsync: deleteOutput, isPending: deleting } = useDeleteOutput(ch.id);
  const { mutateAsync: restartOutput } = useRestartOutput(ch.id);

  useEscClose(() => setConfirmDelete(null), confirmDelete !== null && !deleting);

  const handleSubmit = async (payload: Record<string, unknown>) => {
    try {
      if (modalState?.mode === 'create') {
        await addOutput(payload);
        toast(t('outputs.added'), 'success');
      } else if (modalState?.mode === 'edit') {
        const { id, type: _type, ...patch } = payload;
        void id; void _type;
        await patchOutput({ outputId: modalState.out.id, body: patch });
        toast(t('outputs.updated'), 'success');
      }
      setModalState(null);
    } catch (e: unknown) {
      const msg = (e as { detail?: string; message?: string })?.detail
        ?? (e as Error)?.message
        ?? t(modalState?.mode === 'create' ? 'outputs.addError' : 'outputs.updateError');
      toast(msg, 'danger');
    }
  };

  const handleDelete = async (outputId: string) => {
    try {
      await deleteOutput(outputId);
      toast(t('outputs.deleted'), 'success');
      setConfirmDelete(null);
    } catch (e: unknown) {
      const msg = (e as { detail?: string; message?: string })?.detail ?? t('outputs.deleteError');
      toast(msg, 'danger');
    }
  };

  const handleRestart = async (outputId: string) => {
    try {
      await restartOutput(outputId);
      toast(t('outputs.restarted', { id: outputId }), 'info');
    } catch (e: unknown) {
      const msg = (e as { detail?: string; message?: string })?.detail ?? t('outputs.restartError');
      toast(msg, 'danger');
    }
  };

  return (
    <div className="flex flex-col gap-4 w-full">
      <div className="flex gap-2">
        <button onClick={() => setModalState({ mode: 'create' })}
          className="px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md">
          + {t('outputs.addTitle')}
        </button>
      </div>
      {!ch.outputs?.length ? (
        <EmptyState Icon={MoreVertical} title={t('channels.overview.outputs')} description="Add at least one output for broadcasting" />
      ) : (
        <div className="flex flex-col gap-3">
          {ch.outputs.map(out => (
            <Block key={out.id} padding="p-4">
              <div className="flex items-center gap-3">
                <div className="h-8 px-2.5 rounded-lg bg-canvas border border-[var(--border-subtle)] flex items-center justify-center flex-shrink-0">
                  <span className="text-xs font-bold text-[var(--accent)] whitespace-nowrap">{outputType(out)}</span>
                </div>
                <span className="text-sm font-mono text-[var(--text-muted)] truncate min-w-0">{outputEndpoint(out)}</span>
                <span className="font-semibold text-sm text-[var(--text-primary)] flex-shrink-0">{out.id}</span>
                <HealthBadge status={out.healthy ? 'ok' : 'fail'} small />
                {out.connected != null && (
                  <span className={`text-xs px-1.5 py-0.5 rounded flex-shrink-0 ${out.connected ? 'bg-[var(--success)]/15 text-[var(--success)]' : 'bg-[var(--text-muted)]/15 text-[var(--text-muted)]'}`}>
                    {out.connected ? t('outputs.connected') : t('outputs.disconnected')}
                  </span>
                )}
                <div className="flex-1" />
                <div className="flex gap-1.5 flex-shrink-0">
                  <button onClick={() => handleRestart(out.id)}
                    title={t('outputs.restart')}
                    className="p-1.5 border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                    <RotateCw size={14} />
                  </button>
                  <button onClick={() => setModalState({ mode: 'edit', out })}
                    title={t('outputs.edit')}
                    className="p-1.5 border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                    <Pencil size={14} />
                  </button>
                  <button onClick={() => setConfirmDelete(out.id)}
                    title={t('outputs.delete')}
                    className="p-1.5 border border-[var(--border-subtle)] rounded text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                    <Trash2 size={14} />
                  </button>
                </div>
              </div>

              <div className="mt-3 pt-3 border-t border-[var(--border-subtle)] grid grid-cols-[repeat(auto-fit,minmax(140px,1fr))] gap-3 text-xs">
                <Counter label={t('outputs.bitrate')}     value={out.bitrate_bps != null ? `${(out.bitrate_bps / 1000).toFixed(0)} kbps` : '—'} />
                <Counter label={t('outputs.bytesSent')}    value={out.bytes_sent != null ? fmtBytes(out.bytes_sent) : '—'} />
                <Counter label={t('outputs.packetsSent')}  value={out.packets_sent != null ? out.packets_sent.toLocaleString() : '—'} />
                <Counter label={t('outputs.queueUsed')}    value={out.queue_bytes_limit > 0 ? `${fmtBytes(out.queue_bytes_used)} / ${fmtBytes(out.queue_bytes_limit)}` : '—'} />
                {out.packets_dropped != null && (
                  <Counter label={t('outputs.packetsDropped')} value={out.packets_dropped.toLocaleString()}
                    tone={out.packets_dropped > 0 ? 'warn' : undefined} />
                )}
                {out.queue_drops > 0 && (
                  <Counter label={t('outputs.queueDrops')} value={out.queue_drops.toLocaleString()} tone="warn" />
                )}
                {out.rtt_ms != null && (
                  <Counter label={t('outputs.rtt')} value={`${out.rtt_ms} ms`} />
                )}
                {out.reconnect_count != null && out.reconnect_count > 0 && (
                  <Counter label={t('outputs.reconnects')} value={out.reconnect_count.toLocaleString()}
                    tone={out.reconnect_count > 5 ? 'warn' : undefined} />
                )}
                {out.frames_sent != null && (
                  <Counter label={t('outputs.framesSent')} value={out.frames_sent.toLocaleString()} />
                )}
                {out.frames_dropped != null && out.frames_dropped > 0 && (
                  <Counter label={t('outputs.framesDropped')} value={out.frames_dropped.toLocaleString()} tone="warn" />
                )}
              </div>
            </Block>
          ))}
        </div>
      )}

      {modalState && (
        <OutputFormModal
          mode={modalState.mode}
          initialValues={modalState.mode === 'edit' ? modalState.out : undefined}
          onSubmit={handleSubmit}
          onCancel={() => setModalState(null)}
          submitting={adding || patching}
        />
      )}

      {confirmDelete && (
        <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
          <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md p-6 shadow-2xl">
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-2">{t('outputs.deleteConfirmTitle')}</h2>
            <p className="text-sm text-[var(--text-muted)] mb-5">{t('outputs.deleteConfirmBody', { id: confirmDelete })}</p>
            <div className="flex gap-2 justify-end">
              <button onClick={() => setConfirmDelete(null)}
                className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
                {t('common.cancel')}
              </button>
              <button onClick={() => handleDelete(confirmDelete)} disabled={deleting}
                className="px-4 py-2 bg-[var(--danger)] hover:opacity-90 text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {deleting ? t('common.loading') : t('outputs.delete')}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}

// ─── Config Tab ───────────────────────────────────────────────────────────────
import { useUpdateChannelConfig } from '@/api/queries/channels';

// Encoder modes accepted by the engine (see ChannelInstance.cpp:328).
const ENCODER_MODES = ['auto', 'cpu', 'nvenc', 'qsv', 'vaapi'] as const;
const PRESETS = [
  'ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
  'medium', 'slow', 'slower', 'veryslow',
] as const;
const TRANSITION_TYPES = [
  'crossfade',
  'wipe_left', 'wipe_right', 'wipe_up', 'wipe_down',
  'push_left', 'push_right', 'push_up', 'push_down',
  'dissolve', 'fade_black',
] as const;
const EASINGS = ['linear', 'ease_in', 'ease_out', 'ease_in_out'] as const;
const SAMPLE_RATES = [44100, 48000] as const;
const SINK_TYPES = ['none', 'file', 'db'] as const;

// Compact form-field wrapper — keeps every label/input pair consistent.
function Field({
  label, hint, warn, children, span,
}: {
  label: string;
  hint?: string;
  warn?: string;
  children: React.ReactNode;
  span?: 1 | 2;
}) {
  return (
    <div className={`flex flex-col gap-1.5 ${span === 2 ? 'col-span-2' : ''}`}>
      <label className="text-sm font-medium text-[var(--text-primary)]">{label}</label>
      {children}
      {hint && <p className="text-xs text-[var(--text-muted)]">{hint}</p>}
      {warn && <p className="text-xs text-[var(--warning)]">{warn}</p>}
    </div>
  );
}

const inputCls =
  'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]';
const selectCls = inputCls;
const roCls =
  'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-muted)] opacity-60';

function ConfigTab({ ch }: { ch: ChannelStatus }) {
  const toast = useToast();
  const { t } = useTranslation();
  const { mutateAsync: save, isPending } = useUpdateChannelConfig(ch.id);


  // ─ baseline values from ChannelStatus ─────────────────────────────────────
  const baseBitrate     = Math.round(ch.bitrate / 1000);
  const basePreset      = ch.preset ?? 'medium';
  const baseEncoderMode = ch.encoder_mode ?? 'auto';
  const baseGpuIndex    = ch.gpu_index ?? 0;
  const baseMaxB        = ch.max_b_frames ?? 2;
  const basePreloadSec  = ch.preload_sec ?? 4.0;
  const baseTimezone        = ch.channel_timezone ?? '';
  const baseInheritsServer  = ch.inherits_server_tz ?? !ch.channel_timezone;
  const baseEffectiveTz     = ch.effective_timezone ?? ch.channel_timezone ?? 'UTC';
  const basePhotoDur    = ch.default_photo_duration ?? 10;
  const baseFallback    = ch.fallback?.image_path ?? '';
  const baseContentMode = ch.content_source?.mode ?? 'none';
  const baseSrcPath     =
    ch.content_source && 'source_path' in ch.content_source
      ? ch.content_source.source_path
      : ch.content_source && 'share_path' in ch.content_source
        ? ch.content_source.share_path
        : '';
  const baseCachePath   =
    ch.content_source && 'cache_path' in ch.content_source ? ch.content_source.cache_path : '';
  const basePlaybackSink = ch.playback_log?.sink ?? 'none';
  const basePlaybackRetention =
    ch.playback_log?.retention_days != null ? String(ch.playback_log.retention_days) : '';
  const baseServiceName        = ch.mpegts?.service_name ?? 'LiveQX Channel';
  const baseServiceProvider    = ch.mpegts?.service_provider ?? 'LiveQX';
  const baseServiceId          = ch.mpegts?.service_id ?? 1;
  const baseTransportStreamId  = ch.mpegts?.transport_stream_id ?? 1;
  const baseOriginalNetworkId  = ch.mpegts?.original_network_id ?? 1;
  const baseMuxRate            = ch.mpegts?.mux_rate ?? 0;
  const baseSdtPeriodMs        = ch.mpegts?.sdt_period_ms ?? 0;
  const basePatPeriodMs        = ch.mpegts?.pat_period_ms ?? 0;
  // default_transition surface (см. fix48): backend всегда отдаёт type/duration/easing.
  // В UI «hardcut» отображается как «нет» (опция со значением '').
  const baseTrTypeRaw    = ch.default_transition?.type ?? 'hardcut';
  const baseTrType       = baseTrTypeRaw === 'hardcut' ? '' : baseTrTypeRaw;
  const baseTrDuration   = ch.default_transition?.duration ?? 2.0;
  const baseTrEasingRaw  = ch.default_transition?.easing ?? 'linear';
  const baseTrEasing     = baseTrEasingRaw === 'linear' ? '' : baseTrEasingRaw;

  // ─ form state ─────────────────────────────────────────────────────────────
  const [bitrate, setBitrate]       = React.useState(String(baseBitrate));
  const [preset, setPreset]         = React.useState(basePreset);
  const [encoderMode, setEncoderMode] = React.useState<string>(baseEncoderMode);
  const [gpuIndex, setGpuIndex]     = React.useState<string>(String(baseGpuIndex));
  const [maxB, setMaxB]             = React.useState(String(baseMaxB));
  const [preloadSec, setPreloadSec] = React.useState(String(basePreloadSec));
  const [timezone, setTimezone]         = React.useState(baseTimezone);
  const [inheritsServerTz, setInheritsServerTz] = React.useState(baseInheritsServer);
  const [audioBitrate, setAudioBitrate] = React.useState<string>(''); // empty = unchanged
  const [sampleRate, setSampleRate] = React.useState<string>('');     // empty = unchanged
  const [photoDur, setPhotoDur]     = React.useState(String(basePhotoDur));
  const [trType, setTrType]         = React.useState<string>(baseTrType);
  const [trDuration, setTrDuration] = React.useState<string>(String(baseTrDuration));
  const [trEasing, setTrEasing]     = React.useState<string>(baseTrEasing);
  const [fallbackPath, setFallbackPath] = React.useState(baseFallback);
  const [pickFallback, setPickFallback] = React.useState(false);
  const [csMode, setCsMode]         = React.useState<string>(baseContentMode);
  const [csSource, setCsSource]     = React.useState(baseSrcPath);
  const [csCache, setCsCache]       = React.useState(baseCachePath);
  const [pickSrc, setPickSrc]       = React.useState(false);
  const [pickCache, setPickCache]   = React.useState(false);
  const [sinkType, setSinkType]     = React.useState<string>(basePlaybackSink);
  const [retentionDays, setRetentionDays] = React.useState<string>(basePlaybackRetention);
  const [serviceName, setServiceName]             = React.useState<string>(baseServiceName);
  const [serviceProvider, setServiceProvider]     = React.useState<string>(baseServiceProvider);
  const [serviceId, setServiceId]                 = React.useState<string>(String(baseServiceId));
  const [transportStreamId, setTransportStreamId] = React.useState<string>(String(baseTransportStreamId));
  const [originalNetworkId, setOriginalNetworkId] = React.useState<string>(String(baseOriginalNetworkId));
  const [muxRateKbps, setMuxRateKbps]             = React.useState<string>(String(Math.round(baseMuxRate / 1000)));
  const [sdtPeriodMs, setSdtPeriodMs]             = React.useState<string>(String(baseSdtPeriodMs));
  const [patPeriodMs, setPatPeriodMs]             = React.useState<string>(String(basePatPeriodMs));

  const [rawJson, setRawJson] = React.useState('');
  const [rawError, setRawError] = React.useState<string | null>(null);

  const isStopped = ch.state === 'stopped';
  const fallbackChanged = fallbackPath.trim() !== baseFallback.trim();

  // ─ build the patch from touched fields only ───────────────────────────────
  // Submitting only changed fields keeps PATCHes minimal — the engine accepts
  // RFC 7396 merge-patches and ignores fields it doesn't recognise.
  const buildPatch = (): Record<string, unknown> => {
    const p: Record<string, unknown> = {};
    const nBit = parseInt(bitrate, 10);
    if (!isNaN(nBit) && nBit !== baseBitrate) p.bitrate = nBit * 1000;
    if (preset && preset !== basePreset) p.preset = preset;
    if (encoderMode !== baseEncoderMode) p.encoder_mode = encoderMode;
    const g = parseInt(gpuIndex, 10);
    if (!isNaN(g) && g !== baseGpuIndex) p.gpu_index = g;
    const nMaxB = parseInt(maxB, 10);
    if (!isNaN(nMaxB) && nMaxB !== baseMaxB) p.max_b_frames = nMaxB;
    const nPreload = parseFloat(preloadSec);
    if (!isNaN(nPreload) && Math.abs(nPreload - basePreloadSec) > 1e-9)
      p.preload_sec = nPreload;
    if (inheritsServerTz) {
      if (!baseInheritsServer) p.channel_timezone = null;
    } else {
      const tz = timezone.trim();
      if (tz && tz !== baseTimezone) p.channel_timezone = tz;
    }
    const audio: Record<string, unknown> = {};
    if (audioBitrate !== '') {
      const ab = parseInt(audioBitrate, 10);
      if (!isNaN(ab)) audio.bitrate = ab * 1000;
    }
    if (sampleRate !== '') {
      const sr = parseInt(sampleRate, 10);
      if (!isNaN(sr)) audio.sample_rate = sr;
    }
    if (Object.keys(audio).length) p.audio = audio;
    const nDur = parseFloat(photoDur);
    if (!isNaN(nDur) && nDur !== basePhotoDur) p.default_photo_duration = nDur;
    // default_transition: diff against base. «нет» в выпадашке = hardcut/linear.
    // Любое расхождение даже в одном поле — слать всю тройку целиком, чтобы
    // RFC 7396 merge не оставлял половину старого, половину нового.
    {
      const effType   = trType || 'hardcut';
      const effEasing = trEasing || 'linear';
      const nTrDur    = parseFloat(trDuration);
      const effDur    = !isNaN(nTrDur) ? nTrDur : baseTrDuration;
      const baseType   = baseTrType || 'hardcut';
      const baseEasing = baseTrEasing || 'linear';
      if (effType !== baseType
          || effEasing !== baseEasing
          || Math.abs(effDur - baseTrDuration) > 1e-9) {
        p.default_transition = { type: effType, duration: effDur, easing: effEasing };
      }
    }
    if (fallbackPath.trim() !== baseFallback.trim()) {
      p.fallback = { image_path: fallbackPath.trim() };
    }
    // content_source — patched only when stopped (server enforces).
    if (isStopped && csMode !== baseContentMode) {
      if (csMode === 'none') {
        p.content_source = null;
      } else if (csMode === 'passthrough' && csSource.trim()) {
        p.content_source = { mode: 'passthrough', source_path: csSource.trim() };
      } else if (csMode === 'cache' && csSource.trim() && csCache.trim()) {
        p.content_source = {
          mode: 'cache',
          share_path: csSource.trim(),
          cache_path: csCache.trim(),
        };
      }
    }
    // mpegts — IPTV/broadcast knobs. Applied through updateConfig(); most
    // fields take effect on the next start (or hot-swap where the muxer
    // supports it). Diff each field independently and only send changed
    // ones so an unrelated PATCH doesn't rewrite the whole subobject.
    {
      const mp: Record<string, unknown> = {};
      if (serviceName !== baseServiceName)         mp.service_name = serviceName;
      if (serviceProvider !== baseServiceProvider) mp.service_provider = serviceProvider;
      const nSid  = parseInt(serviceId, 10);
      const nTsid = parseInt(transportStreamId, 10);
      const nOnid = parseInt(originalNetworkId, 10);
      const nMux  = parseInt(muxRateKbps, 10);
      const nSdt  = parseInt(sdtPeriodMs, 10);
      const nPat  = parseInt(patPeriodMs, 10);
      if (!isNaN(nSid)  && nSid  !== baseServiceId)         mp.service_id = nSid;
      if (!isNaN(nTsid) && nTsid !== baseTransportStreamId) mp.transport_stream_id = nTsid;
      if (!isNaN(nOnid) && nOnid !== baseOriginalNetworkId) mp.original_network_id = nOnid;
      const baseMuxKbps = Math.round(baseMuxRate / 1000);
      if (!isNaN(nMux)  && nMux !== baseMuxKbps)            mp.mux_rate = nMux * 1000;
      if (!isNaN(nSdt)  && nSdt !== baseSdtPeriodMs)        mp.sdt_period_ms = nSdt;
      if (!isNaN(nPat)  && nPat !== basePatPeriodMs)        mp.pat_period_ms = nPat;
      if (Object.keys(mp).length) p.mpegts = mp;
    }
    // playback_log — patched only when stopped, и только если изменилось.
    if (isStopped) {
      const sinkChanged = sinkType !== basePlaybackSink;
      const retentionChanged =
        sinkType === 'db' && retentionDays !== basePlaybackRetention;
      if (sinkChanged || retentionChanged) {
        const pl: Record<string, unknown> = { sink: sinkType || 'none' };
        if (sinkType === 'db' && retentionDays !== '') {
          const r = parseInt(retentionDays, 10);
          if (!isNaN(r) && r >= 0) pl.retention_days = r;
        }
        p.playback_log = pl;
      }
    }
    return p;
  };

  const dirty = Object.keys(buildPatch()).length > 0;

  // beforeunload guard so an in-flight edit isn't lost on navigation/refresh.
  React.useEffect(() => {
    if (!dirty) return;
    const h = (e: BeforeUnloadEvent) => { e.preventDefault(); e.returnValue = ''; };
    window.addEventListener('beforeunload', h);
    return () => window.removeEventListener('beforeunload', h);
  }, [dirty]);

  const onSave = async () => {
    const p = buildPatch();
    if (!Object.keys(p).length) { toast(t('channels.config.noChanges'), 'info'); return; }
    try {
      await save(p);
      toast(t('channels.config.saved'), 'success');
    } catch (err) {
      const msg = err instanceof Error ? err.message : t('channels.config.saveError');
      toast(msg, 'danger');
    }
  };

  const onReset = () => {
    setBitrate(String(baseBitrate));
    setPreset(basePreset);
    setEncoderMode('');
    setGpuIndex('');
    setMaxB(String(baseMaxB));
    setPreloadSec(String(basePreloadSec));
    setTimezone(baseTimezone);
    setAudioBitrate('');
    setSampleRate('');
    setPhotoDur(String(basePhotoDur));
    setTrType(baseTrType); setTrDuration(String(baseTrDuration)); setTrEasing(baseTrEasing);
    setFallbackPath(baseFallback);
    setCsMode(baseContentMode); setCsSource(baseSrcPath); setCsCache(baseCachePath);
    setSinkType(''); setRetentionDays('');
  };

  const onSubmitRaw = async () => {
    let parsed: unknown;
    try { parsed = JSON.parse(rawJson); }
    catch (e) {
      setRawError(e instanceof Error ? e.message : t('channels.config.rawInvalid'));
      return;
    }
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
      setRawError(t('channels.config.rawInvalid'));
      return;
    }
    setRawError(null);
    try {
      await save(parsed as Record<string, unknown>);
      toast(t('channels.config.saved'), 'success');
      setRawJson('');
    } catch (err) {
      const msg = err instanceof Error ? err.message : t('channels.config.saveError');
      toast(msg, 'danger');
    }
  };

  return (
    <div className="grid grid-cols-1 lg:grid-cols-2 gap-5 w-full">
      {/* Form column */}
      <div className="flex flex-col gap-5 min-w-0">
        {dirty && (
          <div className="flex items-center justify-between border-b border-[var(--border-subtle)] pb-2">
            <h2 className="text-sm font-semibold text-[var(--text-primary)]">{t('channels.config.tabForm')}</h2>
            <span className="text-xs text-[var(--warning)] uppercase tracking-wider">
              • {t('channels.config.dirtyBadge')}
            </span>
          </div>
        )}
        <>
          {/* Identity (read-only) */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secIdentity')}
            </h2>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.fieldId')}>
                <input value={String(ch.id)} disabled className={roCls} />
              </Field>
              <Field label={t('channels.config.fieldNameRO')} hint={t('channels.nameImmutableHint')}>
                <input value={ch.name} disabled className={roCls} />
              </Field>
              <Field label={t('channels.config.fieldState')}>
                <input value={ch.state} disabled className={roCls} />
              </Field>
              <Field label={t('channels.config.fieldNumaRO')}>
                <input value={String(ch.numa_node)} disabled className={roCls} />
              </Field>
            </div>
          </Block>

          {/* Encoder */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secEncoder')}
            </h2>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.fieldResolutionRO')}>
                <input value={ch.resolution} disabled className={roCls} />
              </Field>
              <Field label={t('channels.config.fieldFpsRO')}>
                <input value={String(ch.fps_target)} disabled className={roCls} />
              </Field>
              <Field label={t('channels.fieldBitrate')}>
                <input type="number" min={100} value={bitrate}
                  onChange={e => setBitrate(e.target.value)} className={inputCls} />
              </Field>
              <Field label={t('channels.fieldPreset')}>
                <select value={preset} onChange={e => setPreset(e.target.value)} className={selectCls}>
                  {PRESETS.map(p => <option key={p} value={p}>{p}</option>)}
                </select>
              </Field>
              <Field label={t('channels.config.fieldEncoderMode')}
                warn={encoderMode !== baseEncoderMode ? t('channels.config.restartRequired') : undefined}>
                <select value={encoderMode} onChange={e => setEncoderMode(e.target.value)} className={selectCls}>
                  {ENCODER_MODES.map(m => <option key={m} value={m}>{m}</option>)}
                </select>
              </Field>
              {(encoderMode === 'nvenc' || encoderMode === 'qsv' || encoderMode === 'vaapi') && (
                <Field label={t('channels.config.fieldGpuIndex')}>
                  <input type="number" min={0} value={gpuIndex}
                    onChange={e => setGpuIndex(e.target.value)} className={inputCls} placeholder="0" />
                </Field>
              )}
              <Field label={t('channels.config.fieldMaxB')}>
                <input type="number" min={0} max={16} value={maxB}
                  onChange={e => setMaxB(e.target.value)} className={inputCls} />
              </Field>
              <Field label={t('channels.config.fieldPreloadSec')}
                hint={t('channels.config.preloadSecHint')}
                warn={parseFloat(preloadSec) !== basePreloadSec
                  ? t('channels.config.restartRequired')
                  : undefined}>
                <input type="number" step="0.5" min={0.5} max={30} value={preloadSec}
                  onChange={e => setPreloadSec(e.target.value)} className={inputCls} />
              </Field>
            </div>
          </Block>

          {/* Audio */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secAudio')}
            </h2>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.fieldAudioBitrate')}>
                <input type="number" min={32} max={512} value={audioBitrate}
                  onChange={e => setAudioBitrate(e.target.value)}
                  className={inputCls} placeholder="128" />
              </Field>
              <Field label={t('channels.config.fieldAudioSampleRate')}>
                <select value={sampleRate} onChange={e => setSampleRate(e.target.value)} className={selectCls}>
                  <option value="">{t('common.none')}</option>
                  {SAMPLE_RATES.map(r => <option key={r} value={r}>{r} Hz</option>)}
                </select>
              </Field>
            </div>
          </Block>

          {/* MPEG-TS / IPTV */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secMpegts')}
            </h2>
            <p className="text-xs text-[var(--text-muted)] mb-4">
              {t('channels.config.mpegtsHint')}
            </p>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.serviceName')} hint={t('channels.config.serviceNameHint')}>
                <input value={serviceName}
                  onChange={e => setServiceName(e.target.value)}
                  className={inputCls} placeholder="LiveQX Channel" />
              </Field>
              <Field label={t('channels.config.serviceProvider')} hint={t('channels.config.serviceProviderHint')}>
                <input value={serviceProvider}
                  onChange={e => setServiceProvider(e.target.value)}
                  className={inputCls} placeholder="LiveQX" />
              </Field>
              <Field label={t('channels.config.serviceId')} hint={t('channels.config.serviceIdHint')}>
                <input type="number" min={1} max={65535} value={serviceId}
                  onChange={e => setServiceId(e.target.value)}
                  className={inputCls} placeholder="1" />
              </Field>
              <Field label={t('channels.config.transportStreamId')} hint={t('channels.config.transportStreamIdHint')}>
                <input type="number" min={1} max={65535} value={transportStreamId}
                  onChange={e => setTransportStreamId(e.target.value)}
                  className={inputCls} placeholder="1" />
              </Field>
              <Field label={t('channels.config.originalNetworkId')} hint={t('channels.config.originalNetworkIdHint')}>
                <input type="number" min={1} max={65535} value={originalNetworkId}
                  onChange={e => setOriginalNetworkId(e.target.value)}
                  className={inputCls} placeholder="1" />
              </Field>
              <Field label={t('channels.config.muxRateKbps')} hint={t('channels.config.muxRateHint')}>
                <input type="number" min={0} value={muxRateKbps}
                  onChange={e => setMuxRateKbps(e.target.value)}
                  className={inputCls} placeholder="0" />
              </Field>
              <Field label={t('channels.config.sdtPeriodMs')} hint={t('channels.config.sdtPeriodHint')}>
                <input type="number" min={0} value={sdtPeriodMs}
                  onChange={e => setSdtPeriodMs(e.target.value)}
                  className={inputCls} placeholder="0" />
              </Field>
              <Field label={t('channels.config.patPeriodMs')} hint={t('channels.config.patPeriodHint')}>
                <input type="number" min={0} value={patPeriodMs}
                  onChange={e => setPatPeriodMs(e.target.value)}
                  className={inputCls} placeholder="0" />
              </Field>
            </div>
          </Block>

          {/* Photo defaults */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secPhoto')}
            </h2>
            <Field label={t('channels.fieldPhotoDuration')} hint={t('channels.photoDurationHint')}>
              <input type="number" step="0.1" min={0.1} value={photoDur}
                onChange={e => setPhotoDur(e.target.value)} className={inputCls} />
            </Field>
          </Block>

          {/* Schedule timezone */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secSchedule')}
            </h2>

            <label className="flex items-center gap-2 cursor-pointer mb-4">
              <div onClick={() => setInheritsServerTz(v => !v)}
                   className={`w-9 h-5 rounded-full relative transition-colors ${inheritsServerTz ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
                <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${inheritsServerTz ? 'left-4' : 'left-0.5'}`} />
              </div>
              <span className="text-sm text-[var(--text-primary)]">
                {t('channels.config.inheritServerTz')}
              </span>
              <span className="text-xs text-[var(--text-muted)] ml-2 font-mono">
                ({baseEffectiveTz})
              </span>
            </label>

            {!inheritsServerTz && (
              <Field label={t('channels.config.fieldTimezone')}
                hint={t('channels.config.timezoneHint')}
                warn={(timezone.trim() !== baseTimezone || inheritsServerTz !== baseInheritsServer)
                       ? t('channels.config.restartRequired') : undefined}>
                <input value={timezone} onChange={e => setTimezone(e.target.value)}
                  placeholder="Europe/Moscow" className={inputCls} />
              </Field>
            )}
            {inheritsServerTz && inheritsServerTz !== baseInheritsServer && (
              <p className="text-xs text-[var(--warning)] mt-1">
                {t('channels.config.restartRequired')}
              </p>
            )}
          </Block>

          {/* Default transition */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secTransition')}
            </h2>
            <div className="grid grid-cols-3 gap-4">
              <Field label={t('channels.config.fieldTransitionType')}>
                <select value={trType} onChange={e => setTrType(e.target.value)} className={selectCls}>
                  <option value="">{t('common.none')}</option>
                  {TRANSITION_TYPES.map(x => {
                    const labelKey = ({
                      crossfade:  'trCrossfade',
                      wipe_left:  'trWipeLeft',
                      wipe_right: 'trWipeRight',
                      wipe_up:    'trWipeUp',
                      wipe_down:  'trWipeDown',
                      push_left:  'trPushLeft',
                      push_right: 'trPushRight',
                      push_up:    'trPushUp',
                      push_down:  'trPushDown',
                      dissolve:   'trDissolve',
                      fade_black: 'trFadeBlack',
                    } as const)[x];
                    return <option key={x} value={x}>{t(`channels.config.${labelKey}`)}</option>;
                  })}
                </select>
              </Field>
              <Field label={t('channels.config.fieldTransitionDuration')}>
                <input type="number" step="0.1" min={0} value={trDuration}
                  onChange={e => setTrDuration(e.target.value)}
                  className={inputCls} placeholder="2.0" />
              </Field>
              <Field label={t('channels.config.fieldTransitionEasing')}>
                <select value={trEasing} onChange={e => setTrEasing(e.target.value)} className={selectCls}>
                  <option value="">{t('common.none')}</option>
                  {EASINGS.map(x => {
                    const labelKey = ({
                      linear:      'easingLinear',
                      ease_in:     'easingEaseIn',
                      ease_out:    'easingEaseOut',
                      ease_in_out: 'easingEaseInOut',
                    } as const)[x];
                    return <option key={x} value={x}>{t(`channels.config.${labelKey}`)}</option>;
                  })}
                </select>
              </Field>
            </div>
          </Block>

          {/* Fallback */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secFallback')}
            </h2>
            <Field label={t('channels.fieldFallback')}
              hint={t('channels.fallbackHint')}
              warn={fallbackChanged ? t('channels.fallbackRequiresRestart') : undefined}>
              <div className="flex gap-2">
                <input value={fallbackPath} onChange={e => setFallbackPath(e.target.value)}
                  placeholder={t('channels.fallbackPlaceholder')}
                  className={`flex-1 ${inputCls}`} />
                <button type="button" onClick={() => setPickFallback(true)}
                  className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors whitespace-nowrap">
                  <FileText size={14} /> {t('common.browse')}
                </button>
              </div>
            </Field>
          </Block>

          {/* Content source — stopped-only */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secContent')}
              {!isStopped && (
                <span className="ml-3 text-xs font-normal text-[var(--warning)]">
                  {t('channels.config.stoppedRequired')}
                </span>
              )}
            </h2>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.fieldContentMode')}>
                <select value={csMode} onChange={e => setCsMode(e.target.value)}
                  disabled={!isStopped}
                  className={`${selectCls} ${!isStopped ? 'opacity-60 cursor-not-allowed' : ''}`}>
                  <option value="none">{t('channels.config.modeNone')}</option>
                  <option value="passthrough">{t('channels.config.modePassthrough')}</option>
                  <option value="cache">{t('channels.config.modeCache')}</option>
                </select>
                <p className="text-xs text-[var(--text-muted)] mt-1">
                  {csMode === 'passthrough'
                    ? t('channels.config.modePassthroughHint')
                    : csMode === 'cache'
                      ? t('channels.config.modeCacheHint')
                      : t('channels.config.modeNoneHint')}
                </p>
              </Field>
              {csMode !== 'none' && (
                <Field label={t('channels.config.fieldContentSource')} span={2}>
                  <div className="flex gap-2">
                    <input value={csSource} onChange={e => setCsSource(e.target.value)}
                      disabled={!isStopped}
                      className={`flex-1 ${inputCls} ${!isStopped ? 'opacity-60' : ''}`} />
                    <button type="button" onClick={() => setPickSrc(true)}
                      disabled={!isStopped}
                      className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors disabled:opacity-50 whitespace-nowrap">
                      <Folder size={14} /> {t('common.browse')}
                    </button>
                  </div>
                </Field>
              )}
              {csMode === 'cache' && (
                <Field label={t('channels.config.fieldContentCache')} span={2}>
                  <div className="flex gap-2">
                    <input value={csCache} onChange={e => setCsCache(e.target.value)}
                      disabled={!isStopped}
                      className={`flex-1 ${inputCls} ${!isStopped ? 'opacity-60' : ''}`} />
                    <button type="button" onClick={() => setPickCache(true)}
                      disabled={!isStopped}
                      className="flex items-center gap-1 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors disabled:opacity-50 whitespace-nowrap">
                      <Folder size={14} /> {t('common.browse')}
                    </button>
                  </div>
                </Field>
              )}
            </div>
          </Block>

          {/* Playback log — stopped-only */}
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">
              {t('channels.config.secPlaybackLog')}
              {!isStopped && (
                <span className="ml-3 text-xs font-normal text-[var(--warning)]">
                  {t('channels.config.stoppedRequired')}
                </span>
              )}
            </h2>
            <div className="grid grid-cols-2 gap-4">
              <Field label={t('channels.config.fieldSinkType')}>
                <select value={sinkType} onChange={e => setSinkType(e.target.value)}
                  disabled={!isStopped}
                  className={`${selectCls} ${!isStopped ? 'opacity-60 cursor-not-allowed' : ''}`}>
                  {SINK_TYPES.map(s => <option key={s} value={s}>
                    {s === 'none' ? t('channels.config.sinkNone')
                     : s === 'file' ? t('channels.config.sinkFile')
                     : t('channels.config.sinkDb')}
                  </option>)}
                </select>
                <p className="text-xs text-[var(--text-muted)] mt-1">
                  {sinkType === 'file'
                    ? t('channels.config.sinkFileHint')
                    : sinkType === 'db'
                      ? t('channels.config.sinkDbHint')
                      : t('channels.config.sinkNoneHint')}
                </p>
              </Field>
              {sinkType === 'db' && (
                <Field label={t('channels.config.fieldRetentionDays')}>
                  <input type="number" min={0} value={retentionDays}
                    onChange={e => setRetentionDays(e.target.value)}
                    disabled={!isStopped}
                    className={`${inputCls} ${!isStopped ? 'opacity-60' : ''}`}
                    placeholder="0" />
                </Field>
              )}
            </div>
          </Block>

          <div className="flex gap-2">
            <button disabled={isPending || !dirty} onClick={onSave}
              className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
              {isPending ? `${t('common.save')}…` : t('common.save')}
            </button>
            <button disabled={!dirty} onClick={onReset}
              className="px-4 py-2 border border-[var(--border-subtle)] text-[var(--text-muted)] text-sm rounded-md hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
              {t('common.cancel')}
            </button>
          </div>
        </>
      </div>

      {/* Raw JSON column */}
      <div className="flex flex-col gap-5 min-w-0">
        <div className="lg:sticky lg:top-0">
          <Block>
            <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-2">
              {t('channels.config.tabRaw')}
            </h2>
            <p className="text-xs text-[var(--text-muted)] mb-3">{t('channels.config.rawHint')}</p>
            <textarea value={rawJson} onChange={e => { setRawJson(e.target.value); setRawError(null); }}
              placeholder={t('channels.config.rawPlaceholder')}
              spellCheck={false}
              rows={24}
              className={`${inputCls} w-full font-mono text-xs leading-relaxed resize-y`} />
            {rawError && <p className="text-xs text-[var(--danger)] mt-2">{rawError}</p>}
            <div className="flex gap-2 mt-3">
              <button disabled={isPending || !rawJson.trim()} onClick={onSubmitRaw}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                {isPending ? `${t('common.save')}…` : t('channels.config.rawSubmit')}
              </button>
              <button disabled={!rawJson} onClick={() => { setRawJson(''); setRawError(null); }}
                className="px-4 py-2 border border-[var(--border-subtle)] text-[var(--text-muted)] text-sm rounded-md hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
                {t('common.cancel')}
              </button>
            </div>
          </Block>
        </div>
      </div>

      {pickFallback && (
        <FilePickerModal
          initialPath={fallbackPath?.replace(/\/[^/]*$/, '') || '/'}
          acceptExtensions={FALLBACK_EXTS}
          onSelect={(p) => { setFallbackPath(p); setPickFallback(false); }}
          onCancel={() => setPickFallback(false)}
        />
      )}
      {pickSrc && (
        <FolderPickerModal
          initialPath={csSource || '/'}
          onSelect={(p) => { setCsSource(p); setPickSrc(false); }}
          onCancel={() => setPickSrc(false)}
        />
      )}
      {pickCache && (
        <FolderPickerModal
          initialPath={csCache || '/'}
          onSelect={(p) => { setCsCache(p); setPickCache(false); }}
          onCancel={() => setPickCache(false)}
        />
      )}
    </div>
  );
}

// ─── Schedule Tab ─────────────────────────────────────────────────────────────
import { useSchedule, useReplaceSchedule, useScheduleActive, useScheduleUpcoming } from '@/api/queries/playlist';
import { Calendar, Clock } from 'lucide-react';
import type { ScheduleEntry } from '@/api/types';

// Сводка recurrence для строки entry'а — соответствует A3.0-схеме Recurrence (oneOf по kind).
function describeRecurrence(r: ScheduleEntry['recurrence']): string {
  switch (r.kind) {
    case 'once':    return `once @ ${r.start_at} → ${r.end_at}`;
    case 'daily':   return `daily ${r.start_time}–${r.end_time}`;
    case 'weekly':  return `weekly ${r.start_time}–${r.end_time} [${r.days_of_week.join(',')}]`;
    case 'monthly': return `monthly ${r.start_time}–${r.end_time} [days ${r.days_of_month.join(',')}]`;
  }
}

// Computes activations (start/end in unix sec) that overlap a local-time
// 24h window starting at `dayStartLocalSec`. Mirrors Scheduler::nextActivation
// rules: UTC start_time/end_time, ISO weekday (1=Mon..7=Sun), effective_from/to
// gating, valid days_of_week/days_of_month.
function activationsForDay(
  entries: ScheduleEntry[],
  dayStartLocalSec: number,
): { entry_id: string; starts_at: number; ends_at: number; priority?: number }[] {
  const dayEndLocalSec = dayStartLocalSec + 86400;
  const out: { entry_id: string; starts_at: number; ends_at: number; priority?: number }[] = [];

  const parseHM = (s: string): [number, number] => {
    const [h, m] = s.split(':').map(Number);
    return [h || 0, m || 0];
  };
  const parseDate = (s: string): Date | null => {
    // Parse YYYY-MM-DD as local midnight so comparisons against cand
    // (also local midnight) are consistent regardless of UTC offset.
    const [y, m, d] = s.split('-').map(Number);
    if (!y || !m || !d) return null;
    const dt = new Date(y, m - 1, d, 0, 0, 0, 0);
    return isNaN(dt.getTime()) ? null : dt;
  };

  for (const e of entries) {
    const r: any = e.recurrence;
    if (r.kind === 'once') {
      const startMs = Date.parse(r.start_at);
      const endMs   = Date.parse(r.end_at);
      if (!Number.isFinite(startMs) || !Number.isFinite(endMs)) continue;
      const start = Math.floor(startMs / 1000);
      const end   = Math.floor(endMs / 1000);
      if (end > dayStartLocalSec && start < dayEndLocalSec)
        out.push({ entry_id: e.id, starts_at: start, ends_at: end, priority: e.priority });
      continue;
    }
    const [shH, shM] = parseHM(r.start_time);
    const [ehH, ehM] = parseHM(r.end_time);
    const effFrom = e.effective_from ? parseDate(e.effective_from) : null;
    const effTo   = (e as any).effective_to ? parseDate((e as any).effective_to) : null;

    // Schedule times (HH:MM) are server-local wall-clock times, same as the
    // C++ Scheduler which uses localtime_r. Mirror that here: build midnight
    // in browser-local time so the timeline positions match.
    const anchor = new Date(dayStartLocalSec * 1000);
    for (let offset = -1; offset <= 1; ++offset) {
      const localMidnight = new Date(
        anchor.getFullYear(),
        anchor.getMonth(),
        anchor.getDate() + offset,
        0, 0, 0, 0,
      );
      const localMidSec = Math.floor(localMidnight.getTime() / 1000);
      const localStart  = localMidSec + shH * 3600 + shM * 60;
      const localEnd    = localMidSec + ehH * 3600 + ehM * 60;
      if (localEnd <= dayStartLocalSec || localStart >= dayEndLocalSec) continue;

      const cand = localMidnight;
      if (effFrom && cand < effFrom) continue;
      if (effTo   && cand > effTo)   continue;
      if (r.kind === 'weekly') {
        const isoDow = ((cand.getDay() + 6) % 7) + 1; // 1=Mon..7=Sun, local weekday
        if (!r.days_of_week.includes(isoDow)) continue;
      }
      if (r.kind === 'monthly') {
        if (!r.days_of_month.includes(cand.getDate())) continue;
      }
      out.push({ entry_id: e.id, starts_at: localStart, ends_at: localEnd, priority: e.priority });
    }
  }
  return out.sort((a, b) => a.starts_at - b.starts_at);
}

// 24h timeline ribbon — bands for activations that fall on the selected day.
// Today: live from /upcoming; other days: computed client-side from entries.
// All times in unix-seconds.
function ScheduleTimeline({
  entries,
  activeEntryId,
}: {
  entries: ScheduleEntry[];
  activeEntryId: string | null | undefined;
}) {
  const { t, i18n } = useTranslation();
  // Selected day midnight (local), stored as ms-since-epoch so it survives renders.
  const todayMidnight = () => { const d = new Date(); d.setHours(0,0,0,0); return d.getTime(); };
  const [selectedMs, setSelectedMs] = React.useState<number>(todayMidnight);
  const datePickerRef = React.useRef<HTMLInputElement>(null);

  const dayStart = Math.floor(selectedMs / 1000);
  const dayEnd = dayStart + 86400;
  const isToday = selectedMs === todayMidnight();

  const [now, setNow] = React.useState(Math.floor(Date.now()/1000));
  React.useEffect(() => {
    const id = setInterval(() => setNow(Math.floor(Date.now()/1000)), 60_000);
    return () => clearInterval(id);
  }, []);

  // Always use activationsForDay so the currently-active entry (which is no
  // longer in `upcoming` once it has started) stays visible on the timeline.
  // activeEntryId handles the green highlight for the running band.
  const dayItems = activationsForDay(entries, dayStart);

  // Pixel-percent layout via positions on a vertical 24h column.
  const pct = (sec: number) => ((sec - dayStart) / 86400) * 100;

  const bands = dayItems
    .map(u => {
      const start = Math.max(u.starts_at, dayStart);
      const end   = Math.min(u.ends_at, dayEnd);
      if (end <= start) return null;
      return { ...u, top: pct(start), height: pct(end) - pct(start) };
    })
    .filter((x): x is NonNullable<typeof x> => x !== null);

  const shiftDay = (deltaDays: number) => {
    const d = new Date(selectedMs);
    d.setDate(d.getDate() + deltaDays);
    d.setHours(0,0,0,0);
    setSelectedMs(d.getTime());
  };
  const goToday = () => setSelectedMs(todayMidnight());
  const onPickDate = (e: React.ChangeEvent<HTMLInputElement>) => {
    if (!e.target.value) return;
    const [y, m, day] = e.target.value.split('-').map(Number);
    const d = new Date(y, m - 1, day, 0, 0, 0, 0);
    setSelectedMs(d.getTime());
  };

  // YYYY-MM-DD for native date input (in local time).
  const selectedDate = new Date(selectedMs);
  const yyyy = selectedDate.getFullYear();
  const mm = String(selectedDate.getMonth() + 1).padStart(2, '0');
  const dd = String(selectedDate.getDate()).padStart(2, '0');
  const dateInputValue = `${yyyy}-${mm}-${dd}`;

  const dayLabel = isToday
    ? t('schedule.timelineToday')
    : selectedDate.toLocaleDateString(i18n.language, { day: '2-digit', month: 'long', year: 'numeric' });

  return (
    <Block padding="p-5" className="flex flex-col h-full min-h-[440px]">
      <div className="flex items-center justify-between mb-3 gap-2">
        <div className="text-xs uppercase tracking-wider text-[var(--text-muted)] truncate">
          {dayLabel} <span className="opacity-70">(24{t('schedule.hourShort','ч')})</span>
        </div>
        <div className="flex items-center gap-1 flex-shrink-0">
          {!isToday && (
            <button onClick={goToday}
              className="px-2 py-1 text-[11px] text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded hover:bg-[var(--surface-hover)] transition-colors"
              title={t('schedule.goToday','Сегодня')}>
              {t('schedule.goToday','Сегодня')}
            </button>
          )}
          <button onClick={() => shiftDay(-1)}
            className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded hover:bg-[var(--surface-hover)] transition-colors"
            title={t('schedule.prevDay','Предыдущий день')}>
            <ChevronLeft size={14} />
          </button>
          <button onClick={() => datePickerRef.current?.showPicker?.() ?? datePickerRef.current?.click()}
            className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded hover:bg-[var(--surface-hover)] transition-colors relative"
            title={t('schedule.pickDate','Выбрать дату')}>
            <Calendar size={14} />
            <input ref={datePickerRef} type="date" value={dateInputValue} onChange={onPickDate}
              className="absolute inset-0 opacity-0 cursor-pointer" />
          </button>
          <button onClick={() => shiftDay(1)}
            className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded hover:bg-[var(--surface-hover)] transition-colors"
            title={t('schedule.nextDay','Следующий день')}>
            <ChevronRight size={14} />
          </button>
        </div>
      </div>
      <div className="relative flex-1 min-h-[400px]">
        {/* Hour gridlines: label (40px) + dashed rule */}
        {Array.from({ length: 25 }).map((_, h) => (
          <div key={h} className="absolute left-0 right-0 flex items-center" style={{ top: `${(h/24)*100}%`, transform: 'translateY(-50%)' }}>
            <span className="w-10 text-[10px] font-mono text-[var(--text-muted)] tabular-nums text-right pr-2">
              {String(h).padStart(2,'0')}:00
            </span>
            <div className="flex-1 border-t border-dashed border-[var(--border-subtle)] opacity-40" />
          </div>
        ))}
        {/* Vertical gutter line at the start of the gridline column */}
        <div className="absolute top-0 bottom-0 left-10 border-l border-[var(--border-subtle)]" />
        {/* Bands */}
        {bands.map((b, i) => {
          const isActive = isToday && b.entry_id === activeEntryId;
          return (
            <div key={i}
              className="absolute left-11 right-1 rounded px-2 py-1 text-xs overflow-hidden"
              style={{
                top: `${b.top}%`,
                height: `max(${b.height}%, 18px)`,
                background: isActive ? 'rgba(34,197,94,0.12)' : 'rgba(99,102,241,0.07)',
              }}
              title={`${b.entry_id} · ${new Date(b.starts_at*1000).toLocaleTimeString()}–${b.ends_at ? new Date(b.ends_at*1000).toLocaleTimeString() : '?'}`}>
              <span className="font-medium text-[var(--text-primary)] truncate block">{b.entry_id}</span>
            </div>
          );
        })}
        {/* "Now" indicator */}
        {now >= dayStart && now <= dayEnd && (
          <div className="absolute left-10 right-0 flex items-center pointer-events-none"
               style={{ top: `${pct(now)}%`, transform: 'translateY(-50%)' }}>
            <span className="w-2 h-2 -ml-1 rounded-full bg-[var(--danger)]" />
            <div className="flex-1 border-t border-[var(--danger)]" />
          </div>
        )}
      </div>
    </Block>
  );
}

function ScheduleTab({ channelId }: { channelId: number }) {
  const { t } = useTranslation();
  const toast = useToast();
  const qc = useQueryClient();
  const { data: entries = [], isLoading } = useSchedule(channelId);
  const { data: active } = useScheduleActive(channelId);
  const { data: upcoming = [] } = useScheduleUpcoming(channelId);
  const { mutateAsync: replaceScheduleAsync, isPending: saving } = useReplaceSchedule(channelId);
  const [editing, setEditing] = React.useState<ScheduleEntry | 'new' | null>(null);

  // SSE schedule_active → invalidate active + upcoming so the highlight tracks
  // backend changes without waiting for the 5s/30s polls.
  const { events: scheduleEvents } = useEventStream({ channelId, types: ['schedule_active'] });
  React.useEffect(() => {
    if (scheduleEvents.length === 0) return;
    qc.invalidateQueries({ queryKey: ['channels', channelId, 'schedule', 'active'] });
    qc.invalidateQueries({ queryKey: ['channels', channelId, 'schedule', 'upcoming'] });
  }, [scheduleEvents, qc, channelId]);

  if (isLoading) return <div className="h-32 bg-surface2 rounded-xl animate-pulse" />;

  const isActive = active?.mode === 'schedule';

  const handleSubmit = async (next: ScheduleEntry) => {
    const updated = editing === 'new'
      ? [...entries, next]
      : entries.map(e => (e.id === next.id ? next : e));
    try {
      await replaceScheduleAsync(updated);
      toast(t('schedule.saved'), 'success');
      setEditing(null);
    } catch (err: any) {
      toast(err?.detail || err?.error || String(err), 'danger');
    }
  };

  const handleRemove = async (entry: ScheduleEntry) => {
    if (!window.confirm(t('schedule.deleteConfirm', { id: entry.id }))) return;
    const updated = entries.filter(e => e.id !== entry.id);
    try {
      await replaceScheduleAsync(updated);
      toast(t('schedule.removed'), 'info');
    } catch (err: any) {
      toast(err?.detail || err?.error || String(err), 'danger');
    }
  };

  const hasSchedule = entries.length > 0 || upcoming.length > 0 || isActive;

  const upcomingBlock = (isActive || upcoming.length > 0) ? (
    <Block padding="p-5">
      {isActive && (() => {
        const activeEntry = entries.find(e => e.id === active!.entry_id);
        const r: any = activeEntry?.recurrence;
        const startLabel = r?.start_time ?? null;
        const endLabel   = active!.window_end_ns != null
          ? new Date(active!.window_end_ns / 1_000_000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
          : (r?.end_time ?? null);
        return (
          <div className="mb-3 pb-3 border-b border-[var(--border-subtle)]">
            <div className="text-xs uppercase tracking-wide text-[var(--text-muted)] mb-1">{t('schedule.active')}</div>
            <div className="flex items-center gap-2 text-sm">
              <span className="w-2 h-2 rounded-full bg-[var(--success)] animate-pulse" />
              <span className="font-semibold text-[var(--text-primary)]">{active!.entry_id ?? '—'}</span>
              {(startLabel || endLabel) && (
                <span className="text-[var(--text-muted)] tabular-nums">
                  {startLabel && <span>{startLabel}</span>}
                  {startLabel && endLabel && <span> – </span>}
                  {endLabel && <span>{endLabel}</span>}
                </span>
              )}
            </div>
          </div>
        );
      })()}
      {upcoming.length > 0 && (
        <div>
          <div className="text-xs uppercase tracking-wide text-[var(--text-muted)] mb-2 flex items-center gap-1.5">
            <Clock size={12} /> {t('schedule.upcoming')}
          </div>
          <div className="flex flex-col gap-1.5">
            {upcoming.slice(0, 5).map((u, i) => {
              const startsAtDate = new Date(u.starts_at * 1000);
              const endsAtDate   = u.ends_at != null ? new Date(u.ends_at * 1000) : null;
              const startsAtValid = Number.isFinite(u.starts_at) && u.starts_at > 0 && !isNaN(startsAtDate.getTime());
              const fmt = (d: Date) => d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
              return (
                <div key={i} className="flex items-center gap-3 text-sm">
                  <span className="text-[var(--text-primary)] font-medium flex-shrink-0">{u.entry_id}</span>
                  {startsAtValid ? (
                    <span className="text-[var(--text-muted)] tabular-nums flex-shrink-0">
                      {fmt(startsAtDate)}{endsAtDate ? ` – ${fmt(endsAtDate)}` : ''}
                    </span>
                  ) : (
                    <span className="inline-flex items-center gap-1.5 flex-shrink-0 text-[var(--warning)]"
                          title={t('schedule.invalidStartsAtHint', { id: channelId })}>
                      <AlertTriangle size={13} className="flex-shrink-0" />
                      <span>{t('schedule.invalidStartsAt')}</span>
                    </span>
                  )}
                  {u.priority != null && <span className="text-xs text-[var(--text-muted)]">p{u.priority}</span>}
                </div>
              );
            })}
          </div>
        </div>
      )}
    </Block>
  ) : null;

  return (
    <div className="flex flex-col gap-4 h-full">
      {upcomingBlock}
      <div className={hasSchedule
        ? 'grid grid-cols-1 lg:grid-cols-2 gap-4 items-stretch flex-1 min-h-0'
        : 'flex-1 min-h-0'}>
      <div className="flex flex-col gap-4 min-w-0 h-full">
        <div className="flex gap-2">
          <button onClick={() => setEditing('new')}
            className="px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
            {t('schedule.newEntry')}
          </button>
        </div>

        {entries.length === 0 ? (
          <EmptyState Icon={Calendar} title={t('schedule.emptyTitle')}
            description={t('schedule.emptyDesc')} />
        ) : (
          <Block padding="p-0" className="flex-1 overflow-y-auto">
            {entries.map((entry) => (
              <div key={entry.id} className={`flex items-center gap-3 px-5 py-3.5 border-b last:border-0 border-[var(--border-subtle)] ${
                entry.id === active?.entry_id ? 'bg-[var(--success)]/5' : ''
              }`}>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2 mb-1">
                    <span className="font-semibold text-[var(--text-primary)] text-sm">{entry.id}</span>
                    {entry.id === active?.entry_id && (
                      <span className="w-2 h-2 rounded-full bg-[var(--success)] animate-pulse" title={t('schedule.active')} />
                    )}
                    <span className="text-xs bg-[var(--accent)]/10 text-[var(--accent)] px-1.5 py-0.5 rounded">{entry.recurrence.kind}</span>
                    {entry.hard_switch && (
                      <span className="text-xs text-[var(--warning)]">hard-switch</span>
                    )}
                  </div>
                  <div className="text-xs text-[var(--text-muted)] flex gap-3 flex-wrap">
                    <span>priority: {entry.priority}</span>
                    <span>playlist: <code className="font-mono">{entry.playlist.length} item{entry.playlist.length === 1 ? '' : 's'}</code></span>
                    <span>{describeRecurrence(entry.recurrence)}</span>
                    <span>transition: <code className="font-mono">{entry.transition.type}/{entry.transition.mode}</code></span>
                  </div>
                </div>

                <div className="flex gap-1.5 flex-shrink-0">
                  <button
                    onClick={() => setEditing(entry)}
                    title={t('schedule.editEntry')}
                    className="p-1.5 text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                    <Pencil size={14} />
                  </button>
                  <button
                    onClick={() => handleRemove(entry)}
                    title={t('schedule.deleteEntry')}
                    className="p-1.5 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                    <Trash2 size={14} />
                  </button>
                </div>
              </div>
            ))}
          </Block>
        )}
      </div>

      {hasSchedule && (
        <div className="hidden lg:flex flex-col gap-4 h-full">
          {/* Invisible spacer aligns timeline top with entries list (under the «+ Новая запись» button) */}
          <div aria-hidden className="invisible flex gap-2">
            <button className="px-3 py-1.5 text-sm">{t('schedule.newEntry')}</button>
          </div>
          <div className="flex-1 min-h-0">
            <ScheduleTimeline entries={entries} activeEntryId={active?.entry_id} />
          </div>
        </div>
      )}
      </div>

      {editing !== null && (
        <ScheduleEntryModal
          initial={editing === 'new' ? undefined : editing}
          existingIds={entries.map(e => e.id)}
          onSubmit={handleSubmit}
          onCancel={() => setEditing(null)}
          submitting={saving}
        />
      )}
    </div>
  );
}
// Date input → wall-clock ns. Empty string returns undefined (param omitted).
function dateToNs(value: string, endOfDay = false): number | undefined {
  if (!value) return undefined;
  const d = new Date(value + (endOfDay ? 'T23:59:59.999' : 'T00:00:00.000'));
  return isNaN(d.getTime()) ? undefined : d.getTime() * 1_000_000;
}

const LOG_PAGE_SIZE = 100;

const STATUS_TONE: Record<string, 'ok' | 'warn' | 'danger' | 'muted'> = {
  completed:    'ok',
  skipped_user: 'muted',
  removed:      'warn',
  error:        'danger',
};

function LogTab({ channelId }: { channelId: number }) {
  const { t } = useTranslation();
  const toast = useToast();

  // Filters live in URL-free local state — channel page is leaf-only.
  const [from, setFrom]       = React.useState('');
  const [to,   setTo]         = React.useState('');
  const [statusF, setStatusF] = React.useState<'' | 'completed' | 'skipped_user' | 'removed' | 'error'>('');
  const [search,  setSearch]  = React.useState('');
  // Cursor pagination via backend's next_after_ns. Stack tracks history for "back".
  const [cursorStack, setCursorStack] = React.useState<number[]>([]);
  const cursor = cursorStack.length > 0 ? cursorStack[cursorStack.length - 1] : undefined;

  // Reset pagination when filters change.
  React.useEffect(() => { setCursorStack([]); }, [from, to, statusF]);

  const params = React.useMemo(() => ({
    limit:    LOG_PAGE_SIZE,
    from_ns:  dateToNs(from, false),
    to_ns:    dateToNs(to,   true),
    after_ns: cursor,
  }), [from, to, cursor]);

  const { data: status }     = usePlaybackLogStatus(channelId);
  const { data, isLoading }  = usePlaybackLog(channelId, params);
  const { mutate: purgeMutate, isPending: purgePending } = usePurgePlaybackLog(channelId);

  const [purgeOpen, setPurgeOpen]     = React.useState(false);
  const [purgePreset, setPurgePreset] = React.useState<'last7' | 'last30' | 'filter' | 'all'>('last7');

  // Server has no status filter / search — apply client-side over the page.
  const events: PlaybackLogEvent[] = React.useMemo(() => {
    let evs = data?.events ?? [];
    if (statusF) evs = evs.filter(e => e.status === statusF);
    if (search) {
      const q = search.toLowerCase();
      evs = evs.filter(e =>
        e.clip_path.toLowerCase().includes(q) ||
        (e.error_reason ?? '').toLowerCase().includes(q),
      );
    }
    return evs;
  }, [data, statusF, search]);

  const sink = status?.sink_type ?? 'none';

  // sink=none: backend explicitly returns empty results. Show explanation.
  if (sink === 'none') {
    return (
      <div className="w-full">
        <Block>
          <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-2">{t('log.title')}</h2>
          <p className="text-sm text-[var(--text-muted)]">{t('log.disabledBody')}</p>
        </Block>
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-4 w-full">
      {/* Sink status block */}
      <Block>
        <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-4">{t('log.sinkTitle')}</h2>
        <div className="grid grid-cols-[repeat(auto-fit,minmax(180px,1fr))] gap-4 text-sm">
          <Counter label={t('log.sinkType')}    value={sink.toUpperCase()} />
          <Counter label={t('log.queueDepth')}  value={String(status?.queue_depth ?? 0)} />
          <Counter
            label={t('log.dropped')}
            value={String(status?.dropped_count ?? 0)}
            tone={(status?.dropped_count ?? 0) > 0 ? 'warn' : undefined} />
          <Counter
            label={t('log.lastWrite')}
            value={status?.last_write_ns
              ? new Date(status.last_write_ns / 1e6).toLocaleString()
              : '—'} />
          {sink === 'file' && (
            <Counter label={t('log.filesCount')} value={String(status?.files_count ?? 0)} />
          )}
          {sink === 'db' && (
            <>
              <Counter label={t('log.filesCount')}     value={String(status?.files_count ?? 0)} />
              <Counter label={t('log.retentionDays')}  value={String(status?.retention_days ?? '—')} />
              <Counter
                label={t('log.schemaErrors')}
                value={String(status?.schema_errors ?? 0)}
                tone={(status?.schema_errors ?? 0) > 0 ? 'danger' : undefined} />
            </>
          )}
        </div>
      </Block>

      {/* Filters */}
      <Block>
        <div className="flex flex-wrap items-end gap-3">
          <label className="flex flex-col gap-1 text-xs text-[var(--text-muted)] uppercase tracking-wider">
            {t('log.from')}
            <input type="date" value={from} onChange={e => setFrom(e.target.value)}
              className="px-3 py-1.5 text-sm bg-canvas border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)]" />
          </label>
          <label className="flex flex-col gap-1 text-xs text-[var(--text-muted)] uppercase tracking-wider">
            {t('log.to')}
            <input type="date" value={to} onChange={e => setTo(e.target.value)}
              className="px-3 py-1.5 text-sm bg-canvas border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)]" />
          </label>
          <label className="flex flex-col gap-1 text-xs text-[var(--text-muted)] uppercase tracking-wider">
            {t('log.status')}
            <select value={statusF} onChange={e => setStatusF(e.target.value as typeof statusF)}
              className="px-3 py-1.5 text-sm bg-canvas border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)]">
              <option value="">{t('log.all')}</option>
              <option value="completed">{t('log.statusCompleted')}</option>
              <option value="skipped_user">{t('log.statusSkipped')}</option>
              <option value="removed">{t('log.statusRemoved')}</option>
              <option value="error">{t('log.statusError')}</option>
            </select>
          </label>
          <label className="flex flex-col gap-1 text-xs text-[var(--text-muted)] uppercase tracking-wider flex-1 min-w-[200px]">
            {t('log.search')}
            <input type="text" value={search} onChange={e => setSearch(e.target.value)}
              placeholder={t('log.searchPlaceholder')}
              className="px-3 py-1.5 text-sm bg-canvas border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)]" />
          </label>
          <button
            type="button"
            onClick={() => setPurgeOpen(true)}
            disabled={purgePending}
            className="inline-flex items-center gap-1.5 px-3 py-1.5 text-sm border border-[var(--danger)]/40 text-[var(--danger)] rounded-md hover:bg-[var(--danger)]/10 disabled:opacity-40 disabled:cursor-not-allowed">
            <Trash2 size={14} />
            {t('log.purge')}
          </button>
        </div>
      </Block>

      {/* Events table */}
      <Block padding="p-0">
        {isLoading ? (
          <div className="h-32 bg-surface2 rounded-xl animate-pulse" />
        ) : events.length === 0 ? (
          <div className="px-4 py-12 text-center text-sm text-[var(--text-muted)]">{t('log.noEvents')}</div>
        ) : (
          <table className="w-full text-sm">
            <thead><tr className="border-b border-[var(--border-subtle)]">
              {[t('log.colTime'), t('log.colStatus'), t('log.colPath'), t('log.colType'), t('log.colTransition'), t('log.colDuration'), t('log.colReason')]
                .map(h => <th key={h} className="px-4 py-2.5 text-left text-xs font-semibold uppercase tracking-wider text-[var(--text-muted)]">{h}</th>)}
            </tr></thead>
            <tbody>
              {events.map((row, i) => {
                const tone = STATUS_TONE[row.status] ?? 'muted';
                const toneClr = tone === 'ok'     ? 'text-[var(--success)]'
                              : tone === 'warn'   ? 'text-[var(--warning)]'
                              : tone === 'danger' ? 'text-[var(--danger)]'
                              :                     'text-[var(--text-muted)]';
                return (
                  <tr key={i} className="border-b last:border-0 border-[var(--border-subtle)]">
                    <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-muted)] tabular-nums whitespace-nowrap">
                      {new Date(row.started_at_ns / 1e6).toLocaleString()}
                    </td>
                    <td className={`px-4 py-2.5 font-mono text-xs ${toneClr}`}>{row.status}</td>
                    <td className="px-4 py-2.5 font-mono text-xs text-[var(--text-primary)] truncate max-w-xs">{row.clip_path}</td>
                    <td className="px-4 py-2.5 text-xs text-[var(--text-muted)]">{row.clip_type}</td>
                    <td className="px-4 py-2.5 text-xs text-[var(--text-muted)]">{row.transition_type}</td>
                    <td className="px-4 py-2.5 tabular-nums text-xs text-[var(--text-muted)]">{row.played_sec.toFixed(2)}s</td>
                    <td className="px-4 py-2.5 text-xs text-[var(--danger)] truncate max-w-xs">{row.error_reason || ''}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Block>

      {/* Pagination */}
      <div className="flex items-center justify-between text-xs text-[var(--text-muted)]">
        <span>{t('log.showing', { count: events.length })}</span>
        <div className="flex gap-2">
          <button
            disabled={cursorStack.length === 0}
            onClick={() => setCursorStack(s => s.slice(0, -1))}
            className="px-3 py-1.5 border border-[var(--border-subtle)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-40 disabled:cursor-not-allowed">
            {t('log.prev')}
          </button>
          <button
            disabled={data?.next_after_ns == null}
            onClick={() => data?.next_after_ns != null && setCursorStack(s => [...s, data.next_after_ns!])}
            className="px-3 py-1.5 border border-[var(--border-subtle)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-40 disabled:cursor-not-allowed">
            {t('log.next')}
          </button>
        </div>
      </div>

      {purgeOpen && (
        <PurgeLogModal
          open={purgeOpen}
          pending={purgePending}
          preset={purgePreset}
          onPresetChange={setPurgePreset}
          filterFrom={from}
          filterTo={to}
          onCancel={() => setPurgeOpen(false)}
          onConfirm={() => {
            // Translate preset → (from_ns, to_ns). Omitted bounds = open-ended on that side.
            // last7/last30: only to_ns (cut everything older than N days).
            // filter:       reuse the visible filter range.
            // all:          omit both — purges the whole channel log.
            const nowMs = Date.now();
            const dayMs = 86_400_000;
            let body: { from_ns?: number; to_ns?: number } = {};
            if (purgePreset === 'last7')       body = { to_ns: (nowMs - 7  * dayMs) * 1_000_000 };
            else if (purgePreset === 'last30') body = { to_ns: (nowMs - 30 * dayMs) * 1_000_000 };
            else if (purgePreset === 'filter') body = { from_ns: dateToNs(from, false), to_ns: dateToNs(to, true) };
            purgeMutate(body, {
              onSuccess: (res) => {
                toast(
                  t('log.purgeSuccess', { count: res.deleted_rows, files: res.removed_files }),
                  'success',
                );
                setPurgeOpen(false);
              },
              onError: () => toast(t('log.purgeError'), 'danger'),
            });
          }}
        />
      )}
    </div>
  );
}

function PurgeLogModal({
  open, pending, preset, onPresetChange, filterFrom, filterTo, onCancel, onConfirm,
}: {
  open: boolean;
  pending: boolean;
  preset: 'last7' | 'last30' | 'filter' | 'all';
  onPresetChange: (p: 'last7' | 'last30' | 'filter' | 'all') => void;
  filterFrom: string;
  filterTo:   string;
  onCancel: () => void;
  onConfirm: () => void;
}) {
  const { t } = useTranslation();
  useEscClose(onCancel, open && !pending);
  if (!open) return null;
  const filterLabel = (filterFrom || filterTo)
    ? t('log.purgePresetFilter', { from: filterFrom || '…', to: filterTo || '…' })
    : t('log.purgePresetFilterEmpty');
  const opts: Array<{ key: typeof preset; label: string; disabled?: boolean }> = [
    { key: 'last7',  label: t('log.purgePresetLast7') },
    { key: 'last30', label: t('log.purgePresetLast30') },
    { key: 'filter', label: filterLabel, disabled: !filterFrom && !filterTo },
    { key: 'all',    label: t('log.purgePresetAll') },
  ];
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div
        className="bg-surface border border-[var(--border-subtle)] rounded-xl shadow-2xl w-full max-w-md flex flex-col">
        <div className="flex items-center gap-2 p-5 border-b border-[var(--border-subtle)]">
          <Trash2 size={18} className="text-[var(--danger)]" />
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('log.purgeTitle')}</h2>
        </div>
        <div className="p-5 flex flex-col gap-3">
          <p className="text-sm text-[var(--text-primary)] leading-relaxed">{t('log.purgeBody')}</p>
          <div className="flex flex-col gap-2">
            {opts.map(o => (
              <label
                key={o.key}
                className={`flex items-center gap-2 px-3 py-2 rounded-md border text-sm transition-colors ${
                  preset === o.key
                    ? 'border-[var(--accent)] bg-[var(--accent)]/10 text-[var(--text-primary)]'
                    : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-surface2'
                } ${o.disabled ? 'opacity-50 cursor-not-allowed' : 'cursor-pointer'}`}>
                <input
                  type="radio"
                  name="purge-preset"
                  checked={preset === o.key}
                  disabled={o.disabled}
                  onChange={() => onPresetChange(o.key)}
                />
                {o.label}
              </label>
            ))}
          </div>
        </div>
        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button
            type="button"
            disabled={pending}
            onClick={onCancel}
            className="px-4 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors">
            {t('common.cancel')}
          </button>
          <button
            type="button"
            disabled={pending}
            onClick={onConfirm}
            className="px-4 py-2 text-sm bg-[var(--danger)] text-white rounded-md hover:bg-[var(--danger)]/85 disabled:opacity-50 transition-colors">
            {pending ? t('log.purgePending') : t('log.purgeConfirm')}
          </button>
        </div>
      </div>
    </div>
  );
}

function WatcherPlaceholder({ channelId }: { channelId: number }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { data, error, isLoading } = useWatcherStatus(channelId);
  const { mutate: rescan, isPending } = useRescan(channelId);

  if (isLoading) return <div className="h-32 bg-surface2 rounded-xl animate-pulse max-w-xl" />;

  // 404 → no content_source configured. Show empty-state explaining it.
  const apiErr = error as { error?: string; status?: number } | null;
  if (!data || apiErr) {
    return (
      <div className="max-w-xl">
        <Block>
          <h2 className="text-lg font-semibold text-[var(--text-primary)] mb-2">{t('watcher.title')}</h2>
          <p className="text-sm text-[var(--text-muted)]">{t('watcher.disabledBody')}</p>
        </Block>
      </div>
    );
  }

  const errors = data.cache_copy_errors ?? 0;
  const unreachable = data.share_unreachable_count ?? 0;
  const pending = data.pending_deletes ?? 0;
  const oversized = data.oversized_skipped ?? 0;
  const hasIssue = errors > 0 || unreachable > 0 || !data.running;

  return (
    <div className="max-w-xl flex flex-col gap-4">
      {hasIssue && (
        <div className="flex items-start gap-3 p-3 bg-[var(--warning)]/10 border border-[var(--warning)]/30 rounded-lg text-sm">
          <AlertTriangle size={15} className="text-[var(--warning)] mt-0.5 flex-shrink-0" />
          <div className="flex-1">
            <p className="font-medium text-[var(--text-primary)] mb-0.5">{t('watcher.issuesDetected')}</p>
            <ul className="text-xs text-[var(--text-muted)] space-y-0.5">
              {!data.running && <li>{t('watcher.notRunning')}</li>}
              {unreachable > 0 && <li>{t('watcher.shareUnreachable', { count: unreachable })}</li>}
              {errors > 0 && <li>{t('watcher.copyErrors', { count: errors })}</li>}
            </ul>
          </div>
        </div>
      )}

      <Block>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('watcher.title')}</h2>
          <button disabled={isPending} onClick={() => rescan(undefined, { onSuccess: () => toast(t('watcher.rescanned'), 'success') })}
            className="px-3 py-1.5 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-50 transition-colors">
            {isPending ? t('watcher.scanning') : t('watcher.rescan')}
          </button>
        </div>
        <dl className="grid grid-cols-2 gap-4 text-sm">
          {[
            [t('watcher.mode'),         data.mode],
            [t('watcher.source'),       data.source_path],
            [t('watcher.cache'),        data.cache_path ?? '—'],
            [t('watcher.cachedFiles'),  data.cache_files_count ?? '—'],
            [t('watcher.cacheSize'),    data.cache_size_bytes != null ? fmtBytes(data.cache_size_bytes) : '—'],
            [t('watcher.scanInterval'), `${data.scan_interval_ms} ms`],
            [t('watcher.backoff'),      `${data.current_backoff_ms} ms`],
            [t('watcher.lastOk'),       fmtScanAgo(data.last_share_ok_ns)],
            [t('watcher.running'),      data.running ? t('common.yes') : t('common.no')],
            [t('watcher.numa'),         `Node ${data.numa_node}`],
          ].map(([k, v]) => (
            <div key={String(k)}>
              <dt className="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">{k}</dt>
              <dd className="text-[var(--text-primary)] break-all">{String(v ?? '—')}</dd>
            </div>
          ))}
        </dl>

        {(pending > 0 || oversized > 0) && (
          <div className="mt-4 pt-4 border-t border-[var(--border-subtle)] flex gap-3 flex-wrap text-xs">
            {pending > 0 && (
              <span className="px-2 py-1 bg-[var(--accent)]/10 text-[var(--accent)] rounded">
                {t('watcher.pendingDeletes', { count: pending })}
              </span>
            )}
            {oversized > 0 && (
              <span className="px-2 py-1 bg-[var(--warning)]/10 text-[var(--warning)] rounded">
                {t('watcher.oversizedSkipped', { count: oversized })}
              </span>
            )}
          </div>
        )}
      </Block>
    </div>
  );
}
function PermissionsPlaceholder({ channelId }: { channelId: number }) {
  const { t } = useTranslation();
  const toast = useToast();
  const { data: items = [], isLoading } = useChannelPermissions(channelId);
  const { data: users = [] } = useUsers();
  const qc = useQueryClient();
  const invalidatePerms = () =>
    qc.invalidateQueries({ queryKey: ['channels', channelId, 'permissions'] });
  const [adding, setAdding] = React.useState(false);
  const [newUserId, setNewUserId] = React.useState<number | ''>('');
  const [newPerm, setNewPerm] = React.useState<'view' | 'operate'>('view');
  const [editingUserId, setEditingUserId] = React.useState<number | null>(null);
  const [editingPerm, setEditingPerm] = React.useState<'view' | 'operate'>('view');

  const grantedIds = new Set(items.map(i => i.user_id));
  // Only non-admin, non-already-granted users can be picked. Admins have implicit access.
  const candidates = users.filter(u => u.role !== 'admin' && !grantedIds.has(u.id));

  const handleAdd = async () => {
    if (newUserId === '') return;
    try {
      await api.put(`/api/auth/users/${newUserId}/channels/${channelId}`, { permission: newPerm });
      toast(t('permissions.added'), 'success');
      setAdding(false); setNewUserId(''); setNewPerm('view');
      await invalidatePerms();
    } catch (e: unknown) {
      toast((e as { detail?: string })?.detail ?? 'Error', 'danger');
    }
  };

  const handleSaveEdit = async (userId: number) => {
    try {
      await api.put(`/api/auth/users/${userId}/channels/${channelId}`, { permission: editingPerm });
      toast(t('permissions.updated'), 'success');
      setEditingUserId(null);
      await invalidatePerms();
    } catch (e: unknown) {
      toast((e as { detail?: string })?.detail ?? 'Error', 'danger');
    }
  };

  const handleRemove = async (userId: number, username: string) => {
    if (!window.confirm(t('permissions.removeConfirm', { user: username }))) return;
    try {
      await api.delete(`/api/auth/users/${userId}/channels/${channelId}`);
      toast(t('permissions.removed'), 'info');
      await invalidatePerms();
    } catch (e: unknown) {
      toast((e as { detail?: string })?.detail ?? 'Error', 'danger');
    }
  };

  if (isLoading) return <div className="h-32 bg-surface2 rounded-xl animate-pulse max-w-xl" />;

  const selectCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-2 py-1 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]';

  return (
    <div className="max-w-xl">
      <Block>
        <div className="flex items-center justify-between mb-4">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('permissions.title')}</h2>
          {!adding && (
            <button onClick={() => setAdding(true)}
              className="px-3 py-1.5 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md transition-colors">
              + {t('permissions.addGrant')}
            </button>
          )}
        </div>

        <p className="text-xs text-[var(--text-muted)] mb-3">{t('permissions.adminsHaveAccess')}</p>

        {adding && (
          <div className="flex items-center gap-2 mb-4 p-3 bg-canvas rounded-lg border border-[var(--border-subtle)]">
            <select value={newUserId} onChange={e => setNewUserId(e.target.value === '' ? '' : Number(e.target.value))}
              className={`${selectCls} flex-1`}>
              <option value="">— {t('permissions.selectUser')} —</option>
              {candidates.map(u => <option key={u.id} value={u.id}>{u.username} ({u.role})</option>)}
            </select>
            <select value={newPerm} onChange={e => setNewPerm(e.target.value as 'view' | 'operate')}
              className={selectCls}>
              <option value="view">view</option>
              <option value="operate">operate</option>
            </select>
            <button onClick={handleAdd} disabled={newUserId === ''}
              className="px-3 py-1 text-sm bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded-md disabled:opacity-40 transition-colors">
              {t('common.add')}
            </button>
            <button onClick={() => { setAdding(false); setNewUserId(''); }}
              className="px-3 py-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
              {t('common.cancel')}
            </button>
          </div>
        )}

        {items.length === 0 && !adding ? (
          <p className="text-sm text-[var(--text-muted)]">{t('permissions.empty')}</p>
        ) : (
          <div className="flex flex-col">
            {items.map(row => (
              <div key={row.user_id} className="flex items-center gap-3 py-2 border-b last:border-0 border-[var(--border-subtle)]">
                <span className="text-sm font-medium text-[var(--text-primary)] flex-1">{row.username}</span>
                {editingUserId === row.user_id ? (
                  <>
                    <select value={editingPerm} onChange={e => setEditingPerm(e.target.value as 'view' | 'operate')}
                      className={selectCls}>
                      <option value="view">view</option>
                      <option value="operate">operate</option>
                    </select>
                    <button onClick={() => handleSaveEdit(row.user_id)}
                      className="px-2 py-1 text-xs bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white rounded transition-colors">
                      {t('common.save')}
                    </button>
                    <button onClick={() => setEditingUserId(null)}
                      className="px-2 py-1 text-xs text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                      {t('common.cancel')}
                    </button>
                  </>
                ) : (
                  <>
                    <span className="text-xs bg-[var(--accent)]/10 text-[var(--accent)] px-2 py-0.5 rounded-full">{row.permission}</span>
                    <button onClick={() => { setEditingUserId(row.user_id); setEditingPerm(row.permission); }}
                      title={t('common.edit')}
                      className="p-1 text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
                      <Pencil size={14} />
                    </button>
                    <button onClick={() => handleRemove(row.user_id, row.username)}
                      title={t('permissions.remove')}
                      className="p-1 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                      <Trash2 size={14} />
                    </button>
                  </>
                )}
              </div>
            ))}
          </div>
        )}
      </Block>
    </div>
  );
}

import { useWatcherStatus, useRescan, usePlaybackLog, usePlaybackLogStatus, usePurgePlaybackLog } from '@/api/queries/playlist';
import type { PlaybackLogEvent } from '@/api/queries/playlist';
import { useChannelPermissions } from '@/api/queries/channels';
