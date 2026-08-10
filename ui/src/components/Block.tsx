/**
 * Block — reference container per § 5 of ui-spec.md.
 * bg-surface, border-subtle, rounded-xl, no shadow.
 */
import React from 'react';
import { clsx } from 'clsx';

interface BlockProps {
  children: React.ReactNode;
  className?: string;
  padding?: string;  // override Tailwind padding class
  style?: React.CSSProperties;
  /** Optional utility icon area — rendered top-right */
  action?: React.ReactNode;
  onClick?: () => void;
}

export function Block({ children, className, padding = 'p-7', style, action, onClick }: BlockProps) {
  return (
    <div
      onClick={onClick}
      style={style}
      className={clsx(
        'relative rounded-xl border bg-surface',
        'border-[var(--border-subtle)]',
        padding,
        onClick && 'cursor-pointer transition-colors hover:bg-surface2',
        className,
      )}
    >
      {action && (
        <div className="absolute top-4 right-4 opacity-70 hover:opacity-100 transition-opacity">
          {action}
        </div>
      )}
      {children}
    </div>
  );
}
