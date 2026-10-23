import React from 'react';

// Keyboard focus for a dialog, as a person who cannot use a mouse needs it:
//   * on opening, focus moves INTO the dialog (to its first field, or else its first action
//     other than "close"), so typing or Enter works at once;
//   * Tab and Shift+Tab stay inside the dialog instead of wandering over the page behind it;
//   * on closing, focus goes back to what had it before, so the person is not thrown to the top.
// Put the returned ref on the dialog's outermost element.

const FOCUSABLE =
  'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';
const FIELD = 'input:not([disabled]):not([type=checkbox]):not([type=radio]):not([type=hidden]), select:not([disabled]), textarea:not([disabled])';

export function useDialogFocus<T extends HTMLElement>() {
  const ref = React.useRef<T>(null);

  React.useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const before = document.activeElement as HTMLElement | null;

    el.tabIndex = -1;                                       // so the dialog itself can hold focus if it has nothing else
    const start = el.querySelector<HTMLElement>(FIELD)
      ?? el.querySelector<HTMLElement>('input[type=radio]:checked')
      ?? el.querySelector<HTMLElement>('button:not([aria-label="close"]):not([disabled])')
      ?? el;
    start.focus();

    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Tab') return;
      const items = Array.from(el.querySelectorAll<HTMLElement>(FOCUSABLE));
      if (items.length === 0) { e.preventDefault(); el.focus(); return; }
      const first = items[0];
      const last = items[items.length - 1];
      const active = document.activeElement;
      if (!el.contains(active)) { e.preventDefault(); first.focus(); return; }
      if (e.shiftKey && (active === first || active === el)) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && active === last) { e.preventDefault(); first.focus(); }
    };
    document.addEventListener('keydown', onKey);

    return () => {
      document.removeEventListener('keydown', onKey);
      if (before && document.contains(before)) before.focus();
    };
  }, []);

  return ref;
}
