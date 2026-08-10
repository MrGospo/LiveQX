import { useTranslation } from 'react-i18next';
import { AlertTriangle, X } from 'lucide-react';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  title:        string;
  message:      string;
  confirmLabel: string;
  pendingLabel?: string;
  pending?:     boolean;
  danger?:      boolean;
  onConfirm:    () => void;
  onCancel:     () => void;
}

// Простой yes/no модал для not-typed-name confirm'ов
// (reset-password, batch ops и т.п.). Для полностью необратимых
// операций (purge user) — отдельный модал с typed-name защитой.
export function ConfirmModal({
  title, message, confirmLabel, pendingLabel,
  pending, danger, onConfirm, onCancel,
}: Props) {
  const { t } = useTranslation();
  useEscClose(onCancel, !pending);
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <div className="flex items-center gap-2">
            {danger && <AlertTriangle size={18} className="text-[var(--warning)]" />}
            <h2 className="text-lg font-semibold text-[var(--text-primary)]">{title}</h2>
          </div>
          <button onClick={onCancel}
                  disabled={pending}
                  aria-label="close"
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors">
            <X size={18} />
          </button>
        </div>

        <div className="p-5">
          <p className="text-sm text-[var(--text-primary)] leading-relaxed">{message}</p>
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
                  disabled={pending}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={onConfirm}
                  disabled={pending}
                  className={`px-4 py-2 text-white text-sm rounded-md transition-colors disabled:opacity-50
                              ${danger
                                ? 'bg-[var(--danger)] hover:bg-[var(--danger)]/85'
                                : 'bg-[var(--accent)] hover:bg-[var(--accent-hover)]'}`}>
            {pending ? (pendingLabel ?? confirmLabel) : confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
