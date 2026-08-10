import React from 'react';
import { useTranslation } from 'react-i18next';
import { Plus, X, Wifi } from 'lucide-react';
import { useLdapConfig, useSaveLdapConfig, useTestLdap } from '@/api/queries/auth';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { useToast } from '@/hooks/useToast';
import type { LdapConfigRequest } from '@/api/types';

type Role = 'admin' | 'operator' | 'viewer';
interface MappingRow { id: number; dn: string; role: Role; }

const TLS_MODES = ['ldaps', 'starttls', 'none'] as const;
const ROLES: Role[] = ['admin', 'operator', 'viewer'];

const defaultForm = (): LdapConfigRequest => ({
  enabled: true,
  server: '',
  tls_mode: 'ldaps',
  bind_dn: '',
  base_dn: '',
  user_filter: '(sAMAccountName=%s)',
  email_attribute: 'mail',
  group_attribute: 'memberOf',
  recheck_groups_on_refresh: true,
  network_timeout_sec: 5,
});

export default function LdapPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { data: cfg, isLoading } = useLdapConfig();
  const { mutateAsync: save, isPending: saving } = useSaveLdapConfig();
  const { mutateAsync: test, isPending: testing } = useTestLdap();

  const [form,    setForm]    = React.useState<LdapConfigRequest>(defaultForm());
  const [pwdSet,  setPwdSet]  = React.useState(false);
  const [pwdMode, setPwdMode] = React.useState<'unchanged' | 'set' | 'clear'>('unchanged');
  const [pwdNew,  setPwdNew]  = React.useState('');
  const [mappings, setMappings] = React.useState<MappingRow[]>([]);
  const mappingId = React.useRef(0);
  const [testResult, setTestResult] = React.useState<{ ok: boolean; message: string } | null>(null);

  // ── load saved config ─────────────────────────────────────────────────────
  React.useEffect(() => {
    if (!cfg || !cfg.configured) return;
    const {
      configured: _c, bind_password: _bp, bind_password_set,
      group_role_map, channel_acl: _acl,
      ...rest
    } = cfg;
    setForm(f => ({ ...defaultForm(), ...f, ...rest }));
    setPwdSet(!!bind_password_set);
    setPwdMode('unchanged');
    setPwdNew('');
    const initialMappings: MappingRow[] = Object.entries(group_role_map ?? {}).map(([dn, role]) => ({
      id: ++mappingId.current,
      dn,
      role: role as Role,
    }));
    setMappings(initialMappings);
  }, [cfg]);

  const upd = <K extends keyof LdapConfigRequest>(k: K, v: LdapConfigRequest[K]) =>
    setForm(p => ({ ...p, [k]: v }));

  const addMapping = () =>
    setMappings(m => [...m, { id: ++mappingId.current, dn: '', role: 'viewer' }]);
  const updMapping = (id: number, patch: Partial<Omit<MappingRow, 'id'>>) =>
    setMappings(m => m.map(r => r.id === id ? { ...r, ...patch } : r));
  const rmMapping = (id: number) =>
    setMappings(m => m.filter(r => r.id !== id));

  // ── save ──────────────────────────────────────────────────────────────────
  const handleSave = async () => {
    if (form.enabled) {
      if (!(form.bind_dn ?? '').trim()) {
        toast(t('ldap.bindDnRequired'), 'danger');
        return;
      }
      const pwdEffectivelyEmpty =
        pwdMode === 'clear' ||
        (pwdMode === 'set' && pwdNew.trim() === '') ||
        (pwdMode === 'unchanged' && !pwdSet);
      if (pwdEffectivelyEmpty) {
        toast(t('ldap.bindPwdRequired'), 'danger');
        return;
      }
    }

    const payload: LdapConfigRequest = {
      ...form,
      group_role_map: Object.fromEntries(
        mappings
          .map(m => [m.dn.trim(), m.role] as const)
          .filter(([dn]) => dn.length > 0),
      ),
    };
    if (pwdMode === 'set' && pwdNew) payload.bind_password = pwdNew;
    if (pwdMode === 'clear')         payload.bind_password_unset = true;

    try {
      await save(payload);
      setPwdMode('unchanged');
      setPwdNew('');
      setPwdSet(pwdMode === 'clear' ? false : pwdMode === 'set' ? true : pwdSet);
      toast(t('ldap.saved'), 'success');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    }
  };

  // ── test ──────────────────────────────────────────────────────────────────
  const handleTest = async () => {
    try {
      const r = await test(undefined);
      const ok = r.ping.ok && (!r.bind || r.bind.ok);
      const message = !r.ping.ok
        ? t('ldap.testPingFail', { err: r.ping.error ?? 'unknown' })
        : r.bind && !r.bind.ok
          ? t('ldap.testBindFail', { reason: r.bind.reason, err: r.bind.error ? ` (${r.bind.error})` : '' })
          : t('ldap.testOk', { ms: r.ping.latency_ms, bind: r.bind ? t('ldap.testBindOk') : '' });
      setTestResult({ ok, message });
      toast(message, ok ? 'success' : 'danger');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    }
  };

  if (isLoading) {
    return <div className="p-7"><div className="h-96 bg-surface2 rounded-xl animate-pulse" /></div>;
  }

  // ── UI helpers ────────────────────────────────────────────────────────────
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
        <h1 className="text-xl font-semibold text-[var(--text-primary)]">{t('ldap.title')}</h1>
        <label className="flex items-center gap-2 cursor-pointer">
          <div onClick={() => upd('enabled', !form.enabled)}
               className={`w-9 h-5 rounded-full relative transition-colors ${form.enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
            <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${form.enabled ? 'left-4' : 'left-0.5'}`} />
          </div>
          <span className="text-sm text-[var(--text-primary)]">{t('ldap.enabled')}</span>
        </label>
      </div>

      {testResult && (
        <div className={`p-3 rounded-lg text-sm border ${testResult.ok
          ? 'bg-[var(--success)]/10 border-[var(--success)]/30'
          : 'bg-[var(--danger)]/10 border-[var(--danger)]/30'} text-[var(--text-primary)]`}>
          {testResult.message}
        </div>
      )}

      {/* ── Server ────────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('ldap.secServer')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('ldap.fieldServerUrl')}>
            <input value={form.server ?? ''} onChange={e => upd('server', e.target.value)}
                   placeholder="ldaps://ad.corp.local:636"
                   className={inputCls} />
          </Field>
          <Field label={t('ldap.fieldTlsMode')}>
            <select value={form.tls_mode ?? 'ldaps'} onChange={e => upd('tls_mode', e.target.value as any)}
                    className={inputCls}>
              {TLS_MODES.map(m => <option key={m} value={m}>{m}</option>)}
            </select>
          </Field>
        </div>
      </Block>

      {/* ── Bind credentials ─────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('ldap.secBind')}</h2>
        <Field label={t('ldap.fieldBindDn')}>
          <input value={form.bind_dn ?? ''} onChange={e => upd('bind_dn', e.target.value)}
                 placeholder="CN=svc-streaming,OU=Service,DC=corp,DC=local"
                 className={inputCls} />
          <p className="text-xs text-[var(--text-muted)] mt-1 leading-relaxed">{t('ldap.fieldBindDnHint')}</p>
        </Field>

        <div className="mt-4">
          <label className="text-sm font-medium text-[var(--text-muted)] mb-1.5 block">{t('ldap.fieldBindPwd')}</label>
          {pwdMode === 'unchanged' ? (
            <div className="flex items-center gap-2">
              <div className={`flex-1 ${inputCls} ${pwdSet ? '' : 'text-[var(--text-muted)] italic'}`}>
                {pwdSet ? '••••••••••' : t('ldap.pwdNotSet')}
              </div>
              <button onClick={() => setPwdMode('set')}
                      className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                {t('ldap.pwdEdit')}
              </button>
            </div>
          ) : pwdMode === 'set' ? (
            <div className="flex items-center gap-2">
              <input type="password" autoFocus
                     value={pwdNew} onChange={e => setPwdNew(e.target.value)}
                     placeholder={t('ldap.pwdNew')}
                     className={`flex-1 ${inputCls}`} />
              <button onClick={() => { setPwdMode('clear'); setPwdNew(''); }}
                      className="px-3 py-2 border border-[var(--danger)]/40 text-sm text-[var(--danger)] hover:bg-[var(--danger)]/10 rounded-md transition-colors">
                {t('ldap.pwdClear')}
              </button>
              <button onClick={() => { setPwdMode('unchanged'); setPwdNew(''); }}
                      className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                {t('ldap.pwdCancel')}
              </button>
            </div>
          ) : (
            <div className="flex items-center gap-2">
              <div className={`flex-1 ${inputCls} text-[var(--danger)] italic`}>{t('ldap.pwdClear')}</div>
              <button onClick={() => { setPwdMode('unchanged'); }}
                      className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                {t('ldap.pwdCancel')}
              </button>
            </div>
          )}
        </div>
      </Block>

      {/* ── User search ──────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('ldap.secSearch')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('ldap.fieldBaseDn')}>
            <input value={form.base_dn ?? ''} onChange={e => upd('base_dn', e.target.value)}
                   placeholder="OU=Users,DC=corp,DC=local"
                   className={inputCls} />
          </Field>
          <Field label={t('ldap.fieldUserFilter')}>
            <input value={form.user_filter ?? ''} onChange={e => upd('user_filter', e.target.value)}
                   placeholder="(sAMAccountName=%s)"
                   className={inputCls} />
          </Field>
          <Field label={t('ldap.fieldEmailAttr')}>
            <input value={form.email_attribute ?? ''} onChange={e => upd('email_attribute', e.target.value)}
                   className={inputCls} />
          </Field>
          <Field label={t('ldap.fieldGroupAttr')}>
            <input value={form.group_attribute ?? ''} onChange={e => upd('group_attribute', e.target.value)}
                   className={inputCls} />
          </Field>
        </div>

        <label className="flex items-center gap-2 cursor-pointer mt-4 self-start">
          <div onClick={() => upd('recheck_groups_on_refresh', !form.recheck_groups_on_refresh)}
               className={`w-9 h-5 rounded-full relative transition-colors ${form.recheck_groups_on_refresh ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
            <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${form.recheck_groups_on_refresh ? 'left-4' : 'left-0.5'}`} />
          </div>
          <span className="text-sm text-[var(--text-primary)]">{t('ldap.fieldRecheck')}</span>
        </label>
      </Block>

      {/* ── Group → role mapping ─────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('ldap.secMapping')}</h2>
        {mappings.length === 0 && (
          <p className="text-sm text-[var(--text-muted)] mb-3">{t('ldap.noMappings')}</p>
        )}
        <div className="flex flex-col gap-2">
          {mappings.map(m => (
            <div key={m.id} className="flex items-center gap-2">
              <input value={m.dn}
                     onChange={e => updMapping(m.id, { dn: e.target.value })}
                     placeholder={t('ldap.groupDnPlaceholder')}
                     className={`flex-1 font-mono text-xs ${inputCls}`} />
              <select value={m.role}
                      onChange={e => updMapping(m.id, { role: e.target.value as Role })}
                      className={`${inputCls} w-32`}>
                {ROLES.map(r => <option key={r} value={r}>{r}</option>)}
              </select>
              <button onClick={() => rmMapping(m.id)}
                      aria-label="remove"
                      className="p-2 text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
                <X size={16} />
              </button>
            </div>
          ))}
          <button onClick={addMapping}
                  className="flex items-center justify-center gap-1.5 mt-1 px-3 py-2 border border-dashed border-[var(--border-subtle)] rounded-md text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:border-[var(--accent)] transition-colors">
            <Plus size={14} /> {t('ldap.addMapping')}
          </button>
        </div>
      </Block>

      {/* ── Advanced ─────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('ldap.secAdvanced')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('ldap.fieldTimeout')}>
            <input type="number" min={1} max={60}
                   value={form.network_timeout_sec ?? 5}
                   onChange={e => upd('network_timeout_sec', parseInt(e.target.value, 10) || 5)}
                   className={inputCls} />
          </Field>
        </div>
      </Block>

      {/* ── Actions ──────────────────────────────────────────────────────── */}
      <div className="flex gap-2">
        <button disabled={testing} onClick={handleTest}
                className="flex items-center gap-1.5 px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-50 transition-colors">
          <Wifi size={14} /> {testing ? t('ldap.testing') : t('ldap.testConnection')}
        </button>
        <button disabled={saving} onClick={handleSave}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
          {saving ? t('common.saving') : t('common.save')}
        </button>
      </div>
    </div>
  );
}
