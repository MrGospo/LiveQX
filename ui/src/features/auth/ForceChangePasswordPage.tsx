/**
 * ForceChangePasswordPage — /profile/change-password
 * Full-page route, no sidebar, triggered by RequireAuth when must_change_password=true.
 * § 13.2 ui-spec.md
 */
import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { Lock, LogOut } from 'lucide-react';
import { changePasswordSchema } from '@/schemas';
import type { ChangePasswordFormValues } from '@/schemas';
import { useAuthStore } from '@/stores/auth';
import { useToast } from '@/hooks/useToast';
import { api } from '@/api/client';

export default function ForceChangePasswordPage() {
  const { t } = useTranslation();
  const navigate  = useNavigate();
  const toast     = useToast();
  const logout    = useAuthStore(s => s.logout);
  const setMustChange = useAuthStore(s => s.setMustChangePassword);

  const { register, handleSubmit, formState: { errors, isSubmitting } } = useForm<ChangePasswordFormValues>({
    resolver: zodResolver(changePasswordSchema),
  });
  const [serverError, setServerError] = React.useState<string | null>(null);

  const onSubmit = async (values: ChangePasswordFormValues) => {
    setServerError(null);
    try {
      await api.post('/api/auth/me/password', {
        current_password: values.current_password,
        new_password: values.new_password,
      });
      // Optimistic update: clear the flag immediately
      setMustChange(false);
      toast(t('auth.passwordChanged'), 'success');
      navigate('/dashboard', { replace: true });
    } catch (e: unknown) {
      const err = e as { detail?: string; code?: string };
      setServerError(err.detail ?? err.code ?? t('auth.serverError'));
    }
  };

  const handleLogout = () => {
    logout();
    navigate('/login', { replace: true });
  };

  const inputCls = 'bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-base text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)] transition-colors';

  return (
    <div className="min-h-screen bg-canvas flex flex-col">
      {/* Minimal top-bar — only logout */}
      <header className="flex items-center justify-between px-5 h-14 bg-surface border-b border-[var(--border-subtle)]">
        <span className="text-base font-bold tracking-wider text-[var(--text-primary)]">LiveQX</span>
        <button onClick={handleLogout}
          className="flex items-center gap-1.5 text-sm text-[var(--text-muted)] hover:text-[var(--danger)] transition-colors">
          <LogOut size={15} /> {t('auth.logout')}
        </button>
      </header>

      {/* Centered form */}
      <div className="flex-1 flex items-center justify-center p-5">
        <div className="w-full max-w-md">
          <div className="text-center mb-8">
            <div className="inline-flex items-center justify-center w-12 h-12 rounded-xl bg-[var(--warning)]/20 mb-3">
              <Lock size={24} className="text-[var(--warning)]" />
            </div>
            <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('auth.changePassword.title')}</h1>
            <p className="text-sm text-[var(--text-muted)] mt-2">{t('auth.changePasswordHint')}</p>
          </div>

          <div className="bg-surface border border-[var(--border-subtle)] rounded-xl p-8">
            {serverError && (
              <div className="mb-5 p-3 rounded-lg bg-[var(--danger)]/10 border border-[var(--danger)]/30 text-sm text-[var(--text-primary)]">
                {serverError}
              </div>
            )}

            <form onSubmit={handleSubmit(onSubmit)} className="flex flex-col gap-5">
              {(
                [
                  ['current_password', t('auth.currentPassword')],
                  ['new_password',     t('auth.newPassword')],
                  ['confirm_password', t('auth.confirmPassword')],
                ] as const
              ).map(([field, label]) => (
                <div key={field} className="flex flex-col gap-1.5">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{label}</label>
                  <input {...register(field)} type="password" className={inputCls} />
                  {errors[field] && (
                    <p className="text-xs text-[var(--danger)]">{errors[field]?.message}</p>
                  )}
                </div>
              ))}

              <p className="text-xs text-[var(--text-muted)]">{t('auth.passwordPolicy')}</p>

              <button type="submit" disabled={isSubmitting}
                className="w-full h-11 bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-50 text-white font-medium rounded-md transition-colors">
                {isSubmitting ? t('common.loading') : t('auth.changeAndContinue')}
              </button>
            </form>
          </div>
        </div>
      </div>
    </div>
  );
}
