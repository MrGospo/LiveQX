/**
 * Objects and groups page. The server is replaced at the hooks; what is checked
 * is what the administrator sees and what the page asks the server to do.
 */
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { PlayerObject } from '@/api/queries/players';

const toast = vi.fn();
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role: 'admin', user: { id: 1, username: 'admin', role: 'admin' } })),
}));
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({ addToast: toast })),
}));

const treeState: { data?: PlayerObject[]; isLoading: boolean; isError: boolean } = { isLoading: false, isError: false };
const ops = {
  createObject: vi.fn(), renameObject: vi.fn(), deleteObject: vi.fn(),
  createGroup: vi.fn(), renameGroup: vi.fn(), deleteGroup: vi.fn(),
};

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  const wrap = (fn: (...a: never[]) => unknown) => ({ mutateAsync: fn, isPending: false });
  return {
    ...real,
    usePlayerTree: () => treeState,
    usePlayerTreeMutations: () => ({
      createObject: wrap(ops.createObject), renameObject: wrap(ops.renameObject), deleteObject: wrap(ops.deleteObject),
      createGroup: wrap(ops.createGroup), renameGroup: wrap(ops.renameGroup), deleteGroup: wrap(ops.deleteGroup),
    }),
  };
});

import ObjectsPage from '@/features/players/ObjectsPage';

const tree = (): PlayerObject[] => [
  { id: 1, name: 'Shop floor', created_at: 1, groups: [
    { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 3 },
    { id: 12, object_id: 1, name: 'Window', created_at: 1, players: 0 },
  ] },
  { id: 2, name: 'Warehouse', created_at: 1, groups: [] },
];

const renderPage = () => render(<MemoryRouter><ObjectsPage /></MemoryRouter>);

beforeEach(() => {
  toast.mockClear();
  Object.values(ops).forEach(f => f.mockReset().mockResolvedValue({}));
  treeState.data = tree(); treeState.isLoading = false; treeState.isError = false;
});

describe('ObjectsPage', () => {
  it('shows each object with its groups and how many screens each group holds', () => {
    renderPage();
    expect(screen.getByRole('heading', { name: 'Shop floor' })).toBeInTheDocument();
    expect(screen.getByText('Tills')).toBeInTheDocument();
    expect(screen.getByText('3 screens')).toBeInTheDocument();
    expect(screen.getByText('0 screens')).toBeInTheDocument();
    expect(screen.getByText(/has no groups yet/)).toBeInTheDocument();
  });

  it('says so when there is nothing yet', () => {
    treeState.data = [];
    renderPage();
    expect(screen.getByText('No objects yet')).toBeInTheDocument();
  });

  it('shows a plain failure message when the tree cannot be loaded', () => {
    treeState.data = undefined; treeState.isError = true;
    renderPage();
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load objects and groups.');
  });

  it('refuses up front to delete what has something inside, and explains why', () => {
    renderPage();
    expect(screen.getByRole('button', { name: /Object "Shop floor" cannot be deleted/ })).toBeDisabled();
    expect(screen.getByRole('button', { name: /Group "Tills" cannot be deleted/ })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Delete group "Window"' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Delete object "Warehouse"' })).toBeEnabled();
  });

  it('creates an object with the trimmed name', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Add object' }));
    await user.type(screen.getByLabelText('Object name'), '  Office  ');
    await user.click(screen.getByRole('button', { name: 'Create' }));
    expect(ops.createObject).toHaveBeenCalledWith('Office');
    expect(toast).toHaveBeenCalledWith('Object created', 'success');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('does not send an empty name and shows why', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Add object' }));
    await user.click(screen.getByRole('button', { name: 'Create' }));
    expect(ops.createObject).not.toHaveBeenCalled();
    expect(screen.getByRole('alert')).toHaveTextContent('Enter a name');
  });

  it('counts the name length in bytes, as the server does', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Add object' }));
    // 61 two-byte letters: 61 characters but 122 bytes
    await user.type(screen.getByLabelText('Object name'), 'ж'.repeat(61));
    await user.click(screen.getByRole('button', { name: 'Create' }));
    expect(ops.createObject).not.toHaveBeenCalled();
    expect(screen.getByRole('alert')).toHaveTextContent('too long');
  });

  it('keeps the dialog open and shows the refusal on the field when the name is taken', async () => {
    const user = userEvent.setup();
    ops.createGroup.mockRejectedValue(new ApiClientError(409, 'name_taken'));
    renderPage();
    // "Add group" exists once per object; the last one belongs to the Warehouse.
    const addButtons = screen.getAllByRole('button', { name: 'Add group' });
    await user.click(addButtons[addButtons.length - 1]);
    await user.type(screen.getByLabelText('Group name'), 'Tills');
    await user.click(screen.getByRole('button', { name: 'Create' }));
    expect(ops.createGroup).toHaveBeenCalledWith({ objectId: 2, name: 'Tills' });
    expect(screen.getByRole('dialog')).toBeInTheDocument();
    expect(within(screen.getByRole('dialog')).getByRole('alert')).toHaveTextContent('already used');
    expect(toast).not.toHaveBeenCalled();
  });

  it('renames a group, starting from its current name', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Rename group "Window"' }));
    const field = screen.getByLabelText('Group name');
    expect(field).toHaveValue('Window');
    await user.clear(field);
    await user.type(field, 'Display');
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(ops.renameGroup).toHaveBeenCalledWith({ id: 12, name: 'Display' });
  });

  it('asks before deleting and deletes only after the answer', async () => {
    const user = userEvent.setup();
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Delete object "Warehouse"' }));
    expect(ops.deleteObject).not.toHaveBeenCalled();
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(ops.deleteObject).toHaveBeenCalledWith(2);
    expect(toast).toHaveBeenCalledWith('Deleted', 'success');
  });

  it('turns a "not empty" refusal from a stale page into a plain message', async () => {
    const user = userEvent.setup();
    ops.deleteGroup.mockRejectedValue(new ApiClientError(409, 'not_empty'));
    renderPage();
    await user.click(screen.getByRole('button', { name: 'Delete group "Window"' }));
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    expect(toast).toHaveBeenCalledWith(expect.stringContaining('Cannot delete'), 'danger');
  });
});
