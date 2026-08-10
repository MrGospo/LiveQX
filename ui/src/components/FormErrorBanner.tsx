import { AlertTriangle } from 'lucide-react';
import type { FieldErrors } from 'react-hook-form';

// Lists every field-level error from react-hook-form.
// Conditional fields (rendered only when watch() matches) hide their per-field
// error message — this banner surfaces them so a "silent submit" can never
// happen unnoticed.
export function FormErrorBanner<T extends Record<string, unknown>>({ errors }: { errors: FieldErrors<T> }) {
  const entries = Object.entries(errors) as Array<[string, { message?: string } | undefined]>;
  const visible = entries.filter(([, e]) => e?.message);
  if (visible.length === 0) return null;
  return (
    <div className="flex items-start gap-3 p-3 bg-[var(--danger)]/10 border border-[var(--danger)]/30 rounded-lg text-sm">
      <AlertTriangle size={15} className="text-[var(--danger)] mt-0.5 flex-shrink-0" />
      <div className="flex-1">
        <p className="font-medium text-[var(--text-primary)] mb-1">Form has errors:</p>
        <ul className="text-xs text-[var(--text-muted)] space-y-0.5">
          {visible.map(([field, err]) => (
            <li key={field}><code className="font-mono text-[var(--danger)]">{field}</code>: {err?.message}</li>
          ))}
        </ul>
      </div>
    </div>
  );
}
