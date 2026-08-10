import React from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle, X, Trash2 } from 'lucide-react';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  channelName: string;
  pending?:    boolean;
  onConfirm:   () => void;
  onCancel:    () => void;
}

// Type-name-to-confirm modal for irreversible channel deletion.
// Backend wipes config.json + state.db + cache; logs are kept on disk.
export function DeleteChannelModal({ channelName, pending, onConfirm, onCancel }: Props) {
  const { t } = useTranslation();
  const [typed, setTyped] = React.useState('');
  const matches = typed.trim() === channelName;

  useEscClose(onCancel, !pending);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-md shadow-2xl flex flex-col">
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <div className="flex items-center gap-2">
            <AlertTriangle size={18} className="text-[var(--danger)]" />
            <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('channels.delete')}</h2>
          </div>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>

        <div className="p-5 flex flex-col gap-3">
          <p className="text-sm text-[var(--text-muted)]">{t('channels.deleteWarning')}</p>
          <label className="text-sm text-[var(--text-primary)] flex flex-col gap-1.5">
            {t('channels.deleteConfirm')}
            <input autoFocus value={typed} onChange={(e) => setTyped(e.target.value)}
                   placeholder={channelName}
                   className="bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
          </label>
          {typed && !matches && (
            <p className="text-xs text-[var(--danger)]">{t('channels.deleteConfirmMismatch')}</p>
          )}
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={onConfirm} disabled={!matches || pending}
                  className="flex items-center gap-1.5 px-4 py-2 bg-[var(--danger)] hover:opacity-90 text-white text-sm rounded-md disabled:opacity-40 transition-colors">
            <Trash2 size={14} /> {t('channels.delete')}
          </button>
        </div>
      </div>
    </div>
  );
}
