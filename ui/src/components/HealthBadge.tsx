import { clsx } from 'clsx';
import type { ChannelState, OutputState, LiveInputState, HealthState } from '@/api/types';

type AnyStatus = ChannelState | OutputState | LiveInputState | HealthState | 'playing' | 'unknown';

const STATUS_CONFIG: Record<string, { color: string; bg: string; dot: string }> = {
  running:      { color: 'text-[var(--success)]', bg: 'bg-[var(--success)]/10', dot: 'bg-[var(--success)]' },
  playing:      { color: 'text-[var(--success)]', bg: 'bg-[var(--success)]/10', dot: 'bg-[var(--success)]' },
  ok:           { color: 'text-[var(--success)]', bg: 'bg-[var(--success)]/10', dot: 'bg-[var(--success)]' },
  live:         { color: 'text-[var(--success)]', bg: 'bg-[var(--success)]/10', dot: 'bg-[var(--success)]' },
  starting:     { color: 'text-[var(--accent)]',  bg: 'bg-[var(--accent)]/10',  dot: 'bg-[var(--accent)]' },
  connecting:   { color: 'text-[var(--accent)]',  bg: 'bg-[var(--accent)]/10',  dot: 'bg-[var(--accent)]' },
  degraded:     { color: 'text-[var(--warning)]', bg: 'bg-[var(--warning)]/10', dot: 'bg-[var(--warning)]' },
  stopping:     { color: 'text-[var(--warning)]', bg: 'bg-[var(--warning)]/10', dot: 'bg-[var(--warning)]' },
  pausing:      { color: 'text-[var(--warning)]', bg: 'bg-[var(--warning)]/10', dot: 'bg-[var(--warning)]' },
  stalled:      { color: 'text-[var(--warning)]', bg: 'bg-[var(--warning)]/10', dot: 'bg-[var(--warning)]' },
  reconnecting: { color: 'text-[var(--warning)]', bg: 'bg-[var(--warning)]/10', dot: 'bg-[var(--warning)]' },
  failed:       { color: 'text-[var(--danger)]',  bg: 'bg-[var(--danger)]/10',  dot: 'bg-[var(--danger)]' },
  fail:         { color: 'text-[var(--danger)]',  bg: 'bg-[var(--danger)]/10',  dot: 'bg-[var(--danger)]' },
  fatal:        { color: 'text-[var(--danger)]',  bg: 'bg-[var(--danger)]/10',  dot: 'bg-[var(--danger)]' },
  error:        { color: 'text-[var(--danger)]',  bg: 'bg-[var(--danger)]/10',  dot: 'bg-[var(--danger)]' },
  stopped:      { color: 'text-[var(--text-muted)]', bg: 'bg-[var(--border-subtle)]/30', dot: 'bg-[var(--text-muted)]' },
  paused:       { color: 'text-[var(--text-muted)]', bg: 'bg-[var(--border-subtle)]/30', dot: 'bg-[var(--text-muted)]' },
  idle:         { color: 'text-[var(--text-muted)]', bg: 'bg-[var(--border-subtle)]/30', dot: 'bg-[var(--text-muted)]' },
  disconnected: { color: 'text-[var(--text-muted)]', bg: 'bg-[var(--border-subtle)]/30', dot: 'bg-[var(--text-muted)]' },
};

const fallback = { color: 'text-[var(--text-muted)]', bg: 'bg-[var(--border-subtle)]/30', dot: 'bg-[var(--text-muted)]' };

interface Props {
  status: AnyStatus | string;
  small?: boolean;
  className?: string;
}

export function HealthBadge({ status, small, className }: Props) {
  const cfg = STATUS_CONFIG[status] ?? fallback;
  return (
    <span className={clsx(
      'inline-flex items-center gap-1.5 rounded-full font-medium whitespace-nowrap',
      cfg.color, cfg.bg,
      small ? 'text-xs px-2 py-0.5' : 'text-sm px-2.5 py-0.5',
      className,
    )}>
      <span className={clsx('rounded-full flex-shrink-0', cfg.dot, small ? 'w-1.5 h-1.5' : 'w-2 h-2')} />
      {status}
    </span>
  );
}

export function RoleBadge({ role }: { role: string }) {
  const colors: Record<string, string> = {
    admin:    'text-[var(--danger)] bg-[var(--danger)]/10',
    operator: 'text-[var(--accent)] bg-[var(--accent)]/10',
    viewer:   'text-[var(--text-muted)] bg-[var(--border-subtle)]/30',
  };
  return (
    <span className={clsx('text-xs font-semibold tracking-wide uppercase rounded-full px-2 py-0.5', colors[role] ?? colors.viewer)}>
      {role}
    </span>
  );
}

export function SourceBadge({ source }: { source: string }) {
  return (
    <span className={clsx(
      'text-xs font-medium rounded-full px-2 py-0.5',
      source === 'ldap'
        ? 'text-[var(--warning)] bg-[var(--warning)]/10'
        : 'text-[var(--text-muted)] bg-[var(--border-subtle)]/30',
    )}>
      {source}
    </span>
  );
}
