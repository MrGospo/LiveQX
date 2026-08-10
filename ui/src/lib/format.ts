import { format, formatDistanceToNow } from 'date-fns';
import { ru, enUS } from 'date-fns/locale';
import i18next from 'i18next';

function getLocale() {
  return i18next.language?.startsWith('ru') ? ru : enUS;
}

/** Unix timestamp (seconds) → human-readable relative time */
export function fmtTime(ts: number | null | undefined): string {
  if (!ts) return '—';
  return formatDistanceToNow(new Date(ts * 1000), {
    addSuffix: true,
    locale: getLocale(),
  });
}

/** Unix timestamp (seconds) → locale date+time string */
export function fmtDate(ts: number | null | undefined): string {
  if (!ts) return '—';
  return format(new Date(ts * 1000), 'dd.MM.yyyy HH:mm', { locale: getLocale() });
}

/** Unix timestamp (ms nanoseconds) → HH:mm:ss.SSS */
export function fmtNsTimestamp(tsNs: number): string {
  const d = new Date(tsNs / 1_000_000);
  return format(d, 'HH:mm:ss.SSS');
}

/** Bits per second → human-readable (kbps / Mbps) */
export function fmtBitrate(bps: number | null | undefined): string {
  if (!bps) return '0';
  if (bps < 1_000) return `${bps} bps`;
  if (bps < 1_000_000) return `${(bps / 1_000).toFixed(0)} kbps`;
  return `${(bps / 1_000_000).toFixed(2)} Mbps`;
}

/** Bytes → human-readable */
export function fmtBytes(bytes: number | null | undefined): string {
  if (!bytes) return '0 B';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 ** 3) return `${(bytes / 1024 ** 2).toFixed(1)} MB`;
  return `${(bytes / 1024 ** 3).toFixed(2)} GB`;
}

/** Seconds → MM:SS or HH:MM:SS */
export function fmtDuration(sec: number | null | undefined): string {
  if (sec == null) return '∞';
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (h > 0) return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  return `${m}:${String(s).padStart(2, '0')}`;
}

/** Uptime seconds → "3d 14h" style */
export function fmtUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}

/** Merge Tailwind classes (deduplication) */
export { clsx as cn } from 'clsx';
