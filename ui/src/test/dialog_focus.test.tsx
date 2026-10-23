/**
 * Keyboard focus in dialogs: it goes in when the dialog opens, stays in while Tab is pressed,
 * and goes back to where it was when the dialog closes. A person who cannot use a mouse
 * depends on all three.
 */
import React from 'react';
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi } from 'vitest';
import { useDialogFocus } from '@/hooks/useDialogFocus';
import { ConfirmModal } from '@/components/ConfirmModal';

// A small dialog that uses the hook the way the real ones do: the ref goes on its outermost element.
function Dialog({ onClose, withField }: { onClose: () => void; withField: boolean }) {
  const ref = useDialogFocus<HTMLDivElement>();
  return (
    <div ref={ref} role="dialog" aria-label="Test dialog">
      <button aria-label="close" onClick={onClose}>x</button>
      {withField && <input aria-label="Name" />}
      <button>Save</button>
      <button onClick={onClose}>Cancel</button>
    </div>
  );
}

function Harness({ withField = true }: { withField?: boolean }) {
  const [open, setOpen] = React.useState(false);
  return (
    <div>
      <button onClick={() => setOpen(true)}>Open</button>
      <button>Behind</button>
      {open && <Dialog withField={withField} onClose={() => setOpen(false)} />}
    </div>
  );
}

describe('useDialogFocus', () => {
  it('moves focus to the first field when the dialog opens', async () => {
    const user = userEvent.setup();
    render(<Harness />);
    await user.click(screen.getByRole('button', { name: 'Open' }));
    expect(screen.getByLabelText('Name')).toHaveFocus();
  });

  it('moves focus to the first action that is not "close" when there is no field', async () => {
    const user = userEvent.setup();
    render(<Harness withField={false} />);
    await user.click(screen.getByRole('button', { name: 'Open' }));
    expect(screen.getByRole('button', { name: 'Save' })).toHaveFocus();
  });

  it('keeps Tab inside the dialog, going round from the last element to the first and back', async () => {
    const user = userEvent.setup();
    render(<Harness />);
    await user.click(screen.getByRole('button', { name: 'Open' }));
    await user.tab(); expect(screen.getByRole('button', { name: 'Save' })).toHaveFocus();
    await user.tab(); expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
    await user.tab(); expect(screen.getByRole('button', { name: 'close' })).toHaveFocus();     // round, not to the page behind
    await user.tab({ shift: true }); expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Behind' })).not.toHaveFocus();
  });

  it('gives focus back to what had it before, when the dialog closes', async () => {
    const user = userEvent.setup();
    render(<Harness />);
    const opener = screen.getByRole('button', { name: 'Open' });
    await user.click(opener);
    await user.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(opener).toHaveFocus();
  });
});

describe('ConfirmModal (the shared confirmation)', () => {
  it('puts focus on its first action, holds Tab inside, and Escape still cancels', async () => {
    const user = userEvent.setup();
    const onCancel = vi.fn();
    render(<ConfirmModal title="Delete it?" message="Sure?" confirmLabel="Delete" danger onConfirm={vi.fn()} onCancel={onCancel} />);
    expect(screen.getByRole('dialog', { name: 'Delete it?' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
    await user.tab(); expect(screen.getByRole('button', { name: 'Delete' })).toHaveFocus();
    await user.tab(); expect(screen.getByRole('button', { name: 'close' })).toHaveFocus();     // round to the first element
    await user.keyboard('{Escape}');
    expect(onCancel).toHaveBeenCalled();
  });
});
