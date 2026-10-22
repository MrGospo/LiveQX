import type { TFunction } from 'i18next';

// "300 s" is easier to misjudge than "5 min": show the plain form beside the
// number the administrator types.
export function humanSeconds(sec: number, t: TFunction): string {
  if (!Number.isFinite(sec) || sec < 0) return '';
  if (sec >= 86400 && sec % 86400 === 0) return t('players.units.days',    { n: sec / 86400 });
  if (sec >= 3600  && sec % 3600  === 0) return t('players.units.hours',   { n: sec / 3600 });
  if (sec >= 60    && sec % 60    === 0) return t('players.units.minutes', { n: sec / 60 });
  return t('players.units.seconds', { n: sec });
}

// "3 minutes ago" / "in 2 days", in the interface language.
export function relativeFromNow(unixSec: number, nowMs: number, lang: string): string {
  const diff = unixSec * 1000 - nowMs;
  const abs = Math.abs(diff);
  const rtf = new Intl.RelativeTimeFormat(lang, { numeric: 'auto' });
  if (abs < 60_000)        return rtf.format(Math.round(diff / 1000), 'second');
  if (abs < 3_600_000)     return rtf.format(Math.round(diff / 60_000), 'minute');
  if (abs < 86_400_000)    return rtf.format(Math.round(diff / 3_600_000), 'hour');
  return rtf.format(Math.round(diff / 86_400_000), 'day');
}

export function absoluteTime(unixSec: number | null | undefined): string {
  return unixSec ? new Date(unixSec * 1000).toLocaleString() : '—';
}
