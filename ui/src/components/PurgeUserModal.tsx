import React from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle, X, Trash2 } from 'lucide-react';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  username: string;
  onConfirm: () => void;
  onCancel:  () => void;
  pending?:  boolean;
}

// Two-step confirm: пользователь должен вручную набрать username
// удаляемой учётки. Кнопка "Удалить" остаётся disabled пока ввод
// не совпадает буква в букву — защита от случайного клика.
export function PurgeUserModal({ username, onConfirm, onCancel, pending }: Props) {
  const { t } = useTranslation();
  const [typed, setTyped] = React.useState('');
  const inputRef = React.useRef<HTMLInputElement>(null);

  React.useEffect(() => { inputRef.current?.focus(); }, []);

  const matches = typed.trim() === username;

  useEscClose(onCancel, !pending);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <div className="flex items-center gap-2">
            <AlertTriangle size={18} className="text-[var(--danger)]" />
            <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('users.purgeTitle')}</h2>
          </div>
          <button onClick={onCancel}
                  disabled={pending}
                  className="text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors"
                  aria-label="close">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 flex flex-col gap-4">
          <p className="text-sm text-[var(--text-primary)] leading-relaxed">
            {t('users.purgeWarn', { user: username })}
          </p>

          <div className="flex flex-col gap-1.5">
            <label className="text-xs text-[var(--text-muted)]">
              {t('users.purgeTypeName', { user: username })}
            </label>
            <input ref={inputRef}
                   value={typed}
                   onChange={(e) => setTyped(e.target.value)}
                   onKeyDown={(e) => { if (e.key === 'Enter' && matches && !pending) onConfirm(); }}
                   disabled={pending}
                   placeholder={username}
                   autoComplete="off"
                   spellCheck={false}
                   className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--danger)] disabled:opacity-50" />
          </div>
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
                  disabled={pending}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={onConfirm}
                  disabled={!matches || pending}
                  className="flex items-center gap-1.5 px-4 py-2 bg-[var(--danger)] hover:bg-[var(--danger)]/85 disabled:bg-[var(--danger)]/30 disabled:cursor-not-allowed text-white text-sm rounded-md transition-colors">
            <Trash2 size={14} /> {pending ? t('users.purging') : t('users.purge')}
          </button>
        </div>
      </div>
    </div>
  );
}
