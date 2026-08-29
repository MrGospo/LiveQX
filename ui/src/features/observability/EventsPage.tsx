/**
 * EventsPage — /observability/events
 * Full implementation per § 18.2 ui-spec.md
 * Uses useEventStreamHistory() to access the ring-buffer.
 */
import React from 'react';
import { useTranslation } from 'react-i18next';
import { Pause, Play, Trash2 } from 'lucide-react';
import { useEventStreamHistory } from '@/hooks/useEventStream';
import { Block } from '@/components/Block';
import { EmptyState } from '@/components/EmptyState';
import { SubNav } from '@/components/SubNav';
import { OBSERVABILITY_NAV } from './nav';
import { Activity } from 'lucide-react';
import type { SseEvent } from '@/api/types';
import { useAuthStore } from '@/stores/auth';
import { useEscClose } from '@/hooks/useEscClose';

const EVENT_TYPE_GROUPS = [
  'all', 'channel', 'output', 'schedule', 'gateway', 'plugin', 'auth', 'stress',
] as const;

// Fields that carry stream-level metadata rather than event payload.
// Stripped from the compact JSON preview so operators see only what the
// producer actually attached.
const META_KEYS = new Set(['type', 'ts', 'id', 'channel_id']);

function payloadPreview(ev: SseEvent): string {
  const rest: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(ev)) {
    if (!META_KEYS.has(k)) rest[k] = v;
  }
  return Object.keys(rest).length ? JSON.stringify(rest) : '';
}

// Render an auth_audit event's payload as `event · details` so operators
// see the action verb (channel.play, admin.user.created, …) without
// squinting at raw JSON. Falls back to generic preview if the shape is
// unexpected.
function auditPreview(ev: SseEvent): string {
  const action = typeof ev.event === 'string' ? ev.event : '';
  const details = (ev as { details?: unknown }).details;
  if (!action) return payloadPreview(ev);
  if (details === undefined || details === null) return action;
  return `${action} · ${typeof details === 'string' ? details : JSON.stringify(details)}`;
}

const EVENT_COLORS: Record<string, string> = {
  channel_state_change:  'text-[var(--accent)]',
  channel_health_change: 'text-[var(--info)]',
  clip_change:           'text-[var(--success)]',
  output_state_change:   'text-[var(--warning)]',
  schedule_active_change:'text-[var(--text-muted)]',
  auth_audit:            'text-[var(--danger)]',
  stress_run_started:    'text-[var(--warning)]',
  stress_run_finished:   'text-[var(--warning)]',
  plugin_install:        'text-[var(--info)]',
  plugin_uninstall:      'text-[var(--info)]',
  gateway_state_change:  'text-[var(--accent)]',
};

export default function EventsPage() {
  const { t } = useTranslation();
  const role = useAuthStore(s => s.role);

  const { events, connected, overflowed } = useEventStreamHistory();
  const [paused, setPaused] = React.useState(false);
  const [typeFilter, setTypeFilter] = React.useState<string>('all');
  const [search, setSearch] = React.useState('');
  const [frozenEvents, setFrozenEvents] = React.useState<SseEvent[]>([]);
  const [selectedEvent, setSelectedEvent] = React.useState<SseEvent | null>(null);

  useEscClose(() => setSelectedEvent(null), selectedEvent !== null);

  // Freeze buffer when paused
  React.useEffect(() => {
    if (!paused) setFrozenEvents(events);
  }, [events, paused]);

  const visible = paused ? frozenEvents : events;

  const filtered = visible.filter(ev => {
    if (typeFilter !== 'all' && !ev.type.startsWith(typeFilter.replace('auth', 'auth_'))) return false;
    // Admin-only: auth events
    if (ev.type === 'auth_audit' && role !== 'admin') return false;
    if (search && !JSON.stringify(ev).toLowerCase().includes(search.toLowerCase())) return false;
    return true;
  });

  const COLS = [
    { key: 'ts',      label: t('events.colTime'),                          w: 90  },
    { key: 'type',    label: t('events.colType'),                          w: 220 },
    { key: 'actor',   label: `${t('events.colActor')}/${t('events.colChannel')}`, w: 170 },
    { key: 'payload', label: t('events.colPayload')                              },
  ];

  return (
    <div className="p-7 flex flex-col gap-4" style={{ height: 'calc(100vh - 56px)' }}>
      <SubNav items={OBSERVABILITY_NAV} />
      {/* Toolbar */}
      <div className="flex items-center gap-3 flex-wrap flex-shrink-0">
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('observability.events')}</h1>

        {/* Live indicator */}
        <div className="flex items-center gap-1.5">
          <div className={`w-2 h-2 rounded-full transition-colors ${connected && !paused ? 'bg-[var(--success)] animate-pulse' : 'bg-[var(--text-muted)]'}`}
            style={connected && !paused ? { boxShadow: '0 0 5px var(--success)' } : undefined} />
          <span className="text-xs text-[var(--text-muted)]">
            {connected ? (paused ? t('events.statusPaused') : t('events.statusLive')) : t('events.statusOffline')}
            {' · '}{visible.length}/256
          </span>
        </div>

        <div className="flex-1" />

        <input value={search} onChange={e => setSearch(e.target.value)}
          placeholder={t('events.search')}
          className="px-3 py-1.5 bg-canvas border border-[var(--border-subtle)] rounded-md text-sm text-[var(--text-primary)] outline-none w-44 focus-visible:border-[var(--accent)]" />

        <button onClick={() => setPaused(p => !p)}
          className={`flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-md border transition-colors ${paused ? 'bg-[var(--accent)] text-white border-transparent' : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'}`}>
          {paused ? <Play size={13} /> : <Pause size={13} />}
          {paused ? t('events.resume') : t('events.pause')}
        </button>

        <button onClick={() => { if (!paused) setFrozenEvents([]); }}
          className="flex items-center gap-1.5 px-2.5 py-1.5 text-sm border border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--danger)] rounded-md transition-colors">
          <Trash2 size={13} />
        </button>
      </div>

      {/* Type filter pills */}
      <div className="flex gap-1.5 flex-wrap flex-shrink-0">
        {EVENT_TYPE_GROUPS.map(g => (
          <button key={g} onClick={() => setTypeFilter(g)}
            className={`px-2.5 py-0.5 rounded-full text-xs font-medium border transition-all ${typeFilter === g ? 'border-[var(--accent)] bg-[var(--accent)]/10 text-[var(--accent)]' : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'}`}>
            {t(`events.group_${g}`)}
          </button>
        ))}
      </div>

      {/* Overflow banner */}
      {overflowed && (
        <div className="flex-shrink-0 flex items-center gap-2 px-4 py-2.5 bg-[var(--warning)]/10 border border-[var(--warning)]/30 rounded-lg text-sm text-[var(--text-primary)]">
          ⚠ {t('events.overflow')}
        </div>
      )}

      {/* Table */}
      <Block padding="p-0" className="flex-1 overflow-hidden flex flex-col min-h-0">
        <div className="overflow-y-auto flex-1" aria-live="polite" aria-atomic="false">
          {filtered.length === 0 ? (
            <EmptyState Icon={Activity} title={t('events.empty')} description={t('events.emptyHint')} />
          ) : (
            <table className="w-full text-xs border-collapse">
              <thead className="sticky top-0 z-10 bg-surface border-b border-[var(--border-subtle)]">
                <tr>
                  {COLS.map(c => (
                    <th key={c.key} style={{ width: c.w }}
                      className="px-3 py-2 text-left font-semibold uppercase tracking-wider text-[var(--text-muted)]">
                      {c.label}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {filtered.map((ev, i) => {
                  const isAudit = ev.type === 'auth_audit';
                  const username = typeof ev.username === 'string' ? ev.username : '';
                  const ip       = typeof ev.ip === 'string' ? ev.ip : '';
                  const preview  = isAudit ? auditPreview(ev) : payloadPreview(ev);
                  // Time source: prefer ev.ts (backend unix-ms), fall back to
                  // event id (ring-buffer sequence — not a timestamp, but at
                  // least monotonic). New events always carry ts now that
                  // wire["ts"] is set by ControlApi's SSE encoder.
                  const timeMs = typeof ev.ts === 'number' ? ev.ts : Number(ev.id ?? 0);
                  return (
                    <tr key={`${ev.id}-${i}`}
                      onClick={() => setSelectedEvent(ev)}
                      className="border-b border-[var(--border-subtle)] hover:bg-surface2 cursor-pointer transition-colors">
                      <td className="px-3 py-2 tabular-nums text-[var(--text-muted)] whitespace-nowrap">
                        {new Date(timeMs).toLocaleTimeString('en', { hour12: false })}
                      </td>
                      <td className="px-3 py-2">
                        <span className={`font-mono ${EVENT_COLORS[ev.type] ?? 'text-[var(--text-muted)]'}`}>{ev.type}</span>
                      </td>
                      <td className="px-3 py-2 text-[var(--text-primary)] truncate">
                        {isAudit ? (
                          <span className="font-mono text-xs">
                            {username || '—'}
                            {ip && <span className="text-[var(--text-muted)]"> · {ip}</span>}
                          </span>
                        ) : (
                          <span className="tabular-nums text-[var(--accent)]">
                            {ev.channel_id ? `ch${ev.channel_id}` : ''}
                          </span>
                        )}
                      </td>
                      <td className="px-3 py-2 font-mono text-[var(--text-muted)] truncate max-w-xs">
                        {preview}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          )}
        </div>
      </Block>

      {/* Event detail drawer */}
      {selectedEvent && (
        <div className="fixed inset-0 z-50 flex items-end sm:items-center justify-center p-4 bg-black/50">
          <div
            className="w-full max-w-lg bg-surface border border-[var(--border-subtle)] rounded-xl p-6 flex flex-col gap-4">
            <div className="flex items-center justify-between">
              <span className={`font-mono text-sm font-semibold ${EVENT_COLORS[selectedEvent.type] ?? 'text-[var(--text-muted)]'}`}>{selectedEvent.type}</span>
              <button onClick={() => setSelectedEvent(null)} className="text-[var(--text-muted)] hover:text-[var(--text-primary)]">✕</button>
            </div>
            <pre className="font-mono text-xs text-[var(--text-muted)] bg-canvas rounded-lg p-4 overflow-auto max-h-64">
              {JSON.stringify(selectedEvent, null, 2)}
            </pre>
          </div>
        </div>
      )}
    </div>
  );
}
