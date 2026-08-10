import React from 'react';
import { useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useForm } from 'react-hook-form';
import { zodResolver } from '@hookform/resolvers/zod';
import { ChevronLeft, Copy } from 'lucide-react';
import { useCreateUser } from '@/api/queries/auth';
import { Block } from '@/components/Block';
import { useToast } from '@/hooks/useToast';
import { createUserSchema } from '@/schemas';
import type { CreateUserFormValues } from '@/schemas';

export default function CreateUserPage() {
  const { t }      = useTranslation();
  const navigate   = useNavigate();
  const toast      = useToast();
  const { mutateAsync: create, isPending } = useCreateUser();
  const [issuedPwd, setIssuedPwd] = React.useState<string | null>(null);
  const [createdId, setCreatedId] = React.useState<number | null>(null);

  const { register, handleSubmit, watch, formState: { errors } } = useForm<CreateUserFormValues>({
    resolver: zodResolver(createUserSchema),
    defaultValues: {
      username: '',
      email: '',
      role: 'viewer',
      source: 'local',
      password: '',
      must_change_password: true,
    },
  });

  const source = watch('source');
  const isLdap = source === 'ldap';

  const onSubmit = async (values: CreateUserFormValues) => {
    try {
      const body: Record<string, unknown> = {
        username: values.username,
        role: values.role,
        source: values.source,
        must_change_password: values.must_change_password,
      };
      if (values.email) body.email = values.email;
      if (!isLdap && values.password) body.password = values.password;

      const r = await create(body);
      setCreatedId(r.id);
      if (r.initial_password) {
        setIssuedPwd(r.initial_password);
      } else {
        toast(t('users.createSucceeded', { user: values.username }), 'success');
        navigate(`/settings/users/${r.id}`);
      }
    } catch (e: unknown) {
      toast((e as Error).message, 'danger');
    }
  };

  const onCopyPwd = async () => {
    if (!issuedPwd) return;
    try {
      await navigator.clipboard.writeText(issuedPwd);
      toast(t('users.passwordCopied'), 'success');
    } catch { /* ignore clipboard failure */ }
  };

  return (
    <div className="flex flex-col h-full">
      <div className="sticky top-0 z-10 bg-surface border-b border-[var(--border-subtle)] px-7 pt-4 pb-4">
        <button onClick={() => navigate('/settings/users')}
          className="flex items-center gap-1 text-sm text-[var(--text-muted)] hover:text-[var(--text-primary)] mb-3 transition-colors">
          <ChevronLeft size={15} /> {t('users.title')}
        </button>
        <h1 className="text-2xl font-bold text-[var(--text-primary)]">{t('users.create')}</h1>
      </div>

      <div className="flex-1 overflow-y-auto p-7">
        {issuedPwd ? (
          <Block>
            <h2 className="text-base font-semibold text-[var(--text-primary)] mb-2">{t('users.initialPasswordTitle')}</h2>
            <p className="text-sm text-[var(--text-muted)] mb-4">{t('users.initialPasswordHint')}</p>
            <div className="flex items-center gap-3 mb-5 max-w-lg">
              <code className="flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] break-all">
                {issuedPwd}
              </code>
              <button onClick={onCopyPwd}
                className="flex items-center gap-1.5 px-3 py-2 text-sm border border-[var(--border-subtle)] hover:border-[var(--accent)] text-[var(--text-primary)] rounded-md transition-colors">
                <Copy size={14} /> {t('common.copy')}
              </button>
            </div>
            <div className="flex gap-2">
              <button onClick={() => createdId && navigate(`/settings/users/${createdId}`)}
                className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md transition-colors">
                {t('users.openUser')}
              </button>
              <button onClick={() => navigate('/settings/users')}
                className="px-4 py-2 border border-[var(--border-subtle)] text-[var(--text-primary)] text-sm rounded-md hover:bg-surface2 transition-colors">
                {t('common.done')}
              </button>
            </div>
          </Block>
        ) : (
          <form onSubmit={handleSubmit(onSubmit)} className="max-w-2xl">
            <Block>
              <div className="grid grid-cols-2 gap-4 mb-4">
                <div className="flex flex-col gap-1.5">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.colUsername')}</label>
                  <input {...register('username')} autoFocus
                    className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
                  {errors.username && <p className="text-xs text-[var(--danger)]">{errors.username.message}</p>}
                </div>
                <div className="flex flex-col gap-1.5">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.colEmail')}</label>
                  <input {...register('email')} type="email"
                    className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
                  {errors.email && <p className="text-xs text-[var(--danger)]">{errors.email.message}</p>}
                </div>
                <div className="flex flex-col gap-1.5">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.role')}</label>
                  <select {...register('role')}
                    className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
                    <option value="viewer">{t('users.roleViewer')}</option>
                    <option value="operator">{t('users.roleOperator')}</option>
                    <option value="admin">{t('users.roleAdmin')}</option>
                  </select>
                </div>
                <div className="flex flex-col gap-1.5">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.colSource')}</label>
                  <select {...register('source')}
                    className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)]">
                    <option value="local">local</option>
                    <option value="ldap">ldap</option>
                  </select>
                </div>
              </div>

              {!isLdap && (
                <div className="flex flex-col gap-1.5 mb-4 max-w-md">
                  <label className="text-sm font-medium text-[var(--text-primary)]">{t('users.passwordOptional')}</label>
                  <input {...register('password')} type="text" autoComplete="new-password"
                    placeholder={t('users.passwordAutoHint')}
                    className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
                  {errors.password && <p className="text-xs text-[var(--danger)]">{errors.password.message}</p>}
                </div>
              )}

              <label className="flex items-center gap-2 mb-5 cursor-pointer text-sm text-[var(--text-primary)]">
                <input type="checkbox" {...register('must_change_password')}
                  className="w-4 h-4 accent-[var(--accent)]" />
                {t('users.mustChangePassword')}
              </label>

              <div className="flex gap-2">
                <button type="submit" disabled={isPending}
                  className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-50 transition-colors">
                  {isPending ? t('common.saving') : t('users.create')}
                </button>
                <button type="button" onClick={() => navigate('/settings/users')}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-[var(--text-primary)] text-sm rounded-md hover:bg-surface2 transition-colors">
                  {t('common.cancel')}
                </button>
              </div>
            </Block>
          </form>
        )}
      </div>
    </div>
  );
}
