/**
 * Adding a screen, the one-time pairing package and the actions on a screen's card.
 * The server is replaced at the hooks; what is checked is what the person sees,
 * what the page asks the server to do, and that the token is handled with care.
 */
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { PairingPackage, Player, PlayerObject } from '@/api/queries/players';

const toast = vi.fn();
let role: 'viewer' | 'operator' | 'admin' = 'admin';
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role, user: { id: 1, username: 'u', role } })),
}));
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({ addToast: toast })),
}));

const act = () => ({ mutateAsync: vi.fn(), reset: vi.fn(), isPending: false });
const actions = { create: act(), reissueToken: act(), reEnroll: act(), move: act(), revoke: act(), remove: act() };
const treeState: { data?: PlayerObject[]; isLoading: boolean; isError: boolean } = { isLoading: false, isError: false };

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return { ...real, usePlayerActions: () => actions, usePlayerTree: () => treeState };
});

import { AddPlayerDialog } from '@/features/players/AddPlayerDialog';
import { PlayerActions } from '@/features/players/PlayerActions';

const objects = (): PlayerObject[] => [
  { id: 1, name: 'Shop', created_at: 1, groups: [
    { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 2 },
    { id: 12, object_id: 1, name: 'Window', created_at: 1, players: 0 },
  ] },
  { id: 2, name: 'Warehouse', created_at: 1, groups: [] },
];

const pkg = (): PairingPackage => ({
  schema: 1, player_id: 'p-new', server_url: 'https://liveqx.example.lan', ca_cert_pem: 'PEM',
  device_api_url: 'https://liveqx.example.lan:8444', mqtt_broker_url: 'mqtts://liveqx.example.lan:8883',
  bootstrap_token: 'SECRET-TOKEN-123', token_expires_at: new Date(Date.now() + 7 * 86400_000).toISOString(), clock_policy: 'strict',
});

const player = (over: Partial<Player> = {}): Player => ({
  id: 'p1', name: 'Till 1', state: 'active', clock_policy: 'strict', created_at: 1,
  group_id: 11, group_name: 'Tills', object_id: 1, object_name: 'Shop',
  status: { online: true, version: '1.4.2', updated_at: 1 }, ...over,
});

beforeEach(() => {
  toast.mockClear();
  role = 'admin';
  for (const a of Object.values(actions)) { a.mutateAsync.mockReset(); a.reset.mockReset(); a.isPending = false; }
  treeState.data = objects(); treeState.isLoading = false; treeState.isError = false;
});

const renderAdd = (props: Partial<Parameters<typeof AddPlayerDialog>[0]> = {}) => {
  const onClose = vi.fn();
  render(<MemoryRouter><AddPlayerDialog objects={objects()} onClose={onClose} {...props} /></MemoryRouter>);
  return onClose;
};

// ═════════════════════════════ add ════════════════════════════════════════════
describe('AddPlayerDialog', () => {
  it('needs a name and a group before it asks the server', async () => {
    const user = userEvent.setup();
    renderAdd();
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    expect(actions.create.mutateAsync).not.toHaveBeenCalled();
    expect(screen.getByText('Enter a name')).toBeInTheDocument();
    expect(screen.getByText('Pick a group')).toBeInTheDocument();
  });

  it('sends the trimmed name and the group, and no clock check unless one is chosen', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    renderAdd();
    await user.type(screen.getByLabelText('Screen name'), '  Till 9  ');
    await user.selectOptions(screen.getByLabelText('Group'), 'Window');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    expect(actions.create.mutateAsync).toHaveBeenCalledWith({ name: 'Till 9', group_id: 12 });
  });

  it('sends the clock check when it is chosen', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    renderAdd();
    await user.type(screen.getByLabelText('Screen name'), 'A');
    await user.selectOptions(screen.getByLabelText('Group'), 'Tills');
    await user.selectOptions(screen.getByLabelText('Clock check'), 'relaxed');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    expect(actions.create.mutateAsync).toHaveBeenCalledWith({ name: 'A', group_id: 11, clock_policy: 'relaxed' });
  });

  it('starts with the group picked in the list', () => {
    renderAdd({ initialGroupId: 12 });
    expect(screen.getByLabelText('Group')).toHaveValue('12');
  });

  it('lists only groups that exist, under their objects', () => {
    renderAdd();
    const options = within(screen.getByLabelText('Group')).getAllByRole('option').map(o => o.textContent);
    expect(options).toEqual(['Pick a group…', 'Tills', 'Window']);
  });

  it('sends the person to create a group first when there is none', () => {
    renderAdd({ objects: [{ id: 2, name: 'Warehouse', created_at: 1, groups: [] }] });
    expect(screen.getByRole('status')).toHaveTextContent('at least one group');
    expect(screen.getByRole('link', { name: 'Create an object and a group' })).toHaveAttribute('href', '/players/objects');
    expect(screen.getByRole('button', { name: 'Create and get the package' })).toBeDisabled();
  });

  it('points at the settings when the module is not set up', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockRejectedValue(new ApiClientError(409, 'not_configured'));
    renderAdd({ initialGroupId: 11 });
    await user.type(screen.getByLabelText('Screen name'), 'A');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    expect(screen.getByRole('alert')).toHaveTextContent('not set up yet');
    expect(screen.getByRole('link', { name: 'Open the screens settings' })).toHaveAttribute('href', '/players/settings');
  });

  it('shows the package after creating, and the token is in it', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    renderAdd({ initialGroupId: 11 });
    await user.type(screen.getByLabelText('Screen name'), 'Till 9');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    expect(screen.getByRole('dialog', { name: 'Pairing package: Till 9' })).toBeInTheDocument();
    expect((screen.getByLabelText('Pairing package') as HTMLTextAreaElement).value).toContain('SECRET-TOKEN-123');
    expect(screen.getByRole('alert')).toHaveTextContent('shown once');
  });

  it('keeps the package window open until the package is saved, then wipes it on close', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    const onClose = renderAdd({ initialGroupId: 11 });
    await user.type(screen.getByLabelText('Screen name'), 'Till 9');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    const done = screen.getByRole('button', { name: 'Done' });
    expect(done).toBeDisabled();
    await user.keyboard('{Escape}');
    expect(onClose).not.toHaveBeenCalled();
    await user.click(screen.getByLabelText('I have saved the package'));
    expect(done).toBeEnabled();
    await user.click(done);
    expect(actions.create.reset).toHaveBeenCalled();
    expect(onClose).toHaveBeenCalled();
  });

  it('counts a copy as saved', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    renderAdd({ initialGroupId: 11 });
    await user.type(screen.getByLabelText('Screen name'), 'Till 9');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    await user.click(screen.getByRole('button', { name: 'Copy' }));
    expect(await screen.findByRole('button', { name: 'Done' })).toBeEnabled();
    expect(await navigator.clipboard.readText()).toContain('SECRET-TOKEN-123');
  });

  it('counts a download as saved, under a safe file name', async () => {
    const user = userEvent.setup();
    actions.create.mutateAsync.mockResolvedValue(pkg());
    const made: string[] = [];
    URL.createObjectURL = vi.fn(() => 'blob:x');
    URL.revokeObjectURL = vi.fn();
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (this: HTMLAnchorElement) { made.push(this.download); });
    renderAdd({ initialGroupId: 11 });
    await user.type(screen.getByLabelText('Screen name'), 'Касса / 1');
    await user.click(screen.getByRole('button', { name: 'Create and get the package' }));
    await user.click(screen.getByRole('button', { name: 'Download file' }));
    expect(made).toEqual(['liveqx-pairing-Касса-1.json']);
    expect(screen.getByRole('button', { name: 'Done' })).toBeEnabled();
    click.mockRestore();
  });
});

// ═════════════════════════════ card actions ═══════════════════════════════════
const renderActions = (p: Player) =>
  render(
    <MemoryRouter initialEntries={['/players/p1']}>
      <Routes>
        <Route path="/players/:id" element={<PlayerActions player={p} />} />
        <Route path="/players" element={<p>the list</p>} />
      </Routes>
    </MemoryRouter>,
  );

const buttons = () => screen.queryAllByRole('button').map(b => b.textContent);

describe('PlayerActions', () => {
  it('shows nothing to a viewer', () => {
    role = 'viewer';
    renderActions(player());
    expect(buttons()).toEqual([]);
  });

  it('shows only the move to an operator', () => {
    role = 'operator';
    renderActions(player());
    expect(buttons()).toEqual(['Move']);
  });

  it('shows an administrator everything that fits the screen', () => {
    renderActions(player());
    expect(buttons()).toEqual(['Move', 'Bind again', 'Revoke', 'Delete']);
  });

  it('offers a new token only to a screen that is not enrolled yet', () => {
    renderActions(player({ state: 'pending', status: undefined }));
    expect(buttons()).toEqual(['Move', 'New token', 'Revoke', 'Delete']);
  });

  it('leaves a revoked screen only the way back and Delete', () => {
    renderActions(player({ state: 'revoked', status: undefined }));
    expect(buttons()).toEqual(['Bind again', 'Delete']);
  });

  it('binds an enrolled screen again, with the reason, and shows the new package', async () => {
    const user = userEvent.setup();
    actions.reEnroll.mutateAsync.mockResolvedValue(pkg());
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Bind again' }));
    expect(screen.getByText(/The old device stops working at once/)).toBeInTheDocument();
    expect(screen.getByText(/the identifier is the same/)).toBeInTheDocument();
    await user.type(screen.getByLabelText('Reason (optional)'), '  display replaced ');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Revoke and bind again' }));
    expect(actions.reEnroll.mutateAsync).toHaveBeenCalledWith({ id: 'p1', reason: 'display replaced' });
    expect((screen.getByLabelText('Pairing package') as HTMLTextAreaElement).value).toContain('SECRET-TOKEN-123');
    await user.click(screen.getByLabelText('I have saved the package'));
    await user.click(screen.getByRole('button', { name: 'Done' }));
    expect(actions.reEnroll.reset).toHaveBeenCalled();
  });

  it('tells a revoked screen apart: nothing new is revoked, the old certificates stay revoked', async () => {
    const user = userEvent.setup();
    renderActions(player({ state: 'revoked', status: undefined }));
    await user.click(screen.getByRole('button', { name: 'Bind again' }));
    expect(screen.getByText(/The earlier certificates stay revoked/)).toBeInTheDocument();
    expect(screen.queryByText(/The old device stops working at once/)).not.toBeInTheDocument();
  });

  it('keeps the dialog open with a plain message when the server refuses to bind again', async () => {
    const user = userEvent.setup();
    actions.reEnroll.mutateAsync.mockRejectedValue(new ApiClientError(409, 'not_configured'));
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Bind again' }));
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Revoke and bind again' }));
    expect(screen.getByRole('alert')).toHaveTextContent('not set up');
    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });

  it('does not offer binding again to an operator', () => {
    role = 'operator';
    renderActions(player({ state: 'revoked', status: undefined }));
    expect(buttons()).toEqual([]);
  });

  it('moves the screen to another group, offering all but the current one', async () => {
    const user = userEvent.setup();
    actions.move.mutateAsync.mockResolvedValue({});
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Move' }));
    const options = within(screen.getByLabelText('To group')).getAllByRole('option').map(o => o.textContent);
    expect(options).toEqual(['Pick a group…', 'Window']);
    await user.selectOptions(screen.getByLabelText('To group'), 'Window');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Move' }));
    expect(actions.move.mutateAsync).toHaveBeenCalledWith({ id: 'p1', groupId: 12 });
    expect(toast).toHaveBeenCalledWith('Screen moved', 'success');
  });

  it('explains a refused move in words and stays open', async () => {
    const user = userEvent.setup();
    role = 'operator';
    actions.move.mutateAsync.mockRejectedValue(new ApiClientError(403, 'forbidden'));
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Move' }));
    expect(screen.getByText(/only between groups where you have the move right/)).toBeInTheDocument();
    await user.selectOptions(screen.getByLabelText('To group'), 'Window');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Move' }));
    expect(screen.getByRole('alert')).toHaveTextContent('No right for this move');
    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });

  it('issues a new token and shows the package', async () => {
    const user = userEvent.setup();
    actions.reissueToken.mutateAsync.mockResolvedValue(pkg());
    renderActions(player({ state: 'pending', status: undefined }));
    await user.click(screen.getByRole('button', { name: 'New token' }));
    await user.click(screen.getByRole('button', { name: 'Issue a token' }));
    expect(actions.reissueToken.mutateAsync).toHaveBeenCalledWith('p1');
    expect((screen.getByLabelText('Pairing package') as HTMLTextAreaElement).value).toContain('SECRET-TOKEN-123');
  });

  it('revokes with the reason trimmed', async () => {
    const user = userEvent.setup();
    actions.revoke.mutateAsync.mockResolvedValue({ revoked_certs: 1 });
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Revoke' }));
    await user.type(screen.getByLabelText('Reason (optional)'), '  stolen  ');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Revoke' }));
    expect(actions.revoke.mutateAsync).toHaveBeenCalledWith({ id: 'p1', reason: 'stolen' });
    expect(toast).toHaveBeenCalledWith('Screen revoked', 'success');
  });

  it('sends no reason when none is typed, and blocks one that is too long', async () => {
    const user = userEvent.setup();
    actions.revoke.mutateAsync.mockResolvedValue({ revoked_certs: 1 });
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Revoke' }));
    await user.type(screen.getByLabelText('Reason (optional)'), 'я'.repeat(101));      // 202 bytes
    expect(within(screen.getByRole('dialog')).getByRole('button', { name: 'Revoke' })).toBeDisabled();
    await user.clear(screen.getByLabelText('Reason (optional)'));
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Revoke' }));
    expect(actions.revoke.mutateAsync).toHaveBeenCalledWith({ id: 'p1', reason: undefined });
  });

  it('deletes only after the exact name is typed, then goes back to the list', async () => {
    const user = userEvent.setup();
    actions.remove.mutateAsync.mockResolvedValue({ revoked_certs: 1 });
    renderActions(player());
    await user.click(screen.getByRole('button', { name: 'Delete' }));
    const confirm = within(screen.getByRole('dialog')).getByRole('button', { name: 'Delete' });
    expect(confirm).toBeDisabled();
    await user.type(screen.getByLabelText(/To confirm, type the name/), 'Till');
    expect(confirm).toBeDisabled();
    await user.type(screen.getByLabelText(/To confirm, type the name/), ' 1');
    expect(confirm).toBeEnabled();
    await user.click(confirm);
    expect(actions.remove.mutateAsync).toHaveBeenCalledWith('p1');
    expect(await screen.findByText('the list')).toBeInTheDocument();
  });
});
