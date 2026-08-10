import React from 'react';
import { useParams, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { X, Volume2, VolumeX, RotateCcw, AlertTriangle, Wifi } from 'lucide-react';
import { useWebRTCPreview } from '@/hooks/useWebRTCPreview';

export default function ChannelPreviewPage() {
  const { id } = useParams<{ id: string }>();
  const { t } = useTranslation();
  const navigate = useNavigate();
  const channelId = Number(id);
  const [muted, setMuted] = React.useState(false);

  const { videoRef, state, stats, retry } = useWebRTCPreview(channelId);

  // Close on Escape
  React.useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') navigate(`/channels/${channelId}`);
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [channelId, navigate]);

  // Apply mute to video element
  React.useEffect(() => {
    if (videoRef.current) videoRef.current.muted = muted;
  }, [muted, videoRef]);

  const isError = state === 'failed' || state === 'unsupported';

  return (
    <div className="fixed inset-0 z-50 bg-black flex flex-col">
      {/* Video */}
      <video
        ref={videoRef}
        autoPlay
        playsInline
        muted={muted}
        className="flex-1 w-full h-full object-contain"
      />

      {/* Loading overlay */}
      {state === 'connecting' && (
        <div className="absolute inset-0 flex flex-col items-center justify-center gap-3 text-white bg-black/70">
          <Wifi size={40} className="animate-pulse text-[var(--accent)]" />
          <p className="text-lg font-medium">{t('preview.connecting')}</p>
        </div>
      )}

      {/* Error overlay */}
      {isError && (
        <div className="absolute inset-0 flex flex-col items-center justify-center gap-4 text-white bg-black/80">
          <AlertTriangle size={48} className="text-[var(--warning)]" />
          <p className="text-xl font-semibold">
            {state === 'unsupported' ? t('preview.unsupported') : t('preview.failed')}
          </p>
          <p className="text-sm text-gray-400 max-w-sm text-center">
            {state === 'unsupported'
              ? t('preview.unsupportedHint')
              : t('preview.failedHint')}
          </p>
          {state === 'failed' && (
            <button onClick={retry}
              className="flex items-center gap-2 px-4 py-2 bg-[var(--accent)] text-white rounded-md text-sm hover:bg-[var(--accent-hover)] transition-colors">
              <RotateCcw size={14} /> {t('common.retry')}
            </button>
          )}
        </div>
      )}

      {/* Stats overlay (top-left) */}
      {state === 'playing' && (stats.fps != null || stats.kbps != null) && (
        <div className="absolute top-3 left-3 bg-black/60 text-white text-xs font-mono px-2.5 py-1.5 rounded-lg flex gap-3">
          {stats.fps != null && <span>{stats.fps.toFixed(1)} fps</span>}
          {stats.kbps != null && <span>{stats.kbps.toFixed(0)} kbps</span>}
          {stats.rttMs != null && <span>RTT {stats.rttMs.toFixed(0)}ms</span>}
        </div>
      )}

      {/* Controls overlay (bottom) */}
      <div className="absolute bottom-0 left-0 right-0 flex items-center justify-between px-5 py-3 bg-gradient-to-t from-black/80 to-transparent">
        {/* Channel name */}
        <span className="text-white font-semibold text-sm">ch{channelId}</span>

        <div className="flex items-center gap-3">
          {/* Mute toggle */}
          <button
            onClick={() => setMuted(m => !m)}
            aria-label={muted ? t('preview.unmute') : t('preview.mute')}
            className="p-2 rounded-lg bg-white/10 hover:bg-white/20 text-white transition-colors">
            {muted ? <VolumeX size={18} /> : <Volume2 size={18} />}
          </button>

          {/* ESC hint */}
          <span className="text-white/50 text-xs hidden sm:block">ESC {t('preview.exitHint')}</span>

          {/* Close button */}
          <button
            onClick={() => navigate(`/channels/${channelId}`)}
            aria-label={t('preview.exit')}
            className="p-2 rounded-lg bg-white/10 hover:bg-white/20 text-white transition-colors">
            <X size={18} />
          </button>
        </div>
      </div>
    </div>
  );
}
