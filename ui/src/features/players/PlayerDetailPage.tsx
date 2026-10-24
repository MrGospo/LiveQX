import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link, useParams } from 'react-router-dom';
import { ArrowLeft, Building2, AlertTriangle } from 'lucide-react';
import { clsx } from 'clsx';
import {
  usePlayer, usePlayerCommands, useReleaseCommand,
  type PlayerCertificate, type PlayerCommand, type PlayerReport,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { useAuthStore } from '@/stores/auth';
import { useToast } from '@/hooks/useToast';
import { Block } from '@/components/Block';
import { useNow } from '@/hooks/useNow';
import { PlayersSubNav } from './PlayersSubNav';
import { absoluteTime, formatDuration, relativeFromNow } from './format';
import { PlayerActions } from './PlayerActions';
import { StatusBadge, screenStatus, describeEvent, commandTypeLabel } from './status';
import { CertCell } from './cert';

// One screen: where it stands now, its certificates, the commands sent to it and
// its journal; the buttons above them (move, new token, revoke, delete) are
// PlayerActions, shown to those who may use them.

const CERT_SOON_DAYS = 30;           // the same edge as the first step in the list: a screen that is on has renewed by itself long before (at 60 days)

type CertState = 'revoked' | 'expired' | 'soon' | 'valid';

function certState(c: PlayerCertificate, nowSec: number): CertState {
  if (c.revoked_at) return 'revoked';
  if (c.not_after <= nowSec) return 'expired';
  return c.not_after - nowSec < CERT_SOON_DAYS * 86400 ? 'soon' : 'valid';
}

const CERT_STYLE: Record<CertState, string> = {
  revoked: 'text-[var(--text-muted)]',
  expired: 'text-[var(--danger)]',
  soon:    'text-[var(--warning)]',
  valid:   'text-[var(--success)]',
};

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex gap-4 py-1.5">
      <dt className="w-48 shrink-0 text-sm text-[var(--text-muted)]">{label}</dt>
      <dd className="m-0 text-sm text-[var(--text-primary)] min-w-0 break-words">{children}</dd>
    </div>
  );
}

function SectionTitle({ children }: { children: React.ReactNode }) {
  return <h2 className="text-base font-semibold text-[var(--text-primary)] m-0 mb-3">{children}</h2>;
}

const HEALTH_STYLE = {
  ok:       'text-[var(--success)]',
  warning:  'text-[var(--warning)]',
  critical: 'text-[var(--danger)]',
} as const;

function Report({ r }: { r: PlayerReport }) {
  const { t } = useTranslation();
  return (
    <>
      {r.health && (
        <Row label={t('players.detail.report.health')}>
          <span className={HEALTH_STYLE[r.health]}>{t(`players.detail.report.healthValue.${r.health}`)}</span>
        </Row>
      )}
      {r.watchdog_state && <Row label={t('players.detail.report.watchdog')}>{t(`players.detail.report.watchdogValue.${r.watchdog_state}`)}</Row>}
      {r.current_source && <Row label={t('players.detail.report.source')}><span className="font-mono text-xs">{r.current_source}</span></Row>}
      {r.media_status && <Row label={t('players.detail.report.media')}>{r.media_status}</Row>}
      {r.buffer_progress != null && <Row label={t('players.detail.report.buffer')}>{Math.round(r.buffer_progress * 100)} %</Row>}
      {r.uptime_seconds != null && <Row label={t('players.detail.report.uptime')}>{formatDuration(r.uptime_seconds, t)}</Row>}
      {r.os_uptime_seconds != null && <Row label={t('players.detail.report.osUptime')}>{formatDuration(r.os_uptime_seconds, t)}</Row>}
      {r.last_error && <Row label={t('players.detail.report.lastError')}><span className="text-[var(--danger)]">{r.last_error}</span></Row>}
      {r.last_renew_error && <Row label={t('players.detail.report.renewError')}><span className="text-[var(--danger)]">{r.last_renew_error}</span></Row>}
    </>
  );
}

function CommandsTable({ commands }: { commands: PlayerCommand[] }) {
  const { t } = useTranslation();
  const toast = useToast();
  const isAdmin = useAuthStore(s => s.role) === 'admin';
  const release = useReleaseCommand();
  const lift = async (c: PlayerCommand) => {
    try { await release.mutateAsync({ playerId: c.player_id, commandId: c.id }); toast(t('players.detail.commands.released'), 'success'); }
    catch { toast(t('players.detail.commands.releaseFailed'), 'danger'); }
  };
  return (
    <table className="w-full text-sm">
      <thead>
        <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
          <th className="font-medium py-2 pr-4">{t('players.detail.commands.col.type')}</th>
          <th className="font-medium py-2 pr-4">{t('players.detail.commands.col.state')}</th>
          <th className="font-medium py-2 pr-4">{t('players.detail.commands.col.sent')}</th>
          <th className="font-medium py-2">{t('players.detail.commands.col.finished')}</th>
          {isAdmin && <th className="font-medium py-2"><span className="sr-only">{t('players.detail.commands.col.action')}</span></th>}
        </tr>
      </thead>
      <tbody>
        {commands.map(c => (
          <tr key={c.id} className="border-b border-[var(--border-subtle)] last:border-b-0">
            <td className="py-2 pr-4">{commandTypeLabel(c.type, t)}</td>
            <td className="py-2 pr-4">
              {t(`players.detail.commands.state.${c.state}`, { defaultValue: c.state })}
              {c.result && c.state !== 'executed' && (
                <span className="text-xs text-[var(--text-muted)]"> · {t(`players.commandResults.${c.result}`, { defaultValue: c.result })}</span>
              )}
            </td>
            <td className="py-2 pr-4 text-[var(--text-muted)]">{absoluteTime(c.issued_at)}</td>
            <td className="py-2 text-[var(--text-muted)]">{c.finished_at ? absoluteTime(c.finished_at) : '—'}</td>
            {isAdmin && (
              <td className="py-2 text-right">
                {c.state === 'pending' && (
                  <button type="button" onClick={() => lift(c)} disabled={release.isPending}
                          title={t('players.detail.commands.releaseHint')}
                          className="px-2.5 py-1 text-xs rounded-md border border-[var(--border-subtle)] text-[var(--text-primary)] hover:border-[var(--warning)] disabled:opacity-40">
                    {t('players.detail.commands.release')}
                  </button>
                )}
              </td>
            )}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export default function PlayerDetailPage() {
  const { t, i18n } = useTranslation();
  const { id = '' } = useParams();
  const player = usePlayer(id);
  const commands = usePlayerCommands(id);
  const nowMs = useNow();

  if (player.isLoading) {
    return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  }

  const back = (
    <Link to="/players" className="inline-flex items-center gap-1.5 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] w-fit">
      <ArrowLeft size={14} /> {t('players.detail.back')}
    </Link>
  );

  if (player.isError || !player.data) {
    // The server answers "not found" both for a missing screen and for one outside the caller's rights.
    const notFound = player.error instanceof ApiClientError && player.error.status === 404;
    return (
      <div className="p-7 flex flex-col gap-5">
        <PlayersSubNav />
        {back}
        <p role="alert" className="text-sm text-[var(--danger)]">
          {notFound ? t('players.detail.notFound') : t('players.detail.loadFailed')}
        </p>
      </div>
    );
  }

  const p = player.data;
  const status = screenStatus(p);
  const nowSec = Math.floor(nowMs / 1000);
  const place = p.group_id == null ? null : [p.object_name, p.group_name].filter(Boolean).join(' / ');

  return (
    <div className="p-7 flex flex-col gap-5">
      <PlayersSubNav />
      {back}

      <div className="flex flex-wrap items-center gap-3">
        <h1 className="text-xl font-semibold text-[var(--text-primary)] m-0">{p.name}</h1>
        <StatusBadge status={status} />
      </div>

      <PlayerActions player={p} />

      {status === 'pending' && (
        <div role="status" className="flex gap-3 rounded-xl border border-[var(--warning)] bg-[var(--warning)]/10 p-4">
          <AlertTriangle size={18} className="text-[var(--warning)] shrink-0 mt-0.5" />
          <p className="text-sm text-[var(--text-primary)] m-0">{t('players.detail.pendingNote')}</p>
        </div>
      )}
      {status === 'revoked' && (
        <div role="status" className="flex gap-3 rounded-xl border border-[var(--border-subtle)] bg-surface2 p-4">
          <AlertTriangle size={18} className="text-[var(--text-muted)] shrink-0 mt-0.5" />
          <p className="text-sm text-[var(--text-primary)] m-0">
            {t('players.detail.revokedNote', { when: absoluteTime(p.revoked_at) })}
            {p.revoke_reason ? ` ${t('players.detail.revokedReason', { reason: t(`players.revokeReasons.${p.revoke_reason}`, { defaultValue: p.revoke_reason }) })}` : ''}
          </p>
        </div>
      )}

      {p.cert && p.cert.stage === 'expired' && (
        <div role="alert" className="flex gap-3 rounded-xl border border-[var(--danger)] bg-[var(--danger)]/10 p-4">
          <AlertTriangle size={18} className="text-[var(--danger)] shrink-0 mt-0.5" />
          <div className="text-sm text-[var(--text-primary)]">
            <p className="m-0 font-medium">{t('players.cert.callout.expiredTitle')}</p>
            <p className="m-0 mt-1">{t('players.cert.callout.expiredText')}</p>
          </div>
        </div>
      )}
      {p.cert && p.cert.stage !== 'ok' && p.cert.stage !== 'expired' && (
        <div role="status" className="flex gap-3 rounded-xl border border-[var(--warning)] bg-[var(--warning)]/10 p-4">
          <AlertTriangle size={18} className="text-[var(--warning)] shrink-0 mt-0.5" />
          <div className="text-sm text-[var(--text-primary)]">
            <p className="m-0 font-medium">
              {t('players.cert.callout.soonTitle', { when: p.cert.not_after ? absoluteTime(p.cert.not_after) : '—' })}
              {' '}<CertCell cert={p.cert} />
            </p>
            <p className="m-0 mt-1">{t(p.status?.online ? 'players.cert.callout.soonOnline' : 'players.cert.callout.soonOffline')}</p>
          </div>
        </div>
      )}

      <Block>
        <SectionTitle>{t('players.detail.now')}</SectionTitle>
        {status === 'offline' && p.status && (
          <p className="text-xs text-[var(--text-muted)] -mt-2 mb-3">{t('players.detail.staleNote')}</p>
        )}
        <dl className="m-0">
          <Row label={t('players.detail.place')}>
            {place ? <span className="inline-flex items-center gap-1.5"><Building2 size={14} className="text-[var(--text-muted)]" />{place}</span>
                   : <span className="text-[var(--text-muted)]">{t('players.list.noPlace')}</span>}
          </Row>
          <Row label={t('players.detail.version')}><span className="font-mono text-xs">{p.status?.version || '—'}</span></Row>
          <Row label={t('players.detail.lastSignal')}>
            {p.status
              ? <>{relativeFromNow(p.status.updated_at, nowMs, i18n.language)} <span className="text-[var(--text-muted)]">({absoluteTime(p.status.updated_at)})</span></>
              : t('players.list.never')}
          </Row>
          {p.status?.state && <Report r={p.status.state} />}
          <Row label={t('players.detail.clockPolicy')}>{t(`players.detail.clockValue.${p.clock_policy}`, { defaultValue: p.clock_policy })}</Row>
          <Row label={t('players.detail.created')}>{absoluteTime(p.created_at)}</Row>
          {p.activated_at != null && <Row label={t('players.detail.activated')}>{absoluteTime(p.activated_at)}</Row>}
          <Row label={t('players.detail.id')}><span className="font-mono text-xs">{p.id}</span></Row>
        </dl>
      </Block>

      <Block>
        <SectionTitle>{t('players.detail.certs.title')}</SectionTitle>
        {p.certificates.length === 0 ? (
          <p className="text-sm text-[var(--text-muted)] m-0">{t('players.detail.certs.none')}</p>
        ) : (
          <table className="w-full text-sm">
            <thead>
              <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
                <th className="font-medium py-2 pr-4">{t('players.detail.certs.col.serial')}</th>
                <th className="font-medium py-2 pr-4">{t('players.detail.certs.col.issued')}</th>
                <th className="font-medium py-2 pr-4">{t('players.detail.certs.col.until')}</th>
                <th className="font-medium py-2">{t('players.detail.certs.col.state')}</th>
              </tr>
            </thead>
            <tbody>
              {p.certificates.map(c => {
                const cs = certState(c, nowSec);
                return (
                  <tr key={c.serial} className="border-b border-[var(--border-subtle)] last:border-b-0">
                    <td className="py-2 pr-4 font-mono text-xs">{c.serial}</td>
                    <td className="py-2 pr-4 text-[var(--text-muted)]">{absoluteTime(c.issued_at)}</td>
                    <td className="py-2 pr-4 text-[var(--text-muted)]">{absoluteTime(c.not_after)}</td>
                    <td className={clsx('py-2', CERT_STYLE[cs])}>
                      {t(`players.detail.certs.state.${cs}`)}
                      {cs === 'revoked' && c.revoke_reason && (
                        <span className="text-xs text-[var(--text-muted)]"> · {t(`players.revokeReasons.${c.revoke_reason}`, { defaultValue: c.revoke_reason })}</span>
                      )}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </Block>

      <Block>
        <SectionTitle>{t('players.detail.commands.title')}</SectionTitle>
        {commands.isLoading ? <div className="h-16 bg-surface2 rounded-md animate-pulse" />
          : commands.isError ? <p role="alert" className="text-sm text-[var(--danger)] m-0">{t('players.detail.commands.loadFailed')}</p>
          : (commands.data ?? []).length === 0 ? <p className="text-sm text-[var(--text-muted)] m-0">{t('players.detail.commands.none')}</p>
          : <CommandsTable commands={commands.data ?? []} />}
      </Block>

      <Block>
        <SectionTitle>{t('players.detail.events.title')}</SectionTitle>
        {p.events.length === 0 ? (
          <p className="text-sm text-[var(--text-muted)] m-0">{t('players.detail.events.none')}</p>
        ) : (
          <ul className="list-none m-0 p-0 flex flex-col">
            {p.events.map(e => (
              <li key={e.id} className="flex gap-4 py-1.5 border-b border-[var(--border-subtle)] last:border-b-0 text-sm">
                <time className="w-44 shrink-0 text-[var(--text-muted)]" dateTime={new Date(e.ts * 1000).toISOString()}>{absoluteTime(e.ts)}</time>
                <span className="text-[var(--text-primary)]">{describeEvent(e, t)}</span>
              </li>
            ))}
          </ul>
        )}
      </Block>
    </div>
  );
}
