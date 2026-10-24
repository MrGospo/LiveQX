import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { Radio, CheckCircle2, XCircle, RefreshCw, AlertTriangle, Loader2 } from 'lucide-react';
import { clsx } from 'clsx';
import {
  useBrokerStatus, useBrokerReconcile, useBrokerUpdate, usePlayersLifecycle,
  type BrokerState, type BrokerConnection, type BrokerConnectionPatch,
  type BrokerCheckResult, type BrokerApplyResult,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { Block } from '@/components/Block';
import { useNow } from '@/hooks/useNow';
import { useToast } from '@/hooks/useToast';
import { PlayersSubNav } from './PlayersSubNav';
import { TextField, NumberField, Toggle, SectionTitle } from './fields';
import { absoluteTime, relativeFromNow } from './format';
import { CERT_TONE, useDaysLeftText } from './cert';

// The MQTT broker: where the link stands, what it is missing, and how the server
// reaches the broker. Written for an administrator who is not an MQTT expert: each
// state says in words what it means for the screens, and each missing piece says
// what to do about it.

const STATE_STYLE: Record<BrokerState, string> = {
  connected:  'text-[var(--success)] border-[var(--success)] bg-[var(--success)]/10',
  connecting: 'text-[var(--warning)] border-[var(--warning)] bg-[var(--warning)]/10',
  waiting:    'text-[var(--warning)] border-[var(--warning)] bg-[var(--warning)]/10',
  disabled:   'text-[var(--text-muted)] border-[var(--border-subtle)] bg-surface2',
  stopped:    'text-[var(--text-muted)] border-[var(--border-subtle)] bg-surface2',
};

const HOST_RE = /^[A-Za-z0-9._:][A-Za-z0-9._:-]*$/;
const PREPARE_ERRORS = ['broker_certificate_missing'];
const RELOAD_ERRORS = ['pid_file_unreadable', 'bad_pid', 'process_not_running', 'not_the_broker', 'signal_failed', 'pid_file_not_configured'];
const SOON_SECONDS = 7 * 86400;                // a revocation list this close to its end is worth a warning

// The server's own certificates and when each ends. One that ends takes every screen down with it,
// so they are listed together here, in the place an administrator goes when the screens do not connect.
function ServerCertificates() {
  const { t } = useTranslation();
  const lifecycle = usePlayersLifecycle();
  const daysText = useDaysLeftText();
  const certs = lifecycle.data?.server ?? [];
  if (certs.length === 0) return null;
  return (
    <Block>
      <SectionTitle title={t('players.cert.serverTitle')} hint={t('players.cert.serverHint')} />
      <table className="w-full text-sm">
        <thead>
          <tr className="text-left text-xs text-[var(--text-muted)] border-b border-[var(--border-subtle)]">
            <th className="font-medium py-2 pr-4">{t('players.cert.serverCol.name')}</th>
            <th className="font-medium py-2 pr-4">{t('players.cert.serverCol.until')}</th>
            <th className="font-medium py-2">{t('players.cert.serverCol.left')}</th>
          </tr>
        </thead>
        <tbody>
          {certs.map(c => (
            <tr key={c.name} className="border-b border-[var(--border-subtle)] last:border-b-0">
              <td className="py-2 pr-4">{t(`players.cert.server.${c.name}`)}</td>
              <td className="py-2 pr-4 text-[var(--text-muted)]">{c.not_after ? absoluteTime(c.not_after) : '—'}</td>
              <td className={clsx('py-2', c.stage ? CERT_TONE[c.stage] : 'text-[var(--danger)]')}>
                {c.stage ? daysText(c.stage, c.days_left) : t(`players.cert.serverError.${c.error ?? 'unknown'}`, { defaultValue: c.error ?? '' })}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </Block>
  );
}

type Form = { host: string; port: number | ''; verify_hostname: boolean; reconcile_interval_s: number | '' };

const toForm = (c: BrokerConnection): Form => ({
  host: c.host, port: c.port, verify_hostname: c.verify_hostname, reconcile_interval_s: c.reconcile_interval_s,
});

function StateBadge({ state, label }: { state: BrokerState; label: string }) {
  return (
    <span className={clsx('inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-sm font-medium', STATE_STYLE[state])}>
      {state === 'connected' ? <CheckCircle2 size={14} /> : state === 'connecting' || state === 'waiting' ? <Loader2 size={14} className="animate-spin" /> : null}
      {label}
    </span>
  );
}

function Check({ ok, label, fix }: { ok: boolean; label: string; fix?: React.ReactNode }) {
  return (
    <li className="flex gap-3">
      {ok
        ? <CheckCircle2 size={18} className="text-[var(--success)] shrink-0 mt-0.5" aria-label="ok" />
        : <XCircle size={18} className="text-[var(--danger)] shrink-0 mt-0.5" aria-label="not ok" />}
      <div>
        <p className="text-sm text-[var(--text-primary)] m-0">{label}</p>
        {!ok && fix && <p className="text-xs text-[var(--text-muted)] mt-1 mb-0">{fix}</p>}
      </div>
    </li>
  );
}

export default function BrokerPage() {
  const { t, i18n } = useTranslation();
  const toast = useToast();
  const { data: st, isLoading, isError } = useBrokerStatus();
  const nowMs = useNow();
  const reconcile = useBrokerReconcile();
  const update = useBrokerUpdate();

  const [form, setForm] = React.useState<Form | null>(null);
  const [check, setCheck] = React.useState<{ ok: boolean; detail: string } | null>(null);
  const [probeFailure, setProbeFailure] = React.useState<string | null>(null);
  const [skipCheck, setSkipCheck] = React.useState(false);
  const [fieldErrors, setFieldErrors] = React.useState<Partial<Record<keyof Form, string>>>({});
  const [warning, setWarning] = React.useState<string | null>(null);

  // The form starts from what is in force and is not overwritten by the 5-second
  // refresh while someone is typing.
  const current: BrokerConnection | null = st && st.host !== undefined
    ? { host: st.host, port: st.port ?? 8883, verify_hostname: st.verify_hostname ?? true, reconcile_interval_s: st.reconcile_interval_s ?? 300 }
    : null;
  const currentKey = current ? JSON.stringify(current) : '';
  React.useEffect(() => { if (current && !form) setForm(toForm(current)); }, [currentKey, form]);   // eslint-disable-line react-hooks/exhaustive-deps

  const upd = <K extends keyof Form>(k: K, v: Form[K]) => {
    setForm(p => (p ? { ...p, [k]: v } : p));
    setCheck(null);
    setProbeFailure(null);
    setFieldErrors(p => { if (!(k in p)) return p; const n = { ...p }; delete n[k]; return n; });
  };

  const errors = React.useMemo(() => {
    const e: Partial<Record<keyof Form, string>> = {};
    if (!form) return e;
    const host = form.host.trim();
    if (!host || host.length > 253 || !HOST_RE.test(host)) e.host = t('players.broker.connection.errors.host');
    if (form.port === '' || form.port < 1 || form.port > 65535) e.port = t('players.broker.connection.errors.port');
    if (form.reconcile_interval_s === '' || form.reconcile_interval_s < 10 || form.reconcile_interval_s > 86400)
      e.reconcile_interval_s = t('players.broker.connection.errors.interval');
    return e;
  }, [form, t]);

  const patch = React.useMemo<BrokerConnectionPatch>(() => {
    const p: BrokerConnectionPatch = {};
    if (!form || !current) return p;
    if (form.host.trim() !== current.host) p.host = form.host.trim();
    if (form.port !== '' && form.port !== current.port) p.port = form.port;
    if (form.verify_hostname !== current.verify_hostname) p.verify_hostname = form.verify_hostname;
    if (form.reconcile_interval_s !== '' && form.reconcile_interval_s !== current.reconcile_interval_s)
      p.reconcile_interval_s = form.reconcile_interval_s;
    return p;
  }, [form, currentKey]);                      // eslint-disable-line react-hooks/exhaustive-deps

  const dirty = Object.keys(patch).length > 0;
  const hasErrors = Object.keys(errors).length > 0;
  const full = (): BrokerConnectionPatch => form && !hasErrors
    ? { host: form.host.trim(), port: form.port as number, verify_hostname: form.verify_hostname, reconcile_interval_s: form.reconcile_interval_s as number }
    : {};

  const handleCheck = async () => {
    if (hasErrors) return;
    setProbeFailure(null);
    try {
      const r = await update.mutateAsync({ ...full(), check_only: true }) as BrokerCheckResult;
      setCheck({ ok: r.ok, detail: r.detail });
    } catch (err) {
      toast(err instanceof Error ? err.message : String(err), 'danger');
    }
  };

  const handleSave = async () => {
    if (hasErrors || !dirty) return;
    setCheck(null);
    setWarning(null);
    try {
      const r = await update.mutateAsync({ ...patch, ...(skipCheck ? { skip_check: true } : {}) }) as BrokerApplyResult;
      setForm(toForm(r.connection));
      setProbeFailure(null);
      setSkipCheck(false);
      if (r.warning) setWarning(r.warning);
      toast(r.applied ? t('players.broker.connection.applied') : t('players.broker.connection.unchanged'), 'success');
    } catch (err) {
      const code = err instanceof ApiClientError ? err.code : '';
      if (code === 'probe_failed') {
        setProbeFailure((err as ApiClientError).detail ?? '');
      } else if (code === 'invalid_host' || code === 'invalid_port' || code === 'invalid_reconcile_interval_s') {
        const field = code === 'invalid_host' ? 'host' : code === 'invalid_port' ? 'port' : 'reconcile_interval_s';
        setFieldErrors({ [field]: t(`players.broker.connection.serverErrors.${code}`) });
      } else if (code === 'broker_disabled') {
        toast(t('players.broker.connection.serverErrors.broker_disabled'), 'danger');
      } else {
        toast(err instanceof Error ? err.message : String(err), 'danger');
      }
    }
  };

  const handleReconcile = async () => {
    try {
      await reconcile.mutateAsync();
      toast(t('players.broker.reconcile.requested'), 'info');
    } catch (err) {
      toast(err instanceof ApiClientError && err.code === 'broker_disabled'
        ? t('players.broker.connection.serverErrors.broker_disabled')
        : (err instanceof Error ? err.message : String(err)), 'danger');
    }
  };

  if (isLoading) return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  if (isError || !st) {
    return (
      <div className="p-7 flex flex-col gap-5">
        <PlayersSubNav />
        <p role="alert" className="text-sm text-[var(--danger)]">{t('players.broker.loadFailed')}</p>
      </div>
    );
  }

  const active = st.enabled && st.state !== 'disabled';
  const r = st.readiness;
  const crl = st.crl;
  const crlSoon = !!crl && crl.mode === 'local' && !!crl.valid_until && crl.valid_until * 1000 - nowMs < SOON_SECONDS * 1000;

  return (
    <div className="p-7 flex flex-col gap-5">
      <PlayersSubNav />

      <div className="flex items-center gap-3">
        <Radio size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)]">{t('players.broker.title')}</h1>
      </div>
      <p className="text-sm text-[var(--text-muted)] -mt-3 mb-0">{t('players.broker.lead')}</p>

      {/* ── State ─────────────────────────────────────────────────────────── */}
      <Block>
        <div className="flex flex-wrap items-center gap-3 mb-3">
          <StateBadge state={st.state} label={t(`players.broker.state.${st.state}.label`)} />
        </div>
        <p className="text-sm text-[var(--text-primary)] m-0">{t(`players.broker.state.${st.state}.text`)}</p>
        {!!st.detail && st.state !== 'connected' && (
          <p className="text-xs text-[var(--text-muted)] mt-2 mb-0">
            {t('players.broker.reason')}: <span className="font-mono">{st.detail}</span>
          </p>
        )}
      </Block>

      {/* ── What is needed ────────────────────────────────────────────────── */}
      {r && (
        <Block>
          <SectionTitle title={t('players.broker.readiness.title')} hint={t('players.broker.readiness.hint')} />
          <ul className="flex flex-col gap-3 list-none p-0 m-0">
            <Check ok={r.mosquitto_in_build} label={t('players.broker.readiness.mosquitto_in_build.label')}
                   fix={t('players.broker.readiness.mosquitto_in_build.fix')} />
            <Check ok={r.device_ca} label={t('players.broker.readiness.device_ca.label')}
                   fix={t('players.broker.readiness.device_ca.fix')} />
            <Check ok={r.trust_root} label={t('players.broker.readiness.trust_root.label')}
                   fix={t('players.broker.readiness.trust_root.fix')} />
            <Check ok={r.broker_url_configured} label={t('players.broker.readiness.broker_url_configured.label')}
                   fix={<>{t('players.broker.readiness.broker_url_configured.fix')}{' '}
                     <Link to="/players/settings" className="text-[var(--accent)] hover:underline">{t('players.broker.readiness.toSettings')}</Link></>} />
          </ul>
        </Block>
      )}

      {/* ── Reconciliation ────────────────────────────────────────────────── */}
      <Block>
        <SectionTitle title={t('players.broker.reconcile.title')}
                      hint={t('players.broker.reconcile.hint', { interval: st.reconcile_interval_s ?? 300 })} />
        {st.last_reconcile_at ? (
          <div className="flex flex-col gap-1.5 text-sm">
            <p className="m-0 text-[var(--text-primary)]">
              {t('players.broker.reconcile.last')}: {relativeFromNow(st.last_reconcile_at, nowMs, i18n.language)}
              <span className="text-[var(--text-muted)]"> ({absoluteTime(st.last_reconcile_at)})</span>
            </p>
            <p className={clsx('m-0 flex items-center gap-1.5', st.last_reconcile_ok ? 'text-[var(--success)]' : 'text-[var(--danger)]')}>
              {st.last_reconcile_ok ? <CheckCircle2 size={14} /> : <XCircle size={14} />}
              {st.last_reconcile_ok ? t('players.broker.reconcile.ok') : t('players.broker.reconcile.failed')}
            </p>
            {st.last_report && (
              <p className="m-0 text-xs text-[var(--text-muted)]">
                {t('players.broker.reconcile.counts', { issued: st.last_report.issued, removed: st.last_report.removed, failed: st.last_report.failed })}
                {st.last_report.error ? ` — ${st.last_report.error}` : ''}
              </p>
            )}
          </div>
        ) : (
          <p className="text-sm text-[var(--text-muted)] m-0">{t('players.broker.reconcile.never')}</p>
        )}
        <div className="mt-4">
          <button type="button" onClick={handleReconcile} disabled={!active || reconcile.isPending}
                  className="inline-flex items-center gap-2 px-4 py-2 border border-[var(--border-subtle)] rounded-md text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-40 transition-colors">
            <RefreshCw size={14} className={reconcile.isPending ? 'animate-spin' : ''} />
            {t('players.broker.reconcile.button')}
          </button>
        </div>
      </Block>

      {/* ── Revocation list ───────────────────────────────────────────────── */}
      {crl && (
        <Block>
          <SectionTitle title={t('players.broker.crl.title')} hint={t('players.broker.crl.hint')} />
          {crl.mode === 'off' ? (
            <p className="text-sm text-[var(--text-muted)] m-0">{t('players.broker.crl.off')}</p>
          ) : (
            <div className="flex flex-col gap-2 text-sm">
              {crl.waiting ? (
                <p className="m-0 flex items-center gap-1.5 text-[var(--warning)]">
                  <Loader2 size={14} className="animate-spin" aria-hidden />
                  {t('players.broker.crl.waiting')}
                </p>
              ) : (
                <p className={clsx('m-0 flex items-center gap-1.5', crl.last_ok ? 'text-[var(--success)]' : 'text-[var(--danger)]')}>
                  {crl.last_ok ? <CheckCircle2 size={14} /> : <XCircle size={14} />}
                  {crl.last_ok ? t('players.broker.crl.ok') : t('players.broker.crl.notPublished')}
                </p>
              )}
              {!!crl.error && <p className="m-0 text-xs text-[var(--danger)]">{t('players.broker.crl.error')}: <span className="font-mono">{crl.error}</span></p>}
              {!!crl.reload_error && (
                <p className="m-0 text-xs text-[var(--warning)]">
                  {t('players.broker.crl.reloadFailed')}:{' '}
                  {RELOAD_ERRORS.includes(crl.reload_error)
                    ? t(`players.broker.crl.reloadErrors.${crl.reload_error}`)
                    : <span className="font-mono">{crl.reload_error}</span>}
                </p>
              )}
              {!!crl.last_publish_at && (
                <p className="m-0 text-[var(--text-muted)]">
                  {t('players.broker.crl.published')}: {relativeFromNow(crl.last_publish_at, nowMs, i18n.language)}
                  {' · '}{t('players.broker.crl.counts', { revoked: crl.revoked ?? 0, generations: crl.generations ?? 0 })}
                </p>
              )}
              {!!crl.valid_until && (
                <p className={clsx('m-0 flex items-center gap-1.5', crlSoon ? 'text-[var(--warning)]' : 'text-[var(--text-muted)]')}>
                  {crlSoon && <AlertTriangle size={14} />}
                  {t('players.broker.crl.validUntil')}: {absoluteTime(crl.valid_until)}
                </p>
              )}
              {crlSoon && <p className="m-0 text-xs text-[var(--warning)]">{t('players.broker.crl.soon')}</p>}
              {!!crl.tls && (
                <p className={clsx('m-0', crl.tls.certificate === 'missing' ? 'text-[var(--danger)]' : 'text-[var(--text-muted)]')}>
                  {t('players.broker.crl.cert.title')}: {t(`players.broker.crl.cert.${crl.tls.certificate}`)}
                  {!!crl.tls.not_after && crl.tls.certificate !== 'missing' &&
                    <> · {t('players.broker.crl.validUntil')}: {absoluteTime(crl.tls.not_after)}</>}
                </p>
              )}
              {!!crl.prepare_error && (
                <p className="m-0 text-xs text-[var(--danger)]">
                  {t('players.broker.crl.filesNotReady')}:{' '}
                  {PREPARE_ERRORS.includes(crl.prepare_error)
                    ? t(`players.broker.crl.prepareErrors.${crl.prepare_error}`)
                    : <span className="font-mono">{crl.prepare_error}</span>}
                </p>
              )}
            </div>
          )}
        </Block>
      )}

      <ServerCertificates />

      {/* ── How the server reaches the broker ─────────────────────────────── */}
      <Block>
        <SectionTitle title={t('players.broker.connection.title')}
                      hint={active ? t('players.broker.connection.hint') : t('players.broker.connection.disabledHint')} />
        {form && (
          <div className="grid grid-cols-1 md:grid-cols-2 gap-5">
            <TextField id="pb-host" mono label={t('players.broker.connection.host.label')}
                       hint={t('players.broker.connection.host.hint')} value={form.host} disabled={!active}
                       onChange={v => upd('host', v)} error={fieldErrors.host ?? errors.host ?? null} />
            <NumberField id="pb-port" label={t('players.broker.connection.port.label')}
                         hint={t('players.broker.connection.port.hint')} value={form.port} min={1} max={65535} disabled={!active}
                         onChange={v => upd('port', v)} error={fieldErrors.port ?? errors.port ?? null} />
            <NumberField id="pb-interval" label={t('players.broker.connection.interval.label')}
                         hint={t('players.broker.connection.interval.hint')} value={form.reconcile_interval_s} min={10} max={86400} disabled={!active}
                         onChange={v => upd('reconcile_interval_s', v)} error={fieldErrors.reconcile_interval_s ?? errors.reconcile_interval_s ?? null} />
            <div className="md:col-span-2">
              <Toggle id="pb-verify" label={t('players.broker.connection.verify.label')} checked={form.verify_hostname} disabled={!active}
                      onChange={v => upd('verify_hostname', v)} hint={t('players.broker.connection.verify.hint')} />
              {!form.verify_hostname && (
                <p role="note" className="flex gap-2 text-xs text-[var(--warning)] mt-2 mb-0">
                  <AlertTriangle size={14} className="shrink-0 mt-0.5" />
                  <span>{t('players.broker.connection.verify.warning')}</span>
                </p>
              )}
            </div>
          </div>
        )}

        {check && (
          <p role="status" className={clsx('mt-4 mb-0 flex items-start gap-2 text-sm', check.ok ? 'text-[var(--success)]' : 'text-[var(--danger)]')}>
            {check.ok ? <CheckCircle2 size={16} className="mt-0.5 shrink-0" /> : <XCircle size={16} className="mt-0.5 shrink-0" />}
            <span>{check.ok ? t('players.broker.connection.checkOk') : t('players.broker.connection.checkFailed', { detail: check.detail })}</span>
          </p>
        )}

        {probeFailure !== null && (
          <div role="alert" className="mt-4 rounded-md border border-[var(--danger)] bg-[var(--danger)]/10 p-4">
            <p className="text-sm font-medium text-[var(--text-primary)] m-0">{t('players.broker.connection.probeFailed.title')}</p>
            <p className="text-sm text-[var(--text-muted)] mt-1 mb-0">
              {t('players.broker.connection.probeFailed.text')}
              {probeFailure && <> <span className="font-mono">{probeFailure}</span></>}
            </p>
            <label className="flex items-start gap-2 mt-3 text-sm text-[var(--text-primary)] cursor-pointer">
              <input type="checkbox" checked={skipCheck} onChange={e => setSkipCheck(e.target.checked)} className="mt-1" />
              <span>{t('players.broker.connection.probeFailed.skip')}</span>
            </label>
          </div>
        )}

        {warning && (
          <p role="note" className="flex gap-2 text-xs text-[var(--warning)] mt-4 mb-0">
            <AlertTriangle size={14} className="shrink-0 mt-0.5" />
            <span>{t(`players.broker.connection.warnings.${warning}`, { defaultValue: warning })}</span>
          </p>
        )}

        {active && (
          <div className="mt-5 flex flex-wrap items-center gap-3">
            <button type="button" onClick={handleCheck} disabled={hasErrors || update.isPending}
                    className="px-4 py-2 border border-[var(--border-subtle)] rounded-md text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-40 transition-colors">
              {update.isPending && update.variables?.check_only ? t('players.broker.connection.checking') : t('players.broker.connection.check')}
            </button>
            <button type="button" onClick={handleSave} disabled={!dirty || hasErrors || update.isPending}
                    className="px-5 py-2 rounded-md bg-[var(--accent)] text-white text-sm font-medium disabled:opacity-40 hover:bg-[var(--accent-hover)] transition-colors">
              {update.isPending && !update.variables?.check_only ? t('players.broker.connection.saving') : t('common.save')}
            </button>
            {update.isPending && <span className="text-xs text-[var(--text-muted)]">{t('players.broker.connection.mayTakeAWhile')}</span>}
          </div>
        )}
      </Block>
    </div>
  );
}
