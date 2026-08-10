import React from 'react';
import { MoreVertical } from 'lucide-react';

export interface RowAction {
  key:        string;
  label:      string;
  icon?:      React.ReactNode;
  onClick:    () => void;
  danger?:    boolean;
  disabled?:  boolean;
}

interface Props {
  items:      RowAction[];
  ariaLabel?: string;
}

// Kebab-меню для табличных строк. Popover закрывается на click outside,
// Esc и при выборе пункта. Position: absolute вправо от кнопки —
// рассчитан, что таблица в Block без overflow:hidden.
export function RowActionsMenu({ items, ariaLabel = 'row actions' }: Props) {
  const [open, setOpen] = React.useState(false);
  const ref = React.useRef<HTMLDivElement>(null);

  React.useEffect(() => {
    if (!open) return;
    const onDoc = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setOpen(false); };
    document.addEventListener('mousedown', onDoc);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDoc);
      document.removeEventListener('keydown', onKey);
    };
  }, [open]);

  const visible = items.filter((it) => !it.disabled);
  if (visible.length === 0) return null;

  return (
    <div ref={ref} className="relative inline-flex">
      <button onClick={() => setOpen((v) => !v)}
              aria-label={ariaLabel}
              aria-haspopup="menu"
              aria-expanded={open}
              className="p-1.5 rounded text-[var(--text-muted)] hover:text-[var(--text-primary)] hover:bg-surface2 transition-colors">
        <MoreVertical size={16} />
      </button>

      {open && (
        <div role="menu"
             className="absolute right-0 top-full mt-1 z-30 min-w-[180px] py-1
                        bg-surface border border-[var(--border-subtle)] rounded-lg shadow-2xl">
          {visible.map((it) => (
            <button key={it.key}
                    role="menuitem"
                    onClick={() => { setOpen(false); it.onClick(); }}
                    className={`w-full flex items-center gap-2 px-3 py-2 text-left text-sm
                                hover:bg-surface2 transition-colors
                                ${it.danger ? 'text-[var(--danger)]' : 'text-[var(--text-primary)]'}`}>
              {it.icon && <span className="opacity-80">{it.icon}</span>}
              <span>{it.label}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
