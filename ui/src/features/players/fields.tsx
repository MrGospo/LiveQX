import React from 'react';
import { clsx } from 'clsx';

// Small form pieces for the screens (players) pages. Defined at module level on
// purpose: a component created inside another component's body is a new type on
// every render, so React would rebuild the input under the cursor and it would
// lose focus after each key.

export const inputCls =
  'w-full bg-canvas border border-[var(--border-subtle)] rounded-md px-3 py-2 text-sm text-[var(--text-primary)] ' +
  'outline-none focus-visible:border-[var(--accent)] disabled:opacity-50';

interface FieldProps {
  id: string;
  label: string;
  hint?: React.ReactNode;
  error?: string | null;
  children: React.ReactNode;
}

export function Field({ id, label, hint, error, children }: FieldProps) {
  return (
    <div className="flex flex-col gap-1.5">
      <label htmlFor={id} className="text-sm font-medium text-[var(--text-primary)]">{label}</label>
      {children}
      {error
        ? <p role="alert" className="text-xs text-[var(--danger)] m-0">{error}</p>
        : hint ? <p className="text-xs text-[var(--text-muted)] m-0">{hint}</p> : null}
    </div>
  );
}

interface TextFieldProps {
  id: string;
  label: string;
  value: string;
  onChange: (v: string) => void;
  hint?: React.ReactNode;
  error?: string | null;
  placeholder?: string;
  disabled?: boolean;
  mono?: boolean;
}

export function TextField({ id, label, value, onChange, hint, error, placeholder, disabled, mono }: TextFieldProps) {
  return (
    <Field id={id} label={label} hint={hint} error={error}>
      <input
        id={id}
        value={value}
        placeholder={placeholder}
        disabled={disabled}
        aria-invalid={error ? true : undefined}
        onChange={e => onChange(e.target.value)}
        className={clsx(inputCls, mono && 'font-mono', error && 'border-[var(--danger)]')}
      />
    </Field>
  );
}

interface NumberFieldProps {
  id: string;
  label: string;
  value: number | '';
  onChange: (v: number | '') => void;
  min: number;
  max: number;
  hint?: React.ReactNode;
  error?: string | null;
  disabled?: boolean;
  /** Shown to the right of the field, e.g. "≈ 5 min". */
  note?: string;
}

export function NumberField({ id, label, value, onChange, min, max, hint, error, disabled, note }: NumberFieldProps) {
  return (
    <Field id={id} label={label} hint={hint} error={error}>
      <div className="flex items-center gap-3">
        <input
          id={id}
          type="number"
          inputMode="numeric"
          min={min}
          max={max}
          value={value}
          disabled={disabled}
          aria-invalid={error ? true : undefined}
          onChange={e => {
            const raw = e.target.value;
            onChange(raw === '' ? '' : Number.parseInt(raw, 10));
          }}
          className={clsx(inputCls, 'max-w-[10rem]', error && 'border-[var(--danger)]')}
        />
        {note && <span className="text-xs text-[var(--text-muted)] whitespace-nowrap">{note}</span>}
      </div>
    </Field>
  );
}

interface ToggleProps {
  id: string;
  label: string;
  checked: boolean;
  onChange: (v: boolean) => void;
  hint?: React.ReactNode;
  disabled?: boolean;
}

export function Toggle({ id, label, checked, onChange, hint, disabled }: ToggleProps) {
  return (
    <div className="flex flex-col gap-1.5">
      <div className="flex items-center gap-3">
        <button
          id={id}
          type="button"
          role="switch"
          aria-checked={checked}
          aria-labelledby={`${id}-label`}
          disabled={disabled}
          onClick={() => onChange(!checked)}
          className={clsx(
            'w-9 h-5 rounded-full relative transition-colors outline-none focus-visible:ring-2 ring-[var(--accent)] disabled:opacity-50',
            checked ? 'bg-[var(--accent)]' : 'bg-[var(--border-subtle)]',
          )}
        >
          <span className={clsx('w-3.5 h-3.5 rounded-full bg-white absolute top-[3px] transition-all', checked ? 'left-[18px]' : 'left-[3px]')} />
        </button>
        <span id={`${id}-label`} className="text-sm text-[var(--text-primary)]">{label}</span>
      </div>
      {hint && <p className="text-xs text-[var(--text-muted)] m-0">{hint}</p>}
    </div>
  );
}

export function SectionTitle({ title, hint }: { title: string; hint?: string }) {
  return (
    <div className="mb-4">
      <h2 className="text-base font-semibold text-[var(--text-primary)] m-0">{title}</h2>
      {hint && <p className="text-sm text-[var(--text-muted)] mt-1 mb-0">{hint}</p>}
    </div>
  );
}
