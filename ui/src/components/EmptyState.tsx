import React from 'react';
import { clsx } from 'clsx';
import { LucideIcon } from 'lucide-react';

interface EmptyStateProps {
  Icon: LucideIcon;
  title: string;
  description?: string;
  action?: React.ReactNode;
  className?: string;
}

export function EmptyState({ Icon, title, description, action, className }: EmptyStateProps) {
  return (
    <div className={clsx('flex flex-col items-center justify-center gap-3 py-12 px-6 text-center', className)}>
      <Icon className="text-[var(--border-subtle)] mb-1" size={40} strokeWidth={1.5} />
      <p className="text-lg font-semibold text-[var(--text-primary)] m-0">{title}</p>
      {description && (
        <p className="text-sm text-[var(--text-muted)] max-w-xs m-0">{description}</p>
      )}
      {action && <div className="mt-2">{action}</div>}
    </div>
  );
}
