import React from 'react';
import { useTranslation } from 'react-i18next';
import { Clock, Wifi, CheckCircle2, XCircle } from 'lucide-react';
import {
  useTimeConfig, useSaveTimeConfig, useTestNtp,
  type TimeConfigPatch, type TimeSourceKind, type TimeProbeResult,
} from '@/api/queries/system';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { useToast } from '@/hooks/useToast';

// fix33 D — System Time settings page.
// Three modes (NTP / system_local / manual). server_timezone is global and
// applies to all channels that inherit it. Manual mode lets the operator
// pin an arbitrary wall-clock time via offset_ms (delta) — we expose the
// epoch_unix_ms form for the UI so the operator just enters "what time is
// it now?" and we compute the offset on save.

type Form = {
  source:          TimeSourceKind;
  server_timezone: string;
  ntp_enabled:     boolean;
  ntp_servers:     string;          // comma-or-newline separated
  ntp_poll_s:      number;
  manual_local_dt: string;          // <input type="datetime-local"> value; "" = leave unchanged
};

const COMMON_TZ = [
  'UTC', 'Europe/Moscow', 'Europe/Berlin', 'Europe/London',
  'America/Los_Angeles', 'America/New_York', 'America/Sao_Paulo',
  'Asia/Tokyo', 'Asia/Shanghai', 'Asia/Dubai', 'Asia/Almaty',
  'Australia/Sydney',
];

const defaultForm = (): Form => ({
  source:          'system_local',
  server_timezone: 'UTC',
  ntp_enabled:     false,
  ntp_servers:     'pool.ntp.org',
  ntp_poll_s:      64,
  manual_local_dt: '',
});

const parseServers = (raw: string): string[] =>
  raw.split(/[,\n]/).map(s => s.trim()).filter(Boolean);

const fmtUnixSec = (sec: number | null | undefined): string => {
  if (!sec) return '—';
  return new Date(sec * 1000).toLocaleString();
};

const fmtUnixMs = (ms: number | null | undefined): string => {
  if (!ms) return '—';
  return new Date(ms).toLocaleString();
};

// <input type="datetime-local"> хранит локальное время браузера в формате
// "YYYY-MM-DDTHH:MM:SS" без часового пояса. Конвертим в эту строку из
// epoch-ms и обратно, опираясь на локальную TZ.
const msToLocalInput = (ms: number): string => {
  const d = new Date(ms);
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}` +
    `T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  );
};

const localInputToMs = (s: string): number => new Date(s).getTime();

export default function SystemTimePage() {
  const { t }   = useTranslation();
  const toast   = useToast();
  const { data, isLoading } = useTimeConfig();
  const { mutateAsync: save, isPending: saving } = useSaveTimeConfig();
  const { mutateAsync: test, isPending: testing } = useTestNtp();

  const [form, setForm]   = React.useState<Form>(defaultForm());
  const [probe, setProbe] = React.useState<TimeProbeResult[]>([]);

  React.useEffect(() => {
    if (!data) return;
    const c = data.config;
    setForm({
      source:          c.source,
      server_timezone: c.server_timezone || 'UTC',
      ntp_enabled:     c.ntp.enabled,
      ntp_servers:     (c.ntp.servers ?? []).join('\n'),
      ntp_poll_s:      c.ntp.poll_interval_s ?? 64,
      manual_local_dt: '',
    });
  }, [data]);

  const upd = <K extends keyof Form>(k: K, v: Form[K]) =>
    setForm(p => ({ ...p, [k]: v }));

  // ── save ──────────────────────────────────────────────────────────────────
  const handleSave = async () => {
    const patch: TimeConfigPatch = {
      source:          form.source,
      server_timezone: form.server_timezone.trim() || 'UTC',
      ntp: {
        enabled:         form.ntp_enabled,
        servers:         parseServers(form.ntp_servers),
        poll_interval_s: form.ntp_poll_s,
      },
    };
    if (form.source === 'manual' && form.manual_local_dt.trim()) {
      const ms = localInputToMs(form.manual_local_dt);
      if (!Number.isFinite(ms)) {
        toast(t('systemTime.invalidEpoch'), 'danger');
        return;
      }
      patch.manual = { epoch_unix_ms: ms };
    }
    try {
      await save(patch);
      toast(t('common.savedImmediately'), 'success');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    }
  };

  // ── probe ─────────────────────────────────────────────────────────────────
  const handleTest = async () => {
    try {
      const servers = parseServers(form.ntp_servers);
      const r = await test({ servers: servers.length ? servers : undefined });
      setProbe(r.results);
      const failed = r.results.filter(x => !x.ok).length;
      if (failed === 0) toast(t('systemTime.probeOk'), 'success');
      else toast(t('systemTime.probeFailed', { n: failed }), 'danger');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    }
  };

  // ── manual: epoch input helper ────────────────────────────────────────────
  const useNowAsManual = () =>
    setForm(p => ({ ...p, manual_local_dt: msToLocalInput(Date.now()) }));

  if (isLoading) {
    return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  }

  const Field = ({ label, children }: { label: string; children: React.ReactNode }) => (
    <div className="flex flex-col gap-1.5">
      <label className="text-sm font-medium text-[var(--text-muted)]">{label}</label>
      {children}
    </div>
  );

  const inputCls =
    'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]';

  return (
    <div className="p-7 flex flex-col gap-5">
      <SubNav items={SETTINGS_NAV} />

      <div className="flex items-center gap-3">
        <Clock size={20} className="text-[var(--accent)]" />
        <h1 className="text-xl font-semibold text-[var(--text-primary)]">{t('systemTime.title')}</h1>
      </div>

      {/* ── Source ──────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('systemTime.source')}</h2>
        <div className="grid grid-cols-3 gap-3">
          {(['system_local', 'ntp', 'manual'] as const).map(s => (
            <button key={s}
                    onClick={() => upd('source', s)}
                    className={`px-4 py-3 border rounded-md text-sm text-left transition-colors ${
                      form.source === s
                        ? 'border-[var(--accent)] bg-[var(--accent)]/10 text-[var(--text-primary)]'
                        : 'border-[var(--border-subtle)] text-[var(--text-muted)] hover:text-[var(--text-primary)]'
                    }`}>
              <div className="font-medium">{t(`systemTime.modes.${s}.title`)}</div>
              <div className="text-xs mt-1 text-[var(--text-muted)]">{t(`systemTime.modes.${s}.hint`)}</div>
            </button>
          ))}
        </div>
      </Block>

      {/* ── Server timezone ─────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('systemTime.timezone')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('systemTime.tzPreset')}>
            <select value={COMMON_TZ.includes(form.server_timezone) ? form.server_timezone : ''}
                    onChange={e => upd('server_timezone', e.target.value || form.server_timezone)}
                    className={inputCls}>
              <option value="">{t('systemTime.tzCustom')}</option>
              {COMMON_TZ.map(z => <option key={z} value={z}>{z}</option>)}
            </select>
          </Field>
          <Field label={t('systemTime.tzCustomInput')}>
            <input value={form.server_timezone}
                   onChange={e => upd('server_timezone', e.target.value)}
                   placeholder="Europe/Moscow" className={inputCls} />
          </Field>
        </div>
        <p className="text-xs text-[var(--text-muted)] mt-3">{t('systemTime.tzHelp')}</p>
      </Block>

      {/* ── NTP block ───────────────────────────────────────────────────── */}
      {form.source === 'ntp' && (
        <Block>
          <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('systemTime.ntpSection')}</h2>

          <label className="flex items-center gap-2 cursor-pointer mb-4">
            <div onClick={() => upd('ntp_enabled', !form.ntp_enabled)}
                 className={`w-9 h-5 rounded-full relative transition-colors ${form.ntp_enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
              <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${form.ntp_enabled ? 'left-4' : 'left-0.5'}`} />
            </div>
            <span className="text-sm text-[var(--text-primary)]">{t('systemTime.ntpEnabled')}</span>
          </label>

          <div className="grid grid-cols-3 gap-4">
            <div className="col-span-2">
              <Field label={t('systemTime.ntpServers')}>
                <textarea value={form.ntp_servers}
                          onChange={e => upd('ntp_servers', e.target.value)}
                          rows={3}
                          placeholder="pool.ntp.org&#10;time.cloudflare.com"
                          className={`${inputCls} font-mono resize-none`} />
              </Field>
            </div>
            <Field label={t('systemTime.ntpPollInterval')}>
              <input type="number" min={16} max={4096}
                     value={form.ntp_poll_s}
                     onChange={e => upd('ntp_poll_s', parseInt(e.target.value, 10) || 64)}
                     className={inputCls} />
            </Field>
          </div>

          {data?.config.ntp.last_sync_at && (
            <div className="mt-4 text-xs text-[var(--text-muted)] flex gap-6">
              <span>{t('systemTime.lastSync')}: {fmtUnixSec(data.config.ntp.last_sync_at)}</span>
              <span>{t('systemTime.lastOffset')}: {data.config.ntp.last_offset_ms ?? 0} ms</span>
            </div>
          )}

          <div className="mt-4 flex items-center gap-3">
            <button disabled={testing} onClick={handleTest}
                    className="flex items-center gap-1.5 px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-50 transition-colors">
              <Wifi size={14} /> {testing ? t('systemTime.probing') : t('systemTime.probe')}
            </button>
          </div>

          {probe.length > 0 && (
            <div className="mt-4 border border-[var(--border-subtle)] rounded-md overflow-hidden">
              <table className="w-full text-sm">
                <thead className="bg-canvas">
                  <tr className="text-[var(--text-muted)] text-xs">
                    <th className="text-left px-3 py-2">{t('systemTime.colServer')}</th>
                    <th className="text-left px-3 py-2">{t('systemTime.colStatus')}</th>
                    <th className="text-right px-3 py-2">{t('systemTime.colOffset')}</th>
                    <th className="text-right px-3 py-2">{t('systemTime.colRtt')}</th>
                  </tr>
                </thead>
                <tbody>
                  {probe.map((r, i) => (
                    <tr key={i} className="border-t border-[var(--border-subtle)]">
                      <td className="px-3 py-2 font-mono text-[var(--text-primary)]">{r.server}</td>
                      <td className="px-3 py-2">
                        {r.ok
                          ? <span className="flex items-center gap-1.5 text-[var(--success)]"><CheckCircle2 size={14} /> ok</span>
                          : <span className="flex items-center gap-1.5 text-[var(--danger)]"><XCircle size={14} /> {r.error}</span>}
                      </td>
                      <td className="px-3 py-2 text-right text-[var(--text-muted)]">{r.ok ? `${r.offset_ms} ms` : '—'}</td>
                      <td className="px-3 py-2 text-right text-[var(--text-muted)]">{r.ok ? `${r.round_trip_ms} ms` : '—'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </Block>
      )}

      {/* ── Manual block ────────────────────────────────────────────────── */}
      {form.source === 'manual' && (
        <Block>
          <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('systemTime.manualSection')}</h2>
          <p className="text-sm text-[var(--text-muted)] mb-4">{t('systemTime.manualHelp')}</p>

          <div className="grid grid-cols-3 gap-4">
            <div className="col-span-2">
              <Field label={t('systemTime.manualEpoch')}>
                <input type="datetime-local" step="1"
                       value={form.manual_local_dt}
                       onChange={e => upd('manual_local_dt', e.target.value)}
                       className={inputCls} />
              </Field>
            </div>
            <div className="flex items-end">
              <button onClick={useNowAsManual}
                      className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                {t('systemTime.useBrowserNow')}
              </button>
            </div>
          </div>
          <p className="text-xs text-[var(--text-muted)] mt-2">
            {t('systemTime.manualTzHint', {
              tz: Intl.DateTimeFormat().resolvedOptions().timeZone,
            })}
          </p>
          {data && (
            <div className="mt-4 text-xs text-[var(--text-muted)] flex gap-6">
              <span>{t('systemTime.currentOffset')}: {data.runtime.offset_ms} ms</span>
              {data.config.manual.set_at && (
                <span>{t('systemTime.setAt')}: {fmtUnixSec(data.config.manual.set_at)}</span>
              )}
            </div>
          )}
        </Block>
      )}

      {/* ── Runtime snapshot ────────────────────────────────────────────── */}
      {data && (
        <Block>
          <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('systemTime.runtime')}</h2>
          <div className="grid grid-cols-2 gap-4 text-sm">
            <div className="flex flex-col gap-1">
              <span className="text-[var(--text-muted)] text-xs">{t('systemTime.runtimeSource')}</span>
              <span className="text-[var(--text-primary)] font-mono">{data.runtime.source}</span>
            </div>
            <div className="flex flex-col gap-1">
              <span className="text-[var(--text-muted)] text-xs">{t('systemTime.runtimeOffset')}</span>
              <span className="text-[var(--text-primary)] font-mono">{data.runtime.offset_ms} ms</span>
            </div>
            <div className="flex flex-col gap-1">
              <span className="text-[var(--text-muted)] text-xs">{t('systemTime.runtimeEffective')}</span>
              <span className="text-[var(--text-primary)] font-mono">{fmtUnixMs(data.runtime.effective_now)}</span>
            </div>
            <div className="flex flex-col gap-1">
              <span className="text-[var(--text-muted)] text-xs">{t('systemTime.runtimeSystem')}</span>
              <span className="text-[var(--text-primary)] font-mono">{fmtUnixMs(data.runtime.system_now)}</span>
            </div>
          </div>
        </Block>
      )}

      <button disabled={saving} onClick={handleSave}
              className="self-start px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
        {saving ? t('common.saving') : t('common.save')}
      </button>
    </div>
  );
}
