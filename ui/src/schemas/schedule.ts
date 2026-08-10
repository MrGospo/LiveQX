/**
 * Schedule action discriminated union (Q10).
 *
 * Formally specified in docs/SCHEDULE.md and src/schedule/ScheduleEntry.cpp.
 * openapi.yaml uses additionalProperties: true today; will get proper oneOf in fix25 c16.
 * Until then we define the union locally here.
 */
import { z } from 'zod';

// ─── Action variants ──────────────────────────────────────────────────────────

export const playlistOverrideActionSchema = z.object({
  type: z.literal('playlist_override'),
  /** Replaces the main playlist for the duration of the schedule entry. */
  items: z.array(z.record(z.unknown())).min(1, 'At least one item required'),
});

export const liveSwitchActionSchema = z.object({
  type: z.literal('live_switch'),
  /** Switch channel to a live input for the duration of the entry. */
  input: z.object({
    type: z.enum(['multicast', 'rtmp', 'rtsp', 'ndi']),
    url: z.string().optional(),
    source_name: z.string().optional(),
  }),
});

export const pauseActionSchema = z.object({
  type: z.literal('pause'),
  /** Mute channel (show fallback frame) for the duration of the entry. */
});

export const scheduleActionSchema = z.discriminatedUnion('type', [
  playlistOverrideActionSchema,
  liveSwitchActionSchema,
  pauseActionSchema,
]);

export type ScheduleAction =
  | z.infer<typeof playlistOverrideActionSchema>
  | z.infer<typeof liveSwitchActionSchema>
  | z.infer<typeof pauseActionSchema>;

// ─── Full entry schema (edit form) ───────────────────────────────────────────

export const scheduleEntryEditSchema = z.object({
  id: z.string().min(1, 'Required'),
  kind: z.enum(['once', 'daily', 'weekly', 'monthly']),
  priority: z.number().int().min(0).max(1000).default(500),
  hard_switch: z.boolean().default(false),
  effective_from: z.number().optional(),
  effective_to: z.number().optional(),
  // cron for daily/weekly/monthly, `at` (unix ts) for once
  cron: z.string().optional(),
  at: z.number().optional(),
  // Action — start with playlist_override support only; others use raw JSON
  action: scheduleActionSchema,
});

export type ScheduleEntryEditValues = z.infer<typeof scheduleEntryEditSchema>;

// ─── Display helper ───────────────────────────────────────────────────────────

/** Human-readable summary of a schedule action for table/list display */
export function describeAction(action: unknown): string {
  if (!action || typeof action !== 'object') return '—';
  const a = action as Record<string, unknown>;
  switch (a.type) {
    case 'playlist_override':
      return `playlist_override (${Array.isArray(a.items) ? a.items.length : '?'} items)`;
    case 'live_switch':
      return `live_switch (${(a.input as Record<string, unknown>)?.type ?? '?'})`;
    case 'pause':
      return 'pause';
    default:
      return String(a.type ?? '?');
  }
}
