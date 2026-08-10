/**
 * Schedule recurrence preview — UI-side only.
 *
 * Q4.1: shows up to N upcoming firings for the ScheduleTab.
 * Backend (Scheduler.cpp) is the source of truth — this is best-effort
 * preview based on the openapi Recurrence union (once|daily|weekly|monthly).
 *
 * For unsupported edge cases returns empty (entry just doesn't appear in the
 * upcoming list — backend still fires it).
 */

import type { ScheduleEntry } from '@/api/types';

export interface UpcomingFire {
  entryId: string;
  fireAt: number;        // unix seconds
  summary: string;       // human-readable recurrence + playlist
}

// "HH:MM" → minutes since 00:00 UTC; -1 if malformed.
function parseHHMM(s: string): number {
  const m = /^(\d{2}):(\d{2})$/.exec(s);
  if (!m) return -1;
  const h = Number(m[1]), mi = Number(m[2]);
  if (h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
  return h * 60 + mi;
}

// ISO8601 UTC ("YYYY-MM-DDTHH:MM:SSZ" or with offsets) → unix seconds; NaN if invalid.
function parseIso(s: string): number {
  const t = Date.parse(s);
  return Number.isNaN(t) ? NaN : Math.floor(t / 1000);
}

// Set time-of-day on a Date (UTC) and return unix seconds.
function dateAtUtc(year: number, monthIdx: number, day: number, hours: number, minutes: number): number {
  return Math.floor(Date.UTC(year, monthIdx, day, hours, minutes, 0) / 1000);
}

function describe(entry: ScheduleEntry): string {
  const r = entry.recurrence;
  switch (r.kind) {
    case 'once':    return `${entry.playlist} (once)`;
    case 'daily':   return `${entry.playlist} (daily ${r.start_time}-${r.end_time})`;
    case 'weekly':  return `${entry.playlist} (weekly ${r.start_time}-${r.end_time})`;
    case 'monthly': return `${entry.playlist} (monthly ${r.start_time}-${r.end_time})`;
  }
}

export function computeUpcoming(
  entries: ScheduleEntry[],
  limit = 5,
  now = Date.now(),
): UpcomingFire[] {
  const fires: UpcomingFire[] = [];
  const horizonDays = 30;
  const horizonMs = now + horizonDays * 86_400_000;

  for (const entry of entries) {
    const summary = describe(entry);
    const r = entry.recurrence;

    if (r.kind === 'once') {
      const start = parseIso(r.start_at);
      if (Number.isFinite(start) && start * 1000 >= now) {
        fires.push({ entryId: entry.id, fireAt: start, summary });
      }
      continue;
    }

    const startMin = parseHHMM(r.start_time);
    if (startMin < 0) continue;

    let added = 0;
    const cur = new Date(now);
    for (let i = 0; i < horizonDays && added < limit; i++) {
      const probe = new Date(Date.UTC(cur.getUTCFullYear(), cur.getUTCMonth(), cur.getUTCDate() + i));
      const dow = probe.getUTCDay();           // 0=Sun..6=Sat
      const dom = probe.getUTCDate();          // 1..31

      const matches =
        r.kind === 'daily'   ? true :
        r.kind === 'weekly'  ? r.days_of_week.includes(dow) :
                               r.days_of_month.includes(dom);
      if (!matches) continue;

      const fireAt = dateAtUtc(probe.getUTCFullYear(), probe.getUTCMonth(), probe.getUTCDate(),
                               Math.floor(startMin / 60), startMin % 60);
      if (fireAt * 1000 < now)      continue;
      if (fireAt * 1000 > horizonMs) break;
      fires.push({ entryId: entry.id, fireAt, summary });
      added++;
    }
  }

  return fires.sort((a, b) => a.fireAt - b.fireAt).slice(0, limit);
}
