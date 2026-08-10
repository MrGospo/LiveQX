import { useTranslation } from 'react-i18next';
import { KeyRound, Copy, X } from 'lucide-react';
import { useToast } from '@/hooks/useToast';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  username: string;
  password: string;
  /** Заголовок: "User created — initial password" / "Password reset — new password". */
  title?:   string;
  hint?:    string;
  onClose:  () => void;
}

// Одноразовая выдача пароля. После закрытия пароль больше не показывается.
// Используется при create user / reset password.
export function InitialPasswordModal({ username, password, title, hint, onClose }: Props) {
  const { t } = useTranslation();
  const toast = useToast();

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(password);
      toast(t('users.passwordCopied'), 'success');
    } catch {
      // older browsers / non-https — silently ignore; user can select+copy manually
    }
  };

  useEscClose(onClose);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <div className="flex items-center gap-2">
            <KeyRound size={18} className="text-[var(--accent)]" />
            <h2 className="text-lg font-semibold text-[var(--text-primary)]">
              {title ?? t('users.initialPasswordTitle')}
            </h2>
          </div>
          <button onClick={onClose}
                  aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 flex flex-col gap-4">
          <p className="text-sm text-[var(--text-muted)] leading-relaxed">
            {hint ?? t('users.initialPasswordHint')}
          </p>

          <div className="flex flex-col gap-1.5">
            <label className="text-xs text-[var(--text-muted)]">{t('users.colUsername')}</label>
            <input value={username} readOnly
                   className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)]" />
          </div>

          <div className="flex flex-col gap-1.5">
            <label className="text-xs text-[var(--text-muted)]">{t('auth.password')}</label>
            <div className="flex gap-2">
              <input value={password} readOnly
                     className="flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)]" />
              <button onClick={copy}
                      className="flex items-center gap-1.5 px-3 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md transition-colors">
                <Copy size={14} /> {t('common.copy')}
              </button>
            </div>
          </div>
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex justify-end">
          <button onClick={onClose}
                  className="px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md transition-colors">
            {t('common.done')}
          </button>
        </div>
      </div>
    </div>
  );
}
