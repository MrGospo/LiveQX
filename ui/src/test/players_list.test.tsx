/**
 * The list of screens and the card of one screen. The server is replaced at the
 * hooks; what is checked is what a person reads and which place the page asks for.
 */
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { Player, PlayerObject, PlayerDetails, PlayerCommand, PlayerListFilter } from '@/api/queries/players';

vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role: 'viewer', user: { id: 2, username: 'ann', role: 'viewer' } })),
}));

type Q<T> = { data?: T; isLoading: boolean; isError: boolean; error?: unknown };
const listState: Q<Player[]> = { isLoading: false, isError: false };
const treeState: Q<PlayerObject[]> = { isLoading: false, isError: false };
const oneState: Q<PlayerDetails> = { isLoading: false, isError: false };
const cmdState: Q<PlayerCommand[]> = { isLoading: false, isError: false };
const listCalls: PlayerListFilter[] = [];
const lastCall = () => listCalls[listCalls.length - 1];

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    usePlayers: (f: PlayerListFilter) => { listCalls.push(f); return listState; },
    usePlayerTree: () => treeState,
    usePlayer: () => oneState,
    usePlayerCommands: () => cmdState,
  };
});

import PlayersPage from '@/features/players/PlayersPage';
import PlayerDetailPage from '@/features/players/PlayerDetailPage';

const nowSec = () => Math.floor(Date.now() / 1000);

const mk = (id: string, name: string, over: Partial<Player> = {}): Player => ({
  id, name, state: 'active', clock_policy: 'strict', created_at: 1,
  group_id: 11, group_name: 'Tills', object_id: 1, object_name: 'Shop',
  status: { online: true, version: '1.4.2', updated_at: nowSec() - 5 },
  ...over,
});

const tree = (): PlayerObject[] => [
  { id: 1, name: 'Shop', created_at: 1, groups: [
    { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 2 },
    { id: 12, object_id: 1, name: 'Window', created_at: 1, players: 1 },
  ] },
];

const renderList = (url = '/players') =>
  render(<MemoryRouter initialEntries={[url]}><PlayersPage /></MemoryRouter>);

const renderDetail = () =>
  render(
    <MemoryRouter initialEntries={['/players/p1']}>
      <Routes><Route path="/players/:id" element={<PlayerDetailPage />} /><Route path="/players" element={<p>list</p>} /></Routes>
    </MemoryRouter>,
  );

beforeEach(() => {
  listCalls.length = 0;
  listState.data = [
    mk('p1', 'Till 1'),
    mk('p2', 'Till 2', { status: { online: false, version: '1.4.1', updated_at: nowSec() - 7200 } }),
    mk('p3', 'Window left', { group_id: 12, group_name: 'Window', state: 'pending', status: undefined }),
    mk('p4', 'Old one', { state: 'revoked', status: undefined }),
  ];
  listState.isLoading = false; listState.isError = false;
  treeState.data = tree(); treeState.isLoading = false; treeState.isError = false;
  oneState.data = undefined; oneState.isLoading = false; oneState.isError = false; oneState.error = undefined;
  cmdState.data = []; cmdState.isLoading = false; cmdState.isError = false;
});

// ═════════════════════════════ list ═══════════════════════════════════════════
describe('PlayersPage', () => {
  it('shows each screen with its place, status and version', () => {
    renderList();
    const row = screen.getByRole('link', { name: 'Till 1' }).closest('tr')!;
    expect(within(row).getByText('Shop / Tills')).toBeInTheDocument();
    expect(within(row).getByText('Online')).toBeInTheDocument();
    expect(within(row).getByText('1.4.2')).toBeInTheDocument();
    const off = screen.getByRole('link', { name: 'Till 2' }).closest('tr')!;
    expect(within(off).getByText('Offline')).toBeInTheDocument();
    expect(within(screen.getByRole('link', { name: 'Window left' }).closest('tr')!).getByText('Awaiting enrolment')).toBeInTheDocument();
    expect(within(screen.getByRole('link', { name: 'Old one' }).closest('tr')!).getByText('Revoked')).toBeInTheDocument();
  });

  it('counts the screens by status on the filter buttons', () => {
    renderList();
    expect(screen.getByRole('button', { name: 'All · 4' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Online · 1' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Offline · 1' })).toBeInTheDocument();
  });

  it('narrows the list by status', async () => {
    const user = userEvent.setup();
    renderList();
    await user.click(screen.getByRole('button', { name: 'Offline · 1' }));
    expect(screen.getByRole('link', { name: 'Till 2' })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Till 1' })).not.toBeInTheDocument();
  });

  it('narrows the list by the search text, in name or place', async () => {
    const user = userEvent.setup();
    renderList();
    await user.type(screen.getByRole('searchbox'), 'window');
    expect(screen.getByRole('link', { name: 'Window left' })).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'Till 1' })).not.toBeInTheDocument();
  });

  it('says so when the filter leaves nothing', async () => {
    const user = userEvent.setup();
    renderList();
    await user.type(screen.getByRole('searchbox'), 'zzz');
    expect(screen.getByText('Nothing found')).toBeInTheDocument();
  });

  it('asks the server for the place picked in the tree, and remembers it in the address', async () => {
    const user = userEvent.setup();
    renderList();
    expect(lastCall()).toEqual({ objectId: undefined, groupId: undefined });
    await user.click(screen.getByRole('button', { name: /Window/ }));
    expect(lastCall()).toEqual({ objectId: undefined, groupId: 12 });
    await user.click(screen.getByRole('button', { name: /^Shop/ }));
    expect(lastCall()).toEqual({ objectId: 1, groupId: undefined });
  });

  it('reads the place and status from the address', () => {
    renderList('/players?group=11&status=offline');
    expect(lastCall()).toEqual({ objectId: undefined, groupId: 11 });
    expect(screen.getByRole('button', { name: 'Offline · 1' })).toHaveAttribute('aria-pressed', 'true');
  });

  it('ignores a damaged address instead of breaking', () => {
    renderList('/players?group=abc&status=bogus');
    expect(lastCall()).toEqual({ objectId: undefined, groupId: undefined });
    expect(screen.getByRole('button', { name: 'All · 4' })).toHaveAttribute('aria-pressed', 'true');
  });

  it('tells a person with no screens what to do', () => {
    listState.data = [];
    renderList();
    expect(screen.getByText('No screens yet')).toBeInTheDocument();
    expect(screen.getByText(/ask an administrator/)).toBeInTheDocument();
  });

  it('warns when the list is cut at the server limit', () => {
    listState.data = Array.from({ length: 1000 }, (_, i) => mk(`id${i}`, `Screen ${i}`));
    renderList();
    expect(screen.getByRole('status')).toHaveTextContent('first 1000 screens');
  });

  it('shows a plain failure when the list cannot be loaded', () => {
    listState.data = undefined; listState.isError = true;
    renderList();
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the list of screens.');
  });
});

// ═════════════════════════════ card ═══════════════════════════════════════════
const details = (over: Partial<PlayerDetails> = {}): PlayerDetails => ({
  ...mk('p1', 'Till 1'),
  certificates: [{ serial: 'abc123', generation: 1, issued_at: 1, not_before: 1, not_after: nowSec() + 400 * 86400 }],
  events: [
    { id: 2, ts: nowSec() - 100, kind: 'offline', details: JSON.stringify({ reason: 'heartbeat_timeout', last_seen: 1 }) },
    { id: 1, ts: nowSec() - 900, kind: 'version_changed', details: JSON.stringify({ from: '1.4.0', to: '1.4.1' }) },
  ],
  ...over,
});

describe('PlayerDetailPage', () => {
  it('shows the screen, its place and what it last reported', () => {
    oneState.data = details({ status: { online: true, version: '1.4.2', updated_at: nowSec() - 5,
      state: { health: 'warning', uptime_seconds: 93_000, last_error: 'stream lost' } } });
    renderDetail();
    expect(screen.getByRole('heading', { name: 'Till 1' })).toBeInTheDocument();
    expect(screen.getByText('Shop / Tills')).toBeInTheDocument();
    expect(screen.getByText('has warnings')).toBeInTheDocument();
    expect(screen.getByText('1 d 1 h')).toBeInTheDocument();
    expect(screen.getByText('stream lost')).toBeInTheDocument();
  });

  it('says the report is old when the screen is offline, and not when it is online', () => {
    oneState.data = details({ status: { online: false, version: '1.4.1', updated_at: nowSec() - 7200, state: { health: 'ok' } } });
    const { unmount } = renderDetail();
    expect(screen.getByText('The screen is offline: below is what it last reported.')).toBeInTheDocument();
    unmount();
    oneState.data = details();
    renderDetail();
    expect(screen.queryByText(/below is what it last reported/)).not.toBeInTheDocument();
  });

  it('writes the journal in words and shows unknown kinds by name', () => {
    oneState.data = details({ events: [
      { id: 3, ts: nowSec(), kind: 'offline', details: JSON.stringify({ reason: 'heartbeat_timeout' }) },
      { id: 2, ts: nowSec(), kind: 'version_changed', details: JSON.stringify({ from: '1.4.0', to: '1.4.1' }) },
      { id: 1, ts: nowSec(), kind: 'something_new', details: '{broken' },
    ] });
    renderDetail();
    expect(screen.getByText('The screen went offline (its signals stopped)')).toBeInTheDocument();
    expect(screen.getByText('Player version changed: 1.4.0 → 1.4.1')).toBeInTheDocument();
    expect(screen.getByText('something_new')).toBeInTheDocument();
  });

  it('marks a certificate that ends soon, one that has ended and a revoked one', () => {
    oneState.data = details({ certificates: [
      { serial: 'soon', generation: 1, issued_at: 1, not_before: 1, not_after: nowSec() + 30 * 86400 },
      { serial: 'gone', generation: 1, issued_at: 1, not_before: 1, not_after: nowSec() - 86400 },
      { serial: 'rev',  generation: 1, issued_at: 1, not_before: 1, not_after: nowSec() + 999 * 86400, revoked_at: 5, revoke_reason: 'revoked_by_admin' },
    ] });
    renderDetail();
    expect(within(screen.getByText('soon').closest('tr')!).getByText('ends soon')).toBeInTheDocument();
    expect(within(screen.getByText('gone').closest('tr')!).getByText('expired')).toBeInTheDocument();
    expect(within(screen.getByText('rev').closest('tr')!).getByText('revoked')).toBeInTheDocument();
  });

  it('explains a screen that is not enrolled yet', () => {
    oneState.data = details({ state: 'pending', status: undefined, certificates: [] });
    renderDetail();
    expect(screen.getByRole('status')).toHaveTextContent('not enrolled yet');
    expect(screen.getByText('No certificates yet.')).toBeInTheDocument();
  });

  it('explains a revoked screen and its reason', () => {
    oneState.data = details({ state: 'revoked', revoked_at: nowSec() - 60, revoke_reason: 'revoked_by_admin', status: undefined });
    renderDetail();
    expect(screen.getByRole('status')).toHaveTextContent('Reason: revoked by an administrator.');
  });

  it('shows the commands sent to the screen in words', () => {
    oneState.data = details();
    cmdState.data = [{ id: 'c1', player_id: 'p1', type: 'reboot_screen', state: 'failed', result: 'os_not_rebooted',
      issued_at: nowSec() - 500, ttl_seconds: 300, deadline_at: nowSec() - 200, finished_at: nowSec() - 450 }];
    renderDetail();
    const row = screen.getByText('Reboot the screen').closest('tr')!;
    expect(within(row).getByText(/failed/)).toBeInTheDocument();
    expect(within(row).getByText(/did not reboot/)).toBeInTheDocument();
  });

  it('says "not found" without telling a missing screen from one the person may not see', () => {
    oneState.isError = true; oneState.error = new ApiClientError(404, 'not_found');
    renderDetail();
    expect(screen.getByRole('alert')).toHaveTextContent('Screen not found');
  });

  it('shows a different message when the failure is not a 404', () => {
    oneState.isError = true; oneState.error = new ApiClientError(500, 'storage_error');
    renderDetail();
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the screen.');
  });
});
