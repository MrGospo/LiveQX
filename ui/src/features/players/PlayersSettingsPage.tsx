import React from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { MonitorSmartphone, AlertTriangle } from 'lucide-react';
import {
  usePlayersSettings, useSavePlayersSettings,
  type PlayersSettings, type PlayersSettingsPatch, type ClockPolicy,
} from '@/api/queries/players';
import { ApiClientError } from '@/api/client';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { useToast } from '@/hooks/useToast';
import { PLAYERS_NAV } from './nav';
import { TextField, NumberField, Field, SectionTitle, inputCls } from './fields';
import { humanSeconds } from './format';

// Settings of the screens (players) module. Numbers have the same limits as on
// the server, so a mistake is shown beside the field instead of after a round trip;
// the server still has the last word and its refusals land on the same fields.

type NumKey =
  | 'token_ttl_days' | 'command_ttl_s' | 'confirm_timeout_restart_s' | 'confirm_timeout_reboot_s'
  | 'confirm_timeout_reload_s' | 'command_batch_size' | 'command_batch_stop_percent'
  | 'events_retention_days' | 'commands_retention_days';
type StrKey = 'server_url' | 'device_api_url' | 'mqtt_broker_url' | 'ca_cert_pem';

const RANGES: Record<NumKey, [number, number]> = {
  token_ttl_days:             [1, 365],
  command_ttl_s:              [30, 86400],
  confirm_timeout_restart_s:  [30, 86400],
  confirm_timeout_reboot_s:   [30, 86400],
  confirm_timeout_reload_s:   [30, 86400],
  command_batch_size:         [1, 100],
  command_batch_stop_percent: [1, 100],
  events_retention_days:      [7, 3650],
  commands_retention_days:    [7, 3650],
};
const NUM_KEYS = Object.keys(RANGES) as NumKey[];
const STR_KEYS: StrKey[] = ['server_url', 'device_api_url', 'mqtt_broker_url', 'ca_cert_pem'];
const SECONDS_KEYS: NumKey[] = ['command_ttl_s', 'confirm_timeout_restart_s', 'confirm_timeout_reboot_s', 'confirm_timeout_reload_s'];
const URL_SCHEME: Record<'server_url' | 'device_api_url' | 'mqtt_broker_url', string> = {
  server_url: 'https://', device_api_url: 'https://', mqtt_broker_url: 'mqtts://',
};

type Form = { [K in NumKey]: number | '' } & { [K in StrKey]: string } & { clock_policy: ClockPolicy };

const toForm = (s: PlayersSettings): Form => ({
  server_url: s.server_url, device_api_url: s.device_api_url, mqtt_broker_url: s.mqtt_broker_url,
  ca_cert_pem: s.ca_cert_pem, clock_policy: s.clock_policy,
  token_ttl_days: s.token_ttl_days, command_ttl_s: s.command_ttl_s,
  confirm_timeout_restart_s: s.confirm_timeout_restart_s, confirm_timeout_reboot_s: s.confirm_timeout_reboot_s,
  confirm_timeout_reload_s: s.confirm_timeout_reload_s, command_batch_size: s.command_batch_size,
  command_batch_stop_percent: s.command_batch_stop_percent,
  events_retention_days: s.events_retention_days, commands_retention_days: s.commands_retention_days,
});

// The server's refusal codes: "invalid_<setting>" names the setting.
const serverErrorField = (code: string): keyof Form | null => {
  const m = /^invalid_(.+)$/.exec(code);
  if (!m) return null;
  const key = m[1] as keyof Form;
  return key in RANGES || STR_KEYS.includes(key as StrKey) || key === 'clock_policy' ? key : null;
};

export default function PlayersSettingsPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { data, isLoading, isError } = usePlayersSettings();
  const { mutateAsync: save, isPending: saving } = useSavePlayersSettings();

  const [form, setForm] = React.useState<Form | null>(null);
  const [serverErrors, setServerErrors] = React.useState<Partial<Record<keyof Form, string>>>({});

  React.useEffect(() => { if (data) setForm(toForm(data)); }, [data]);

  const upd = <K extends keyof Form>(k: K, v: Form[K]) => {
    setForm(p => (p ? { ...p, [k]: v } : p));
    setServerErrors(p => { if (!(k in p)) return p; const n = { ...p }; delete n[k]; return n; });
  };

  // ── checks done here, before asking the server ─────────────────────────────
  const errors = React.useMemo(() => {
    const e: Partial<Record<keyof Form, string>> = {};
    if (!form) return e;
    for (const k of NUM_KEYS) {
      const v = form[k];
      const [lo, hi] = RANGES[k];
      if (v === '' || Number.isNaN(v)) e[k] = t('players.settings.errors.required');
      else if (v < lo || v > hi) e[k] = t('players.settings.errors.range', { min: lo, max: hi });
    }
    for (const k of Object.keys(URL_SCHEME) as (keyof typeof URL_SCHEME)[]) {
      const v = form[k].trim();
      if (v && !v.startsWith(URL_SCHEME[k])) e[k] = t('players.settings.errors.scheme', { scheme: URL_SCHEME[k] });
    }
    return e;
  }, [form, t]);

  const patch = React.useMemo<PlayersSettingsPatch>(() => {
    const p: PlayersSettingsPatch = {};
    if (!form || !data) return p;
    for (const k of NUM_KEYS) if (form[k] !== '' && form[k] !== data[k]) p[k] = form[k] as number;
    for (const k of STR_KEYS) {
      const v = k === 'ca_cert_pem' ? form[k] : form[k].trim();
      if (v !== data[k]) p[k] = v;
    }
    if (form.clock_policy !== data.clock_policy) p.clock_policy = form.clock_policy;
    return p;
  }, [form, data]);

  const dirty = Object.keys(patch).length > 0;
  const hasErrors = Object.keys(errors).length > 0;

  const handleSave = async () => {
    if (hasErrors) { toast(t('players.settings.fixErrors'), 'danger'); return; }
    try {
      await save(patch);
      setServerErrors({});
      toast(t('players.settings.saved'), 'success');
    } catch (err) {
      const code = err instanceof ApiClientError ? err.code : '';
      const field = serverErrorField(code);
      if (field) {
        setServerErrors({ [field]: t(`players.settings.serverErrors.${code}`, { defaultValue: code }) });
        toast(t('players.settings.fixErrors'), 'danger');
      } else {
        toast(err instanceof Error ? err.message : String(err), 'danger');
      }
    }
  };

  if (isLoading || (!form && !isError)) {
    return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  }
  if (isError || !form || !data) {
    return (
      <div className="p-7 flex flex-col gap-5">
        <SubNav items={PLAYERS_NAV} />
        <p role="alert" className="text-sm text-[var(--danger)]">{t('players.settings.loadFailed')}</p>
      </div>
    );
  }

  const err = (k: keyof Form) => serverErrors[k] ?? errors[k] ?? null;
  const num = (k: NumKey) => (
    <NumberField
      id={`ps-${k}`}
      label={t(`players.settings.${k}.label`)}
      hint={t(`players.settings.${k}.hint`)}
      value={form[k]}
      onChange={v => upd(k, v)}
      min={RANGES[k][0]}
      max={RANGES[k][1]}
      error={err(k)}
      note={SECONDS_KEYS.includes(k) && form[k] !== '' && !errors[k] ? `≈ ${humanSeconds(form[k] as number, t)}` : undefined}
    />
  );

  const notConfigured = !data.server_url || !data.device_api_url || !data.mqtt_broker_url;

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={PLAYERS_NAV} />

      <div className="flex items-center gap-3">
        <MonitorSmartphone size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)]">{t('players.settings.title')}</h1>
      </div>
      <p className="text-sm text-[var(--text-muted)] -mt-3 mb-0">{t('players.settings.lead')}</p>

      {notConfigured && (
        <div role="status" className="flex gap-3 rounded-xl border border-[var(--warning)] bg-[var(--warning)]/10 p-4">
          <AlertTriangle size={18} className="text-[var(--warning)] shrink-0 mt-0.5" />
          <div>
            <p className="text-sm font-medium text-[var(--text-primary)] m-0">{t('players.settings.notConfigured.title')}</p>
            <p className="text-sm text-[var(--text-muted)] mt-1 mb-0">{t('players.settings.notConfigured.text')}</p>
          </div>
        </div>
      )}

      <Block>
        <SectionTitle title={t('players.settings.addresses.title')} hint={t('players.settings.addresses.hint')} />
        <div className="grid grid-cols-1 gap-5">
          <TextField id="ps-server_url" mono label={t('players.settings.server_url.label')}
                     hint={t('players.settings.server_url.hint')} placeholder="https://liveqx.example.lan"
                     value={form.server_url} onChange={v => upd('server_url', v)} error={err('server_url')} />
          <TextField id="ps-device_api_url" mono label={t('players.settings.device_api_url.label')}
                     hint={t('players.settings.device_api_url.hint')} placeholder="https://liveqx.example.lan:8444"
                     value={form.device_api_url} onChange={v => upd('device_api_url', v)} error={err('device_api_url')} />
          <TextField id="ps-mqtt_broker_url" mono label={t('players.settings.mqtt_broker_url.label')}
                     hint={t('players.settings.mqtt_broker_url.hint')} placeholder="mqtts://liveqx.example.lan:8883"
                     value={form.mqtt_broker_url} onChange={v => upd('mqtt_broker_url', v)} error={err('mqtt_broker_url')} />
        </div>
      </Block>

      <Block>
        <SectionTitle title={t('players.settings.pairing.title')} />
        <div className="grid grid-cols-1 gap-5">
          {num('token_ttl_days')}
          <Field id="ps-clock_policy" label={t('players.settings.clock_policy.label')}
                 hint={t('players.settings.clock_policy.hint')} error={err('clock_policy')}>
            <select id="ps-clock_policy" value={form.clock_policy}
                    onChange={e => upd('clock_policy', e.target.value as ClockPolicy)}
                    className={`${inputCls} max-w-xs`}>
              <option value="strict">{t('players.settings.clock_policy.strict')}</option>
              <option value="relaxed">{t('players.settings.clock_policy.relaxed')}</option>
            </select>
          </Field>
          {form.clock_policy === 'relaxed' && (
            <p role="note" className="flex gap-2 text-xs text-[var(--warning)] m-0">
              <AlertTriangle size={14} className="shrink-0 mt-0.5" />
              <span>{t('players.settings.clock_policy.relaxedWarning')}</span>
            </p>
          )}
          <details>
            <summary className="cursor-pointer text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)]">
              {t('players.settings.ca_cert_pem.summary')}
            </summary>
            <div className="mt-3">
              <Field id="ps-ca_cert_pem" label={t('players.settings.ca_cert_pem.label')}
                     hint={t('players.settings.ca_cert_pem.hint')} error={err('ca_cert_pem')}>
                <textarea id="ps-ca_cert_pem" rows={6} spellCheck={false} value={form.ca_cert_pem}
                          onChange={e => upd('ca_cert_pem', e.target.value)}
                          className={`${inputCls} font-mono resize-y`} />
              </Field>
            </div>
          </details>
        </div>
      </Block>

      <Block>
        <SectionTitle title={t('players.settings.commands.title')} hint={t('players.settings.commands.hint')} />
        <div className="grid grid-cols-1 md:grid-cols-2 gap-5">
          {num('command_ttl_s')}
          {num('confirm_timeout_restart_s')}
          {num('confirm_timeout_reboot_s')}
          {num('confirm_timeout_reload_s')}
          {num('command_batch_size')}
          {num('command_batch_stop_percent')}
        </div>
      </Block>

      <Block>
        <SectionTitle title={t('players.settings.retention.title')} hint={t('players.settings.retention.hint')} />
        <div className="grid grid-cols-1 md:grid-cols-2 gap-5">
          {num('events_retention_days')}
          {num('commands_retention_days')}
        </div>
      </Block>

      <div className="flex items-center gap-4">
        <button type="button" onClick={handleSave} disabled={!dirty || saving}
                className="px-5 py-2 rounded-md bg-[var(--accent)] text-white text-sm font-medium disabled:opacity-40 hover:bg-[var(--accent-hover)] transition-colors">
          {saving ? t('players.settings.saving') : t('common.save')}
        </button>
        {dirty
          ? <span className="text-xs text-[var(--text-muted)]">{t('players.settings.unsaved')}</span>
          : <Link to="/players/broker" className="text-xs text-[var(--accent)] hover:underline">{t('players.settings.toBroker')}</Link>}
      </div>
    </div>
  );
}
