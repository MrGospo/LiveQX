import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import { clsx } from 'clsx';
import type { Player, PlayerEvent } from '@/api/queries/players';

// What the administrator reads in one word: not the raw "state" plus "online"
// flag, but where the screen stands in its life.
export type ScreenStatus = 'online' | 'offline' | 'pending' | 'revoked';

export function screenStatus(p: Pick<Player, 'state' | 'status'>): ScreenStatus {
  if (p.state === 'revoked') return 'revoked';
  if (p.state === 'pending') return 'pending';
  return p.status?.online ? 'online' : 'offline';
}

const STATUS_STYLE: Record<ScreenStatus, string> = {
  online:  'text-[var(--success)] border-[var(--success)] bg-[var(--success)]/10',
  offline: 'text-[var(--danger)] border-[var(--danger)] bg-[var(--danger)]/10',
  pending: 'text-[var(--warning)] border-[var(--warning)] bg-[var(--warning)]/10',
  revoked: 'text-[var(--text-muted)] border-[var(--border-subtle)] bg-surface2',
};

export function StatusBadge({ status }: { status: ScreenStatus }) {
  const { t } = useTranslation();
  return (
    <span className={clsx('inline-flex items-center gap-1.5 rounded-full border px-2.5 py-0.5 text-xs font-medium whitespace-nowrap', STATUS_STYLE[status])}>
      <span aria-hidden className="inline-block w-1.5 h-1.5 rounded-full bg-current" />
      {t(`players.status.${status}`)}
    </span>
  );
}

// `details` of an event is JSON text, or empty; anything unreadable is treated as empty.
function parseDetails(raw: string): Record<string, unknown> {
  if (!raw) return {};
  try {
    const v: unknown = JSON.parse(raw);
    return v && typeof v === 'object' && !Array.isArray(v) ? (v as Record<string, unknown>) : {};
  } catch {
    return {};
  }
}

const str = (v: unknown) => (typeof v === 'string' ? v : '');

export function commandTypeLabel(type: string, t: TFunction): string {
  return t(`players.commandTypes.${type}`, { defaultValue: type });
}

// One line of the journal in words. An event kind this build does not know is
// shown by its own name rather than hidden.
export function describeEvent(ev: PlayerEvent, t: TFunction): string {
  const d = parseDetails(ev.details);
  switch (ev.kind) {
    case 'online':
    case 'started':
      return t(`players.events.${ev.kind}`);
    case 'offline':
      return t('players.events.offline', {
        reason: t(`players.events.offlineReason.${str(d.reason)}`, { defaultValue: str(d.reason) }),
      });
    case 'version_changed':
      return t('players.events.version_changed', { from: str(d.from) || '—', to: str(d.to) || '—' });
    case 'command_sent':
    case 'command_executed':
    case 'command_failed':
    case 'command_timeout':
    case 'command_released':
      return t(`players.events.${ev.kind}`, {
        type: commandTypeLabel(str(d.type), t),
        result: t(`players.commandResults.${str(d.result)}`, { defaultValue: str(d.result) }),
      });
    default:
      return ev.kind;
  }
}
