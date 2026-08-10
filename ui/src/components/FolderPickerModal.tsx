import React from 'react';
import { useTranslation } from 'react-i18next';
import { Folder, ChevronUp, X, Check } from 'lucide-react';
import { useBrowseFolder } from '@/api/queries/system';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  initialPath?: string;
  onSelect:    (path: string) => void;
  onCancel:    () => void;
}

// Filesystem folder picker backed by GET /api/system/browse.
// Admin-only on the backend — non-admin sessions get 403 and the modal
// surfaces an empty error state.
export function FolderPickerModal({ initialPath = '/', onSelect, onCancel }: Props) {
  const { t } = useTranslation();
  const [path, setPath] = React.useState(initialPath);
  const { data, isLoading, error } = useBrowseFolder(path, false);

  useEscClose(onCancel);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-lg shadow-2xl flex flex-col"
           style={{ maxHeight: '80vh' }}>
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('folderPicker.title')}</h2>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>

        <div className="px-5 py-3 border-b border-[var(--border-subtle)] flex items-center gap-2">
          <button onClick={() => data?.parent && setPath(data.parent)}
                  disabled={!data?.parent}
                  className="p-1.5 rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors"
                  aria-label="parent">
            <ChevronUp size={16} />
          </button>
          <input value={path} onChange={(e) => setPath(e.target.value)}
                 className="flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
        </div>

        <div className="flex-1 overflow-y-auto px-2 py-2">
          {isLoading && <p className="text-sm text-[var(--text-muted)] px-3 py-2">{t('common.loading')}</p>}
          {error && <p className="text-sm text-[var(--danger)] px-3 py-2">{(error as Error).message}</p>}
          {data && data.entries.length === 0 && !isLoading && (
            <p className="text-sm text-[var(--text-muted)] px-3 py-2">{t('folderPicker.empty')}</p>
          )}
          {data?.entries.map((e) => (
            <button key={e.full_path}
                    onDoubleClick={() => setPath(e.full_path)}
                    onClick={() => setPath(e.full_path)}
                    className="w-full flex items-center gap-2 px-3 py-2 text-left text-sm rounded-md hover:bg-surface2 text-[var(--text-primary)] transition-colors">
              <Folder size={14} className="text-[var(--accent)]" />
              <span className="truncate">{e.name}</span>
            </button>
          ))}
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end">
          <button onClick={onCancel}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={() => onSelect(path)}
                  className="flex items-center gap-1.5 px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md transition-colors">
            <Check size={14} /> {t('folderPicker.select')}
          </button>
        </div>
      </div>
    </div>
  );
}
