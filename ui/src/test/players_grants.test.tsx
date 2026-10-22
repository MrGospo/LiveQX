/**
 * The tab with a person's rights on screens. The server is replaced at the hooks;
 * what is checked is what the administrator sees, what a save sends (it replaces the
 * whole list, so the draft must be exact), and that a change made by somebody else
 * in the meantime is shown and never overwritten.
 */
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { PlayerObject, UserPlayerGrant, UserPlayerGrants } from '@/api/queries/players';

const toast = vi.fn();
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({ addToast: toast })),
}));

type Q<T> = { data?: T; isLoading: boolean; isError: boolean; refetch: () => Promise<unknown> };
const refetchTree = vi.fn(async () => ({}));
const refetchGrants = vi.fn(async () => ({}));
const treeState: Q<PlayerObject[]> = { isLoading: false, isError: false, refetch: refetchTree };
const grantsState: Q<UserPlayerGrants> = { isLoading: false, isError: false, refetch: refetchGrants };
const save = vi.fn();

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    usePlayerTree: () => treeState,
    useUserPlayerGrants: () => grantsState,
    useSaveUserPlayerGrants: () => ({ mutateAsync: save, isPending: false }),
  };
});

import { PlayerGrantsTab } from '@/features/settings/PlayerGrantsTab';

const tree = (): PlayerObject[] => [
  { id: 1, name: 'Shop', created_at: 1, groups: [
    { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 2 },
    { id: 12, object_id: 1, name: 'Window', created_at: 1, players: 0 },
  ] },
  { id: 2, name: 'Warehouse', created_at: 1, groups: [] },
];

const grant = (scope: 'object' | 'group', id: number, over: Partial<UserPlayerGrant> = {}): UserPlayerGrant => ({
  scope, id, can_command_group: false, can_command_object: false, can_move: false, can_view_audit: false, ...over,
});

const setGrants = (grants: UserPlayerGrant[], revision = 'rev1') => { grantsState.data = { grants, revision }; };

const box = (label: string) => screen.getByRole('checkbox', { name: label }) as HTMLInputElement;
const saveBtn = () => screen.getByRole('button', { name: 'Save' });

beforeEach(() => {
  toast.mockClear(); save.mockReset(); refetchTree.mockClear(); refetchGrants.mockClear();
  treeState.data = tree(); treeState.isLoading = false; treeState.isError = false;
  grantsState.isLoading = false; grantsState.isError = false;
  setGrants([]);
});

describe('PlayerGrantsTab', () => {
  it('shows every object and group with what is given now', () => {
    setGrants([grant('group', 11, { can_move: true }), grant('object', 2, { can_view_audit: true })]);
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(box('Access: Tills')).toBeChecked();
    expect(box('Move: Tills')).toBeChecked();
    expect(box('Access: Window')).not.toBeChecked();
    expect(box('Access: Warehouse')).toBeChecked();
    expect(box('Audit: Warehouse')).toBeChecked();
    expect(box('Access: Shop')).not.toBeChecked();
  });

  it('says plainly that nothing is given when nothing is', () => {
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getByText(/Nothing is given yet/)).toBeInTheDocument();
  });

  it('keeps the marks off until the place has access, and drops them when access is taken away', async () => {
    const user = userEvent.setup();
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(box('Move: Tills')).toBeDisabled();
    await user.click(box('Access: Tills'));
    expect(box('Move: Tills')).toBeEnabled();
    await user.click(box('Move: Tills'));
    await user.click(box('Access: Tills'));                  // access off: the marks go with it
    await user.click(box('Access: Tills'));                  // and on again: from nothing
    expect(box('Move: Tills')).not.toBeChecked();
  });

  it('offers "command an object" only on an object, not on a group', () => {
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getByRole('checkbox', { name: 'Command an object: Shop' })).toBeInTheDocument();
    expect(screen.queryByRole('checkbox', { name: 'Command an object: Tills' })).not.toBeInTheDocument();
  });

  it('saves exactly the list on the screen, with the revision it started from', async () => {
    const user = userEvent.setup();
    setGrants([grant('group', 11)], 'rev-7');
    save.mockResolvedValue({ grants: [], revision: 'rev-8' });
    render(<PlayerGrantsTab userId={7} role="operator" />);
    await user.click(box('Command a group: Tills'));
    await user.click(box('Access: Window'));
    await user.click(box('Audit: Window'));
    await user.click(saveBtn());
    expect(save).toHaveBeenCalledTimes(1);
    const sent = save.mock.calls[0][0] as { grants: UserPlayerGrant[]; revision: string };
    expect(sent.revision).toBe('rev-7');
    expect([...sent.grants].sort((a, b) => a.id - b.id)).toEqual([
      { scope: 'group', id: 11, can_command_group: true, can_command_object: false, can_move: false, can_view_audit: false },
      { scope: 'group', id: 12, can_command_group: false, can_command_object: false, can_move: false, can_view_audit: true },
    ]);
    expect(toast).toHaveBeenCalledWith('Rights saved', 'success');
  });

  it('can take every right away: an empty list is a real save', async () => {
    const user = userEvent.setup();
    setGrants([grant('group', 11)]);
    save.mockResolvedValue({ grants: [], revision: 'r' });
    render(<PlayerGrantsTab userId={7} role="operator" />);
    await user.click(box('Access: Tills'));
    await user.click(saveBtn());
    expect((save.mock.calls[0][0] as { grants: unknown[] }).grants).toEqual([]);
  });

  it('keeps Save off until something is changed, and Discard puts the list back', async () => {
    const user = userEvent.setup();
    setGrants([grant('group', 11)]);
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(saveBtn()).toBeDisabled();
    await user.click(box('Move: Tills'));
    expect(saveBtn()).toBeEnabled();
    await user.click(screen.getByRole('button', { name: 'Discard edits' }));
    expect(box('Move: Tills')).not.toBeChecked();
    expect(saveBtn()).toBeDisabled();
  });

  it('notes that a group is already reached through its object', () => {
    setGrants([grant('object', 1)]);
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getAllByText('access through the object')).toHaveLength(2);
  });

  it('never shows or sends commands and moves for a viewer, whatever is stored', async () => {
    const user = userEvent.setup();
    setGrants([grant('group', 11, { can_command_group: true, can_move: true, can_view_audit: true })]);
    save.mockResolvedValue({ grants: [], revision: 'r' });
    render(<PlayerGrantsTab userId={7} role="viewer" />);
    expect(screen.getByRole('note')).toHaveTextContent('only looks');
    expect(box('Command a group: Tills')).toBeDisabled();
    expect(box('Command a group: Tills')).not.toBeChecked();
    expect(box('Move: Tills')).toBeDisabled();
    expect(box('Audit: Tills')).toBeEnabled();
    expect(box('Audit: Tills')).toBeChecked();
    // what is stored and does nothing is still a difference to clear, so it cannot come alive later
    expect(screen.getByText(/Saving removes them/)).toBeInTheDocument();
    expect(saveBtn()).toBeEnabled();
    await user.click(saveBtn());
    expect((save.mock.calls[0][0] as { grants: UserPlayerGrant[] }).grants).toEqual([
      { scope: 'group', id: 11, can_command_group: false, can_command_object: false, can_move: false, can_view_audit: true },
    ]);
  });

  it('has nothing to save and no warning for a viewer whose stored rights are all effective', () => {
    setGrants([grant('group', 11, { can_view_audit: true })]);
    render(<PlayerGrantsTab userId={7} role="viewer" />);
    expect(saveBtn()).toBeDisabled();
    expect(screen.queryByText(/Saving removes them/)).not.toBeInTheDocument();
  });

  it("does not overwrite somebody else's change: it says so and reads the rights again", async () => {
    const user = userEvent.setup();
    setGrants([grant('group', 11)], 'old');
    save.mockRejectedValue(new ApiClientError(409, 'grants_changed'));
    render(<PlayerGrantsTab userId={7} role="operator" />);
    await user.click(box('Move: Tills'));
    await user.click(saveBtn());
    expect(screen.getByRole('alert')).toHaveTextContent('Somebody else has changed');
    expect(refetchGrants).toHaveBeenCalled();
    expect(toast).not.toHaveBeenCalledWith('Rights saved', 'success');
  });

  it('tells in words when a place was deleted meanwhile, and reads again', async () => {
    const user = userEvent.setup();
    save.mockRejectedValue(new ApiClientError(400, 'unknown_scope'));
    render(<PlayerGrantsTab userId={7} role="operator" />);
    await user.click(box('Access: Tills'));
    await user.click(saveBtn());
    expect(screen.getByRole('alert')).toHaveTextContent('has been deleted');
    expect(refetchGrants).toHaveBeenCalled();
    expect(refetchTree).toHaveBeenCalled();
  });

  it('leaves out rights that point at deleted places and says they will go', () => {
    setGrants([grant('group', 11), grant('group', 99, { missing: true })]);
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getByText(/pointed at a deleted place/)).toBeInTheDocument();
  });

  it('shows a plain failure when the rights cannot be loaded', () => {
    grantsState.data = undefined; grantsState.isError = true;
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the rights');
  });

  it('points to the place where objects are made when there are none', () => {
    treeState.data = [];
    render(<PlayerGrantsTab userId={7} role="operator" />);
    expect(screen.getByText(/no objects or groups yet/i)).toBeInTheDocument();
    expect(screen.queryAllByRole('checkbox')).toHaveLength(0);
  });
});
