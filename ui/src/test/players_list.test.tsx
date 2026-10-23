/**
 * The list of screens and the card of one screen. The server is replaced at the
 * hooks; what is checked is what a person reads and which place the page asks for.
 */
import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { Player, PlayerObject, PlayerDetails, PlayerCommand, PlayerListQuery, PlayerPage } from '@/api/queries/players';

let role: 'viewer' | 'operator' | 'admin' = 'viewer';
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role, user: { id: 2, username: 'ann', role } })),
}));

type Q<T> = { data?: T; isLoading: boolean; isError: boolean; error?: unknown };
const listState: Q<PlayerPage> = { isLoading: false, isError: false };
const treeState: Q<PlayerObject[]> = { isLoading: false, isError: false };
const oneState: Q<PlayerDetails> = { isLoading: false, isError: false };
const cmdState: Q<PlayerCommand[]> = { isLoading: false, isError: false };
const listCalls: PlayerListQuery[] = [];
const lastCall = () => listCalls[listCalls.length - 1];

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    usePlayers: (f: PlayerListQuery) => { listCalls.push(f); return listState; },
    usePlayerTree: () => treeState,
    usePlayer: () => oneState,
    usePlayerCommands: () => cmdState,
    useBulkPlan: () => ({ mutateAsync: vi.fn(), isPending: false }),
    useBulkSend: () => ({ mutateAsync: vi.fn(), isPending: false }),
    usePlayerActions: () => { const a = { mutateAsync: vi.fn(), reset: vi.fn(), isPending: false }; return { create: a, reissueToken: a, reEnroll: a, move: a, revoke: a, remove: a }; },
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

// What the server answers for a list: the page, how many match, and the count by status.
const setPage = (players: Player[], counts: PlayerPage['counts'], total = players.length) => {
  listState.data = { players, total, counts };
};

const renderList = (url = '/players') =>
  render(<MemoryRouter initialEntries={[url]}><PlayersPage /></MemoryRouter>);

const renderDetail = () =>
  render(
    <MemoryRouter initialEntries={['/players/p1']}>
      <Routes><Route path="/players/:id" element={<PlayerDetailPage />} /><Route path="/players" element={<p>list</p>} /></Routes>
    </MemoryRouter>,
  );

beforeEach(() => {
  role = 'viewer';
  listCalls.length = 0;
  setPage([
    mk('p1', 'Till 1'),
    mk('p2', 'Till 2', { status: { online: false, version: '1.4.1', updated_at: nowSec() - 7200 } }),
    mk('p3', 'Window left', { group_id: 12, group_name: 'Window', state: 'pending', status: undefined }),
    mk('p4', 'Old one', { state: 'revoked', status: undefined }),
  ], { online: 1, offline: 1, pending: 1, revoked: 1 });
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

  it('shows the order the server gave and does not sort again', () => {
    setPage([mk('a', 'Zeta'), mk('b', 'Alpha')], { online: 2, offline: 0, pending: 0, revoked: 0 });
    renderList();
    const names = screen.getAllByRole('link').filter(l => l.getAttribute('href')?.startsWith('/players/')).map(l => l.textContent);
    expect(names).toEqual(['Zeta', 'Alpha']);
  });

  it('takes the numbers on the filter buttons from the server, for the whole park and not for the page', () => {
    setPage([mk('p1', 'Till 1')], { online: 120, offline: 30, pending: 5, revoked: 2 }, 157);
    renderList();
    expect(screen.getByRole('button', { name: 'All · 157' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Online · 120' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Offline · 30' })).toBeInTheDocument();
  });

  it('asks the server for the status, which narrows the list there and nowhere else', async () => {
    const user = userEvent.setup();
    renderList();
    await user.click(screen.getByRole('button', { name: 'Offline · 1' }));
    expect(lastCall()).toMatchObject({ status: 'offline', page: 1 });
    expect(screen.getByRole('link', { name: 'Till 1' })).toBeInTheDocument() ;
  });

  it('sends the search text to the server a moment after typing stops, not on every key', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    try {
      const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
      renderList();
      await user.type(screen.getByRole('searchbox'), 'window');
      expect(lastCall().q).toBeUndefined();
      await act(async () => { vi.advanceTimersByTime(400); });
      expect(lastCall()).toMatchObject({ q: 'window', page: 1 });
    } finally {
      vi.useRealTimers();
    }
  });

  it('says so when the filter leaves nothing', () => {
    setPage([], { online: 3, offline: 0, pending: 0, revoked: 0 }, 0);
    renderList('/players?q=zzz');
    expect(screen.getByText('Nothing found')).toBeInTheDocument();
  });

  it('asks the server for the place picked in the tree, and remembers it in the address', async () => {
    const user = userEvent.setup();
    renderList();
    expect(lastCall()).toMatchObject({ objectId: undefined, groupId: undefined });
    await user.click(screen.getByRole('button', { name: /Window/ }));
    expect(lastCall()).toMatchObject({ objectId: undefined, groupId: 12 });
    await user.click(screen.getByRole('button', { name: /^Shop/ }));
    expect(lastCall()).toMatchObject({ objectId: 1, groupId: undefined });
  });

  it('reads the place, the status, the search and the page from the address', () => {
    renderList('/players?group=11&status=offline&q=till&page=3');
    expect(lastCall()).toEqual({ objectId: undefined, groupId: 11, status: 'offline', q: 'till', page: 3 });
    expect(screen.getByRole('button', { name: 'Offline · 1' })).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByRole('searchbox')).toHaveValue('till');
  });

  it('ignores a damaged address instead of breaking', () => {
    renderList('/players?group=abc&status=bogus&page=-4');
    expect(lastCall()).toEqual({ objectId: undefined, groupId: undefined, status: undefined, q: undefined, page: 1 });
    expect(screen.getByRole('button', { name: 'All · 4' })).toHaveAttribute('aria-pressed', 'true');
  });

  it('tells a person with no screens what to do', () => {
    setPage([], { online: 0, offline: 0, pending: 0, revoked: 0 }, 0);
    renderList();
    expect(screen.getByText('No screens yet')).toBeInTheDocument();
    expect(screen.getByText(/ask an administrator/)).toBeInTheDocument();
  });

  it('pages a large park: the range, the next page, and back to the first page when a filter changes', async () => {
    const user = userEvent.setup();
    setPage(Array.from({ length: 50 }, (_, i) => mk(`id${i}`, `Screen ${i}`)), { online: 120, offline: 0, pending: 0, revoked: 0 }, 120);
    renderList();
    expect(screen.getByText('1–50 of 120')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Previous' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Next' }));
    expect(lastCall()).toMatchObject({ page: 2 });
    await user.click(screen.getByRole('button', { name: 'Offline · 0' }));
    expect(lastCall()).toMatchObject({ status: 'offline', page: 1 });
  });

  it('shows no pager when everything fits on one page', () => {
    renderList();
    expect(screen.queryByRole('navigation', { name: 'List pages' })).not.toBeInTheDocument();
  });

  // ── the command button follows the place picked and what the person may do there ──
  const markedTree = (cg: boolean, co: boolean): PlayerObject[] => [
    { id: 1, name: 'Shop', created_at: 1, can: { command_object: co }, groups: [
      { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 2, can: { move: false, command_group: cg } },
    ] },
  ];

  it('offers an administrator a command to everyone when no place is picked', () => {
    role = 'admin';
    renderList();
    expect(screen.getByRole('button', { name: 'Command everyone' })).toBeInTheDocument();
  });

  it('offers no command to everyone to an operator, nor anything to a viewer', () => {
    role = 'operator';
    renderList();
    expect(screen.queryByRole('button', { name: /^Command/ })).not.toBeInTheDocument();
    role = 'viewer';
    renderList('/players?group=11');
    expect(screen.queryByRole('button', { name: /^Command/ })).not.toBeInTheDocument();
  });

  it('offers a command to the picked group, to an operator even without the group mark (the playlist only)', () => {
    role = 'operator';
    treeState.data = markedTree(false, false);
    renderList('/players?group=11');
    expect(screen.getByRole('button', { name: 'Command the group' })).toBeInTheDocument();
  });

  it('offers a command to the picked object only with the object mark', () => {
    role = 'operator';
    treeState.data = markedTree(true, false);
    renderList('/players?object=1');
    expect(screen.queryByRole('button', { name: 'Command the object' })).not.toBeInTheDocument();
    treeState.data = markedTree(false, true);
    renderList('/players?object=1');
    expect(screen.getByRole('button', { name: 'Command the object' })).toBeInTheDocument();
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
