import React from 'react';
import { useTranslation } from 'react-i18next';
import { Copy, Download, KeyRound, Check } from 'lucide-react';
import type { PairingPackage } from '@/api/queries/players';
import { useEscClose } from '@/hooks/useEscClose';
import { absoluteTime } from './format';

// The pairing package of a screen, shown once. The server never gives the token
// again, so the window cannot be closed by accident: Close opens only after the
// package was copied, saved to a file, or the person says they have it.

// A file name safe on any system: the screen's name cut to letters, digits and dashes.
const fileNameFor = (name: string, id: string) => {
  const slug = name.normalize('NFKD').replace(/[^\p{L}\p{N}]+/gu, '-').replace(/^-+|-+$/g, '').slice(0, 40);
  return `liveqx-pairing-${slug || id.slice(0, 8)}.json`;
};

export function PairingPackageDialog({ pkg, playerName, onClose }: {
  pkg: PairingPackage; playerName: string; onClose: () => void;
}) {
  const { t } = useTranslation();
  const [kept, setKept] = React.useState(false);
  const [copyFailed, setCopyFailed] = React.useState(false);
  const text = React.useMemo(() => JSON.stringify(pkg, null, 2), [pkg]);
  const expires = Date.parse(pkg.token_expires_at);
  useEscClose(onClose, kept);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopyFailed(false);
      setKept(true);
    } catch {
      // The clipboard is closed to pages without a secure origin; the text stays selectable.
      setCopyFailed(true);
    }
  };

  const download = () => {
    const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = fileNameFor(playerName, pkg.player_id);
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    setKept(true);
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4">
      <div role="dialog" aria-modal="true" aria-label={t('players.pairing.title', { name: playerName })}
           className="bg-surface border border-[var(--border-subtle)] rounded-xl w-full max-w-2xl max-h-[90vh] overflow-y-auto shadow-2xl flex flex-col">
        <div className="flex items-center gap-2 p-5 border-b border-[var(--border-subtle)]">
          <KeyRound size={18} className="text-[var(--accent)]" />
          <h2 className="text-lg font-semibold text-[var(--text-primary)] m-0">{t('players.pairing.title', { name: playerName })}</h2>
        </div>

        <div className="p-5 flex flex-col gap-4">
          <p role="alert" className="text-sm text-[var(--text-primary)] m-0 rounded-md border border-[var(--warning)] bg-[var(--warning)]/10 p-3">
            {t('players.pairing.once')}
          </p>
          <p className="text-sm text-[var(--text-muted)] m-0">
            {t('players.pairing.howTo')} {Number.isFinite(expires) && t('players.pairing.expires', { when: absoluteTime(Math.floor(expires / 1000)) })}
          </p>

          <textarea readOnly value={text} aria-label={t('players.pairing.package')} rows={10}
                    onFocus={e => e.currentTarget.select()}
                    className="w-full bg-canvas border border-[var(--border-subtle)] rounded-md p-3 text-xs font-mono text-[var(--text-primary)] outline-none focus-visible:border-[var(--accent)]" />

          {copyFailed && <p role="status" className="text-xs text-[var(--warning)] m-0">{t('players.pairing.copyFailed')}</p>}

          <div className="flex flex-wrap gap-2">
            <button type="button" onClick={copy}
                    className="inline-flex items-center gap-1.5 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)] hover:bg-surface2">
              <Copy size={14} /> {t('players.pairing.copy')}
            </button>
            <button type="button" onClick={download}
                    className="inline-flex items-center gap-1.5 px-3 py-2 text-sm border border-[var(--border-subtle)] rounded-md text-[var(--text-primary)] hover:bg-surface2">
              <Download size={14} /> {t('players.pairing.download')}
            </button>
          </div>

          <label className="flex items-center gap-2 text-sm text-[var(--text-primary)]">
            <input type="checkbox" checked={kept} onChange={e => setKept(e.target.checked)} />
            {t('players.pairing.kept')}
          </label>
        </div>

        <div className="p-5 border-t border-[var(--border-subtle)] flex justify-end">
          <button type="button" onClick={onClose} disabled={!kept}
                  className="inline-flex items-center gap-1.5 px-4 py-2 text-sm text-white rounded-md bg-[var(--accent)] hover:bg-[var(--accent-hover)] disabled:opacity-40 disabled:cursor-not-allowed">
            <Check size={14} /> {t('players.pairing.close')}
          </button>
        </div>
      </div>
    </div>
  );
}
