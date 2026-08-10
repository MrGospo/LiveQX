import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Eye, EyeOff, Lock } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useAuthStore } from '@/stores/auth';
import { loginSchema, changePasswordSchema } from '@/schemas';
import type { LoginFormValues, ChangePasswordFormValues } from '@/schemas';

// ─── Login Page ───────────────────────────────────────────────────────────────
export default function LoginPage() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const setTokens = useAuthStore(s => s.setTokens);
  const setUser   = useAuthStore(s => s.setUser);

  const [showPwd, setShowPwd] = React.useState(false);
  const [serverError, setServerError] = React.useState<string | null>(null);

  const { register, handleSubmit, formState: { errors, isSubmitting } } = useForm<LoginFormValues>({
    resolver: zodResolver(loginSchema),
    defaultValues: { username: '', password: '' },
  });

  const onSubmit = async (values: LoginFormValues) => {
    setServerError(null);
    try {
      const res = await fetch('/api/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(values),
      });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ error: 'unknown_error' }));
        if (res.status === 401) setServerError(t('auth.invalidCredentials'));
        else if (res.status === 423) setServerError(t('auth.locked', { count: 5, duration: '15min' }));
        else if (res.status === 503) setServerError(t('auth.ldapUnavailable'));
        else setServerError(err.detail ?? t('auth.serverError'));
        return;
      }
      const data = await res.json();
      setTokens(data.access_token, data.refresh_token, data.expires_in);
      setUser(data.user);
      navigate('/dashboard', { replace: true });
    } catch {
      setServerError(t('auth.serverError'));
    }
  };

  return (
    <div className="min-h-screen flex items-center justify-center bg-canvas p-5">
      <div className="w-full max-w-md">
        {/* Logo */}
        <div className="text-center mb-8">
          <img src="/logo.svg" alt="LiveQX" className="w-16 h-16 mx-auto mb-3" />
          <p className="text-xl font-bold tracking-widest uppercase text-[var(--text-primary)]">LiveQX</p>
        </div>

        {/* Card */}
        <div className="bg-surface border border-[var(--border-subtle)] rounded-xl p-8">
          <h1 className="text-xl font-semibold text-[var(--text-primary)] text-center mb-6">{t('auth.login')}</h1>

          {serverError && (
            <div className="mb-5 flex items-start gap-2.5 rounded-lg bg-[var(--danger)]/10 border border-[var(--danger)]/30 px-4 py-3 text-sm text-[var(--text-primary)]">
              {serverError}
            </div>
          )}

          <form onSubmit={handleSubmit(onSubmit)} className="flex flex-col gap-5">
            <div className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t('auth.username')}</label>
              <input
                {...register('username')}
                autoComplete="username"
                className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-base text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors"
              />
              {errors.username && <p className="text-sm text-[var(--danger)]">{errors.username.message}</p>}
            </div>

            <div className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t('auth.password')}</label>
              <div className="relative">
                <input
                  {...register('password')}
                  type={showPwd ? 'text' : 'password'}
                  autoComplete="current-password"
                  className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 pr-10 text-base text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors"
                />
                <button
                  type="button"
                  onClick={() => setShowPwd(v => !v)}
                  aria-pressed={showPwd}
                  aria-label={showPwd ? t('auth.hidePassword') : t('auth.showPassword')}
                  className="absolute right-3 top-1/2 -translate-y-1/2 text-[var(--text-muted)] hover:text-[var(--text-primary)]"
                >
                  {showPwd ? <EyeOff size={16} /> : <Eye size={16} />}
                </button>
              </div>
              {errors.password && <p className="text-sm text-[var(--danger)]">{errors.password.message}</p>}
            </div>

            <button
              type="submit"
              disabled={isSubmitting}
              className="w-full h-11 bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50 text-white font-medium rounded-md transition-colors text-base mt-1"
            >
              {isSubmitting ? t('common.loading') : t('auth.login')}
            </button>
          </form>

          <p className="text-sm text-[var(--text-muted)] text-center mt-5">{t('auth.contactAdmin')}</p>
        </div>

        {import.meta.env.DEV && (
          <p className="text-center mt-3 text-xs text-[var(--text-muted)]">Dev: admin / any password</p>
        )}
      </div>
    </div>
  );
}

// ─── Force Change Password Modal ──────────────────────────────────────────────
export function ForceChangeModal() {
  const { t } = useTranslation();
  const setMustChange = useAuthStore(s => s.setMustChangePassword);
  const token         = useAuthStore(s => s.accessToken);

  const { register, handleSubmit, formState: { errors, isSubmitting } } = useForm<ChangePasswordFormValues>({
    resolver: zodResolver(changePasswordSchema),
  });

  const [serverError, setServerError] = React.useState<string | null>(null);

  const onSubmit = async (values: ChangePasswordFormValues) => {
    setServerError(null);
    try {
      const res = await fetch('/api/auth/me/password', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
        body: JSON.stringify({ current_password: values.current_password, new_password: values.new_password }),
      });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ error: 'unknown' }));
        setServerError(err.detail ?? err.error);
        return;
      }
      setMustChange(false);
    } catch {
      setServerError(t('auth.serverError'));
    }
  };

  return (
    <div className="fixed inset-0 z-[1000] bg-black/65 flex items-center justify-center p-5">
      <div className="w-full max-w-md bg-surface border border-[var(--border-subtle)] rounded-xl p-8">
        <div className="text-center mb-6">
          <Lock size={32} className="text-[var(--warning)] mx-auto mb-3" />
          <h2 className="text-xl font-bold text-[var(--text-primary)]">{t('auth.changePassword.title')}</h2>
          <p className="text-sm text-[var(--text-muted)] mt-2">{t('auth.changePasswordHint')}</p>
        </div>

        {serverError && (
          <div className="mb-4 text-sm text-[var(--danger)] bg-[var(--danger)]/10 border border-[var(--danger)]/30 rounded-lg px-4 py-3">{serverError}</div>
        )}

        <form onSubmit={handleSubmit(onSubmit)} className="flex flex-col gap-4">
          {(['current_password', 'new_password', 'confirm_password'] as const).map(field => (
            <div key={field} className="flex flex-col gap-1.5">
              <label className="text-sm font-medium text-[var(--text-primary)]">{t(`auth.${field === 'current_password' ? 'currentPassword' : field === 'new_password' ? 'newPassword' : 'confirmPassword'}`)}</label>
              <input {...register(field)} type="password"
                className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-base text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors" />
              {errors[field] && <p className="text-sm text-[var(--danger)]">{errors[field]?.message}</p>}
            </div>
          ))}

          <button type="submit" disabled={isSubmitting}
            className="w-full h-11 bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50 text-white font-medium rounded-md transition-colors mt-2">
            {isSubmitting ? t('common.loading') : t('auth.changeAndContinue')}
          </button>
        </form>
      </div>
    </div>
  );
}
