import React from 'react';
import { useTranslation } from 'react-i18next';
import { Folder, FileText, ChevronUp, X, Check } from 'lucide-react';
import { useBrowseFolder } from '@/api/queries/system';
import { useEscClose } from '@/hooks/useEscClose';

interface Props {
  initialPath?: string;
  // Optional case-insensitive extension filter (e.g. ['.png','.jpg','.jpeg']).
  // Files outside the filter are still shown but greyed out / non-selectable.
  acceptExtensions?: string[];
  onSelect: (path: string) => void;
  onCancel: () => void;
}

// File picker backed by GET /api/system/browse?include_files=true. Single
// click navigates into a directory or selects a file (button at bottom
// commits the current path/selection). Reuses the folder picker layout
// so the affordance is familiar.
export function FilePickerModal({
  initialPath = '/',
  acceptExtensions,
  onSelect,
  onCancel,
}: Props) {
  const { t } = useTranslation();
  const [path, setPath] = React.useState(initialPath);
  const [selectedFile, setSelectedFile] = React.useState<string | null>(null);
  const { data, isLoading, error } = useBrowseFolder(path, true);

  const matchesExt = (name: string) => {
    if (!acceptExtensions || acceptExtensions.length === 0) return true;
    const lower = name.toLowerCase();
    return acceptExtensions.some(ext => lower.endsWith(ext.toLowerCase()));
  };

  useEscClose(onCancel);

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-lg shadow-2xl flex flex-col"
           style={{ maxHeight: '80vh' }}>
        <div className="flex items-center justify-between p-5 border-b border-[var(--border-subtle)]">
          <h2 className="text-lg font-semibold text-[var(--text-primary)]">{t('filePicker.title')}</h2>
          <button onClick={onCancel} className="text-[var(--text-muted)] hover:text-[var(--text-primary)] transition-colors" aria-label="close">
            <X size={18} />
          </button>
        </div>

        <div className="px-5 py-3 border-b border-[var(--border-subtle)] flex items-center gap-2">
          <button onClick={() => { if (data?.parent) { setPath(data.parent); setSelectedFile(null); } }}
                  disabled={!data?.parent}
                  className="p-1.5 rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] disabled:opacity-30 transition-colors"
                  aria-label="parent">
            <ChevronUp size={16} />
          </button>
          <input value={path} onChange={(e) => { setPath(e.target.value); setSelectedFile(null); }}
                 className="flex-1 bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-1.5 text-sm font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />
        </div>

        <div className="flex-1 overflow-y-auto px-2 py-2">
          {isLoading && <p className="text-sm text-[var(--text-muted)] px-3 py-2">{t('common.loading')}</p>}
          {error && <p className="text-sm text-[var(--danger)] px-3 py-2">{(error as Error).message}</p>}
          {data && data.entries.length === 0 && !isLoading && (
            <p className="text-sm text-[var(--text-muted)] px-3 py-2">{t('folderPicker.empty')}</p>
          )}
          {data?.entries.map((e) => {
            const isFile = !e.is_dir;
            const allowed = !isFile || matchesExt(e.name);
            const isSelected = selectedFile === e.full_path;
            return (
              <button key={e.full_path}
                      disabled={isFile && !allowed}
                      onClick={() => {
                        if (e.is_dir) { setPath(e.full_path); setSelectedFile(null); }
                        else if (allowed) setSelectedFile(e.full_path);
                      }}
                      className={`w-full flex items-center gap-2 px-3 py-2 text-left text-sm rounded-md transition-colors ${
                        isSelected ? 'bg-[var(--accent)]/15 text-[var(--accent)]'
                                   : (allowed ? 'text-[var(--text-primary)] hover:bg-surface2'
                                              : 'text-[var(--text-muted)] opacity-40 cursor-not-allowed')
                      }`}>
                {e.is_dir
                  ? <Folder size={14} className="text-[var(--accent)]" />
                  : <FileText size={14} className="text-[var(--text-muted)]" />}
                <span className="truncate">{e.name}</span>
                {isFile && e.size_bytes != null && (
                  <span className="ml-auto text-xs text-[var(--text-muted)] font-mono tabular-nums">
                    {formatSize(e.size_bytes)}
                  </span>
                )}
              </button>
            );
          })}
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex gap-2 justify-end items-center">
          {selectedFile && (
            <span className="flex-1 text-xs font-mono text-[var(--text-muted)] truncate">{selectedFile}</span>
          )}
          <button onClick={onCancel}
                  className="px-4 py-2 border border-[var(--border-subtle)] text-sm text-[var(--text-muted)] rounded-md hover:text-[var(--text-primary)] transition-colors">
            {t('common.cancel')}
          </button>
          <button onClick={() => selectedFile && onSelect(selectedFile)}
                  disabled={!selectedFile}
                  className="flex items-center gap-1.5 px-4 py-2 bg-[var(--accent)] hover:bg-[var(--accent-hover)] text-white text-sm rounded-md disabled:opacity-40 transition-colors">
            <Check size={14} /> {t('filePicker.select')}
          </button>
        </div>
      </div>
    </div>
  );
}

function formatSize(b: number): string {
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(1)} KB`;
  if (b < 1024 * 1024 * 1024) return `${(b / 1024 / 1024).toFixed(1)} MB`;
  return `${(b / 1024 / 1024 / 1024).toFixed(2)} GB`;
}
