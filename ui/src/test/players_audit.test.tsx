/**
 * The journal by place. The server is replaced at the hooks; what is checked is what the
 * person reads (a move shows both ends, a place outside their tree is not named), which places
 * they may filter by, and that the journal tab appears only for those who may read a journal.
 */
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { PlayerObject } from '@/api/queries/players';
import type { AuditTrailEvent, AuditTrailFilter, AuditTrailListResponse } from '@/api/types';

let role: 'viewer' | 'operator' | 'admin' = 'admin';
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role, user: { id: 1, username: 'u', role } })),
}));

type Q<T> = { data?: T; isLoading: boolean; isError: boolean; error?: unknown };
const auditState: Q<AuditTrailListResponse> = { isLoading: false, isError: false };
const treeState: Q<PlayerObject[]> = { isLoading: false, isError: false };
const auditCalls: AuditTrailFilter[] = [];
const lastCall = () => auditCalls[auditCalls.length - 1];

vi.mock('@/api/queries/audit', () => ({
  useAuditTrail: (f: AuditTrailFilter) => { auditCalls.push(f); return auditState; },
}));
vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return { ...real, usePlayerTree: () => treeState };
});

import PlayersAuditPage from '@/features/players/PlayersAuditPage';
import { PlayersSubNav } from '@/features/players/PlayersSubNav';
import { placeIndex, placeText } from '@/features/players/places';

const tree = (audit: { shop?: boolean; tills?: boolean; window?: boolean } = {}): PlayerObject[] => [
  { id: 1, name: 'Shop', created_at: 1, can: { command_object: false, view_audit: !!audit.shop }, groups: [
    { id: 11, object_id: 1, name: 'Tills', created_at: 1, players: 2, can: { move: false, command_group: false, view_audit: !!audit.tills } },
    { id: 12, object_id: 1, name: 'Window', created_at: 1, players: 0, can: { move: false, command_group: false, view_audit: !!audit.window } },
  ] },
  { id: 2, name: 'Warehouse', created_at: 1, can: { command_object: false, view_audit: false }, groups: [
    { id: 21, object_id: 2, name: 'Dock', created_at: 1, players: 0, can: { move: false, command_group: false, view_audit: false } },
  ] },
];

const ev = (id: number, action: string, over: Partial<AuditTrailEvent> = {}): AuditTrailEvent => ({
  id, ts_ms: 1_800_000_000_000 + id, category: 'player', action, summary: '', target_type: 'player', target_id: 'p1-uuid-0000',
  http_method: 'POST', http_path: '/', http_status: 200, elapsed_ms: 1, actor_user_id: 1, actor_username: 'ann', actor_role: 'admin',
  actor_ip: '', request_id: '', key_fingerprint: '', legacy: false, details: { name: 'Till 1' }, object_id: 1, group_id: 11, ...over,
});

const setEvents = (events: AuditTrailEvent[], total = events.length) => { auditState.data = { total, limit: 50, offset: 0, events }; };
const renderPage = () => render(<MemoryRouter><PlayersAuditPage /></MemoryRouter>);

beforeEach(() => {
  role = 'admin';
  auditCalls.length = 0;
  auditState.isLoading = false; auditState.isError = false; auditState.error = undefined;
  treeState.data = tree(); treeState.isLoading = false; treeState.isError = false;
  setEvents([ev(1, 'player.register')]);
});

describe('places.ts', () => {
  it('names a group with its object, an object alone, and says "unknown" for a place outside the tree', () => {
    const idx = placeIndex(tree());
    expect(placeText(idx, 1, 11, '?')).toBe('Shop / Tills');
    expect(placeText(idx, 1, undefined, '?')).toBe('Shop');
    expect(placeText(idx, 9, 99, '?')).toBe('?');
    expect(placeText(idx, undefined, undefined, '?')).toBe('');
  });
});

describe('PlayersAuditPage', () => {
  it('writes each row in words: when, who, what, about what and where', () => {
    setEvents([ev(2, 'player.command_group', { details: { name: 'Restart the player' } }), ev(1, 'player.register')]);
    renderPage();
    const row = screen.getByText('Screen added').closest('tr')!;
    expect(within(row).getByText('ann')).toBeInTheDocument();
    expect(within(row).getByRole('link', { name: 'Till 1' })).toHaveAttribute('href', '/players/p1-uuid-0000');
    expect(within(row).getByText('Shop / Tills')).toBeInTheDocument();
    expect(screen.getByText('Command to a group')).toBeInTheDocument();
  });

  it('shows a move as one row with both ends', () => {
    setEvents([ev(1, 'player.move', { group_id: 11, from_group_id: 12, from_object_id: 1 })]);
    renderPage();
    const row = screen.getByText('Screen moved').closest('tr')!;
    expect(within(row).getByText('Shop / Window')).toBeInTheDocument();      // from
    expect(within(row).getByText('Shop / Tills')).toBeInTheDocument();       // to
    expect(within(row).getByLabelText('→')).toBeInTheDocument();
  });

  it('does not name a place the person cannot see (the source of a move, or one deleted since)', () => {
    role = 'operator';
    treeState.data = tree({ tills: true }).map(o => ({ ...o, groups: o.groups.filter(g => g.id === 11) }));
    setEvents([ev(1, 'player.move', { group_id: 11, from_group_id: 77, from_object_id: 9 })]);
    renderPage();
    const row = screen.getByText('Screen moved').closest('tr')!;
    expect(within(row).getByText('another place')).toBeInTheDocument();
    expect(within(row).getByText('Shop / Tills')).toBeInTheDocument();
  });

  it('falls back to the raw action name for an action it has no words for', () => {
    setEvents([ev(1, 'player.something_new')]);
    renderPage();
    expect(screen.getByText('player.something_new')).toBeInTheDocument();
  });

  it('asks only for events of screens, and asks for the picked place', async () => {
    const user = userEvent.setup();
    renderPage();
    expect(lastCall()).toMatchObject({ category: 'player', object_id: undefined, group_id: undefined });
    await user.selectOptions(screen.getByLabelText('Object'), 'Shop');
    expect(lastCall()).toMatchObject({ object_id: 1, group_id: undefined });
    await user.selectOptions(screen.getByLabelText('Group'), 'Window');
    expect(lastCall()).toMatchObject({ object_id: 1, group_id: 12 });
  });

  it('offers a person only the places whose journal is open to them', () => {
    role = 'operator';
    treeState.data = tree({ tills: true });
    renderPage();
    const objects = within(screen.getByLabelText('Object')).getAllByRole('option').map(o => o.textContent);
    expect(objects).toEqual(['All objects', 'Shop']);                           // not the Warehouse
  });

  it('offers every group of an object whose own journal is open', async () => {
    const user = userEvent.setup();
    role = 'viewer';
    treeState.data = tree({ shop: true });
    renderPage();
    await user.selectOptions(screen.getByLabelText('Object'), 'Shop');
    expect(within(screen.getByLabelText('Group')).getAllByRole('option').map(o => o.textContent)).toEqual(['All groups', 'Tills', 'Window']);
  });

  it('shows only the permitted group when the right is on the group alone', async () => {
    const user = userEvent.setup();
    role = 'operator';
    treeState.data = tree({ window: true });
    renderPage();
    await user.selectOptions(screen.getByLabelText('Object'), 'Shop');
    expect(within(screen.getByLabelText('Group')).getAllByRole('option').map(o => o.textContent)).toEqual(['All groups', 'Window']);
  });

  it('says in words that no journal is open when the server refuses, and offers no filter', () => {
    role = 'operator';
    auditState.data = undefined; auditState.isError = true; auditState.error = new ApiClientError(403, 'forbidden');
    renderPage();
    expect(screen.getByRole('alert')).toHaveTextContent("No place's journal is open to you");
    expect(screen.queryByLabelText('Object')).not.toBeInTheDocument();
  });

  it('shows a different message for any other failure', () => {
    auditState.data = undefined; auditState.isError = true; auditState.error = new ApiClientError(500, 'storage_error');
    renderPage();
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the journal.');
  });

  it('says so when there is nothing to show', () => {
    setEvents([]);
    renderPage();
    expect(screen.getByText('No records')).toBeInTheDocument();
  });

  it('pages a long journal and goes back to the first page when the place changes', async () => {
    const user = userEvent.setup();
    setEvents(Array.from({ length: 50 }, (_, i) => ev(i + 1, 'player.register')), 120);
    renderPage();
    expect(screen.getByText('1–50 of 120')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Next' }));
    expect(lastCall()).toMatchObject({ offset: 50 });
    await user.selectOptions(screen.getByLabelText('Object'), 'Shop');
    expect(lastCall()).toMatchObject({ offset: 0, object_id: 1 });
  });

  it("links an administrator to the server's full journal", () => {
    renderPage();
    expect(screen.getByRole('link', { name: "The server's full journal" })).toHaveAttribute('href', '/settings/audit-trail');
  });

  it('does not link anyone else to it', () => {
    role = 'operator';
    treeState.data = tree({ tills: true });
    renderPage();
    expect(screen.queryByRole('link', { name: "The server's full journal" })).not.toBeInTheDocument();
  });
});

describe('PlayersSubNav', () => {
  const tabs = () => screen.queryAllByRole('link').map(l => l.textContent);
  const renderNav = () => render(<MemoryRouter><PlayersSubNav /></MemoryRouter>);

  it('shows the journal tab to an administrator', () => {
    renderNav();
    expect(tabs()).toContain('Journal');
  });

  it('shows it to anyone who has the journal of at least one place, even only one group', () => {
    role = 'operator';
    treeState.data = tree({ window: true });
    renderNav();
    expect(tabs()).toContain('Journal');
  });

  it('hides it from those who have no journal, so there is no dead tab', () => {
    role = 'operator';
    treeState.data = tree();
    renderNav();
    expect(tabs()).not.toContain('Journal');
    expect(tabs()).toContain('List');
  });
});
