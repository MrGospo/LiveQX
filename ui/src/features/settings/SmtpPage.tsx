import React from 'react';
import { useTranslation } from 'react-i18next';
import { Mail } from 'lucide-react';
import { useSmtpConfig, useSaveSmtpConfig, useTestSmtp } from '@/api/queries/auth';
import { Block } from '@/components/Block';
import { SubNav } from '@/components/SubNav';
import { SETTINGS_NAV } from './nav';
import { useToast } from '@/hooks/useToast';
import type { SmtpConfigRequest } from '@/api/types';

const TLS_MODES = [
  { value: 'none',     label: 'none' },
  { value: 'starttls', label: 'starttls' },
  { value: 'tls',      label: 'smtps' },
] as const;

const defaultForm = (): SmtpConfigRequest => ({
  enabled: true,
  server: '',
  port: 587,
  security: 'starttls',
  username: '',
  from_email: '',
  from_name: 'LiveQX',
  timeout_sec: 15,
});

export default function SmtpPage() {
  const { t } = useTranslation();
  const toast = useToast();
  const { data: cfg, isLoading } = useSmtpConfig();
  const { mutateAsync: save, isPending: saving } = useSaveSmtpConfig();
  const { mutateAsync: test, isPending: testing } = useTestSmtp();

  const [form,    setForm]    = React.useState<SmtpConfigRequest>(defaultForm());
  const [pwdSet,  setPwdSet]  = React.useState(false);
  const [pwdMode, setPwdMode] = React.useState<'unchanged' | 'set' | 'clear'>('unchanged');
  const [pwdNew,  setPwdNew]  = React.useState('');
  const [testTo,  setTestTo]  = React.useState('');

  React.useEffect(() => {
    if (!cfg) return;
    const { password_set, ...rest } = cfg;
    setForm(f => ({ ...defaultForm(), ...f, ...rest }));
    setPwdSet(!!password_set);
    setPwdMode('unchanged');
    setPwdNew('');
  }, [cfg]);

  const upd = <K extends keyof SmtpConfigRequest>(k: K, v: SmtpConfigRequest[K]) =>
    setForm(p => ({ ...p, [k]: v }));

  // ── save ──────────────────────────────────────────────────────────────────
  const handleSave = async () => {
    if (form.enabled) {
      if (!(form.server ?? '').trim()) {
        toast(t('smtp.serverRequired'), 'danger');
        return;
      }
      if (!(form.from_email ?? '').trim()) {
        toast(t('smtp.fromEmailRequired'), 'danger');
        return;
      }
      // Если задан username — требуем password (anonymous SMTP с
      // username'ом не имеет смысла; это всегда означает, что
      // оператор забыл ввести пароль).
      if ((form.username ?? '').trim()) {
        const pwdEmpty =
          pwdMode === 'clear' ||
          (pwdMode === 'set' && pwdNew.trim() === '') ||
          (pwdMode === 'unchanged' && !pwdSet);
        if (pwdEmpty) {
          toast(t('smtp.pwdRequired'), 'danger');
          return;
        }
      }
    }

    const payload: SmtpConfigRequest = { ...form };
    if (pwdMode === 'set')   payload.password = pwdNew;
    if (pwdMode === 'clear') payload.password = '';
    // pwdMode === 'unchanged' — поле не отправляем, бэкенд сохранит текущее.

    try {
      await save(payload);
      const wasSet = pwdMode === 'clear' ? false : pwdMode === 'set' ? true : pwdSet;
      setPwdSet(wasSet);
      setPwdMode('unchanged');
      setPwdNew('');
      toast(t('common.savedImmediately'), 'success');
    } catch (err: any) {
      toast(err?.message ?? String(err), 'danger');
    }
  };

  // ── test ──────────────────────────────────────────────────────────────────
  const handleTest = async () => {
    if (!testTo.trim()) return;
    try {
      const r = await test({ to: testTo, use_saved: true });
      toast(r.ok ? t('smtp.sent') : (r.error ?? t('smtp.failed')),
            r.ok ? 'success' : 'danger');
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
        <h1 className="text-xl font-semibold text-[var(--text-primary)]">{t('smtp.title')}</h1>
        <label className="flex items-center gap-2 cursor-pointer">
          <div onClick={() => upd('enabled', !form.enabled)}
               className={`w-9 h-5 rounded-full relative transition-colors ${form.enabled ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]'}`}>
            <div className={`w-3.5 h-3.5 rounded-full bg-white absolute top-0.5 transition-all ${form.enabled ? 'left-4' : 'left-0.5'}`} />
          </div>
          <span className="text-sm text-[var(--text-primary)]">{t('smtp.enabled')}</span>
        </label>
      </div>

      {/* ── Server ────────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('smtp.secServer')}</h2>
        <div className="grid grid-cols-3 gap-4">
          <div className="col-span-2">
            <Field label={t('smtp.server')}>
              <input value={form.server ?? ''} onChange={e => upd('server', e.target.value)}
                     placeholder="smtp.corp.local" className={inputCls} />
            </Field>
          </div>
          <Field label={t('smtp.port')}>
            <input type="number" min={1} max={65535} value={form.port ?? 587}
                   onChange={e => upd('port', parseInt(e.target.value, 10) || 587)}
                   className={inputCls} />
          </Field>
        </div>
        <div className="mt-4 w-40">
          <Field label={t('smtp.tls')}>
            <select value={form.security ?? 'starttls'}
                    onChange={e => upd('security', e.target.value as any)}
                    className={inputCls}>
              {TLS_MODES.map(m => <option key={m.value} value={m.value}>{m.label}</option>)}
            </select>
          </Field>
        </div>
      </Block>

      {/* ── Authentication + From ────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('smtp.secAuth')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('smtp.username')}>
            <input value={form.username ?? ''} onChange={e => upd('username', e.target.value)}
                   placeholder="notify@corp.local" className={inputCls} />
          </Field>

          <div className="flex flex-col gap-1.5">
            <label className="text-sm font-medium text-[var(--text-muted)]">{t('smtp.password')}</label>
            {pwdMode === 'unchanged' ? (
              <div className="flex items-center gap-2">
                <div className={`flex-1 ${inputCls} ${pwdSet ? '' : 'text-[var(--text-muted)] italic'}`}>
                  {pwdSet ? '••••••••••' : t('smtp.pwdNotSet')}
                </div>
                <button onClick={() => setPwdMode('set')}
                        className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                  {t('smtp.pwdEdit')}
                </button>
              </div>
            ) : pwdMode === 'set' ? (
              <div className="flex items-center gap-2">
                <input type="password" autoFocus value={pwdNew}
                       onChange={e => setPwdNew(e.target.value)}
                       placeholder={t('smtp.pwdNew')}
                       className={`flex-1 ${inputCls}`} />
                <button onClick={() => { setPwdMode('clear'); setPwdNew(''); }}
                        className="px-3 py-2 border border-[var(--danger)]/40 text-sm text-[var(--danger)] hover:bg-[var(--danger)]/10 rounded-md transition-colors">
                  {t('smtp.pwdClear')}
                </button>
                <button onClick={() => { setPwdMode('unchanged'); setPwdNew(''); }}
                        className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                  {t('smtp.pwdCancel')}
                </button>
              </div>
            ) : (
              <div className="flex items-center gap-2">
                <div className={`flex-1 ${inputCls} text-[var(--danger)] italic`}>{t('smtp.pwdClear')}</div>
                <button onClick={() => setPwdMode('unchanged')}
                        className="px-3 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md transition-colors">
                  {t('smtp.pwdCancel')}
                </button>
              </div>
            )}
          </div>

          <Field label={t('smtp.fromEmail')}>
            <input type="email" value={form.from_email ?? ''}
                   onChange={e => upd('from_email', e.target.value)}
                   placeholder="notify@corp.local" className={inputCls} />
          </Field>

          <Field label={t('smtp.fromName')}>
            <input value={form.from_name ?? ''}
                   onChange={e => upd('from_name', e.target.value)}
                   placeholder="LiveQX" className={inputCls} />
          </Field>
        </div>
      </Block>

      {/* ── Advanced ─────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('smtp.secAdvanced')}</h2>
        <div className="grid grid-cols-2 gap-4">
          <Field label={t('smtp.timeout')}>
            <input type="number" min={1} max={120} value={form.timeout_sec ?? 15}
                   onChange={e => upd('timeout_sec', parseInt(e.target.value, 10) || 15)}
                   className={inputCls} />
          </Field>
        </div>
      </Block>

      {/* ── Test send ────────────────────────────────────────────────────── */}
      <Block>
        <h2 className="text-base font-semibold text-[var(--text-primary)] mb-4">{t('smtp.testSend')}</h2>
        <div className="flex gap-2 items-end">
          <div className="flex-1">
            <Field label={t('smtp.recipient')}>
              <input type="email" value={testTo} onChange={e => setTestTo(e.target.value)}
                     placeholder="admin@corp.local" className={inputCls} />
            </Field>
          </div>
          <button disabled={testing || !testTo} onClick={handleTest}
                  className="flex items-center gap-1.5 px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] rounded-md disabled:opacity-50 transition-colors">
            <Mail size={14} /> {testing ? t('smtp.sending') : t('smtp.sendTest')}
          </button>
        </div>
      </Block>

      <button disabled={saving} onClick={handleSave}
              className="self-start px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
        {saving ? t('common.saving') : t('common.save')}
      </button>
    </div>
  );
}
