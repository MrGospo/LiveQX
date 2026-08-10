/**
 * useWebRTCPreview — per-channel WebRTC low-latency preview.
 *
 * Flow:
 *  1. Create RTCPeerConnection, generate SDP offer.
 *  2. POST /api/channels/{id}/preview/offer → get SDP answer.
 *  3. setRemoteDescription(answer) → ICE → video playing.
 *
 * Cleanup: DELETE /api/channels/{id}/preview/{session_id} on unmount.
 */
import React from 'react';
import { useAuthStore } from '@/stores/auth';

export type PreviewState = 'idle' | 'connecting' | 'playing' | 'failed' | 'unsupported';

interface PreviewStats {
  fps?: number;
  kbps?: number;
  rttMs?: number;
  jitter?: number;
}

interface UseWebRTCPreviewResult {
  videoRef: React.RefObject<HTMLVideoElement>;
  state: PreviewState;
  stats: PreviewStats;
  retry: () => void;
}

// Пустой массив = только host-кандидаты, прямой LAN-путь между браузером
// и LiveQX. STUN сам NAT-traversal не решает, а добавляет мёртвую
// srflx-пару, на которую ICE падает обратно если host не сошёлся, и тогда
// RTP теряется в NAT hairpin (см. fix45). Для remote-оператора через
// интернет оператор должен сам передать TURN через `ice_servers` поле
// offer-запроса, либо админ задаёт его в `preview.ice_servers` сервера.
const ICE_SERVERS: RTCIceServer[] = [];

export function useWebRTCPreview(channelId: number): UseWebRTCPreviewResult {
  const videoRef = React.useRef<HTMLVideoElement>(null);
  const [state, setState] = React.useState<PreviewState>('idle');
  const [stats, setStats] = React.useState<PreviewStats>({});
  const pcRef = React.useRef<RTCPeerConnection | null>(null);
  const sessionRef = React.useRef<string | null>(null);
  const statsTimer = React.useRef<ReturnType<typeof setInterval> | null>(null);
  const token = useAuthStore((s) => s.accessToken);

  const cleanup = React.useCallback(() => {
    if (statsTimer.current) clearInterval(statsTimer.current);
    if (sessionRef.current) {
      // Fire-and-forget DELETE
      fetch(`/api/channels/${channelId}/preview/${sessionRef.current}`, {
        method: 'DELETE',
        headers: { Authorization: `Bearer ${token ?? ''}` },
      }).catch(() => {});
      sessionRef.current = null;
    }
    pcRef.current?.close();
    pcRef.current = null;
  }, [channelId, token]);

  const start = React.useCallback(async () => {
    if (!('RTCPeerConnection' in window)) {
      setState('unsupported');
      return;
    }

    setState('connecting');
    cleanup();

    try {
      const pc = new RTCPeerConnection({ iceServers: ICE_SERVERS });
      pcRef.current = pc;

      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.addTransceiver('audio', { direction: 'recvonly' });

      pc.ontrack = (ev) => {
        if (videoRef.current && ev.streams[0]) {
          videoRef.current.srcObject = ev.streams[0];
          setState('playing');
        }
      };

      pc.oniceconnectionstatechange = () => {
        if (pc.iceConnectionState === 'failed') setState('failed');
        if (pc.iceConnectionState === 'disconnected') setState('failed');
      };

      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);

      const res = await fetch(`/api/channels/${channelId}/preview/offer`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Authorization: `Bearer ${token ?? ''}`,
        },
        body: JSON.stringify({ sdp: offer.sdp, type: offer.type }),
      });

      if (!res.ok) {
        // Q8: handle all documented error codes
        // 400 bad_offer, 404 not found, 429 too_many_clients,
        // 501 preview_not_compiled, 503 preview_not_configured
        const err = await res.json().catch(() => ({ error: 'unknown' }));
        const code: string = (err as { error?: string }).error ?? '';
        if (
          res.status === 501 ||
          res.status === 503 ||
          code === 'feature_disabled' ||
          code === 'preview_not_compiled' ||
          code === 'preview_not_configured'
        ) {
          setState('unsupported');
        } else if (res.status === 429 || code === 'too_many_clients') {
          // Show as failed with a distinct note in state — use 'failed' + consumers check error
          console.warn('[WebRTC] too_many_clients for channel', channelId);
          setState('failed');
        } else {
          setState('failed');
        }
        return;
      }

      const answer = await res.json();
      sessionRef.current = answer.session_id ?? null;
      await pc.setRemoteDescription(new RTCSessionDescription(answer));

      // Poll stats every 2s
      statsTimer.current = setInterval(async () => {
        try {
          const s = await fetch(`/api/channels/${channelId}/preview/stats`, {
            headers: { Authorization: `Bearer ${token ?? ''}` },
          }).then(r => r.json());
          setStats({ fps: s.fps, kbps: s.kbps });
        } catch { /* ignore */ }
      }, 2000);

    } catch (e) {
      console.error('[WebRTC]', e);
      setState('failed');
    }
  }, [channelId, token, cleanup]);

  React.useEffect(() => {
    if (channelId > 0) start();
    return cleanup;
  }, [channelId]); // eslint-disable-line

  return { videoRef, state, stats, retry: start };
}
