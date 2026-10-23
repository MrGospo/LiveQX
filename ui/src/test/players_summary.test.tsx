/**
 * The screens card on the main page: the numbers, what needs a look, and that it stays out of
 * the way (nothing for a person with no screens, nothing at all when the module is off).
 */
import { render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import type { BrokerStatus, CommandJob } from '@/api/queries/players';

let role: 'viewer' | 'operator' | 'admin' = 'admin';
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role, user: { id: 1, username: 'u', role } })),
}));

type Q<T> = { data?: T; isLoading: boolean; isError: boolean };
const summary: Q<{ counts: Record<'online' | 'offline' | 'pending' | 'revoked', number>; total: number }> = { isLoading: false, isError: false };
const broker: Q<Partial<BrokerStatus>> = { isLoading: false, isError: false };
const jobs: Q<Partial<CommandJob>[]> = { isLoading: false, isError: false };
const enabledCalls: { broker?: boolean; jobs?: boolean } = {};

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    usePlayersSummary: () => summary,
    useBrokerStatus: (o?: { enabled?: boolean }) => { enabledCalls.broker = o?.enabled; return broker; },
    useCommandJobs: (o?: { enabled?: boolean }) => { enabledCalls.jobs = o?.enabled; return jobs; },
  };
});

import { PlayersSummary } from '@/features/players/PlayersSummary';

const counts = (online: number, offline: number, pending = 0, revoked = 0) => ({ online, offline, pending, revoked });
const setSummary = (c: ReturnType<typeof counts>) => {
  summary.data = { counts: c, total: c.online + c.offline + c.pending + c.revoked };
};
const renderCard = () => render(<MemoryRouter><PlayersSummary /></MemoryRouter>);

beforeEach(() => {
  role = 'admin';
  summary.isLoading = false; summary.isError = false; setSummary(counts(12, 0));
  broker.data = { enabled: true, state: 'connected' }; jobs.data = [];
  enabledCalls.broker = undefined; enabledCalls.jobs = undefined;
});

describe('PlayersSummary', () => {
  it('shows how many screens are in each state, each a link to the list narrowed to that state', () => {
    setSummary(counts(12, 3, 2, 1));
    renderCard();
    expect(screen.getByRole('link', { name: 'Online' })).toHaveAttribute('href', '/players?status=online');
    expect(screen.getByRole('link', { name: 'Offline' })).toHaveAttribute('href', '/players?status=offline');
    expect(screen.getByRole('link', { name: 'Awaiting enrolment' })).toHaveAttribute('href', '/players?status=pending');
    expect(screen.getByText('12')).toBeInTheDocument();
    expect(screen.getByText('3')).toBeInTheDocument();
  });

  it('points at the screens that are offline, in words, only when there are some', () => {
    const first = renderCard();
    expect(screen.queryByText(/offline\./)).not.toBeInTheDocument();
    first.unmount();
    setSummary(counts(10, 2));
    renderCard();
    expect(screen.getByText(/2 screens are offline\./)).toBeInTheDocument();
  });

  it('says that command jobs are running, with a way to them', () => {
    jobs.data = [{ state: 'running' }, { state: 'done' }, { state: 'running' }];
    renderCard();
    expect(screen.getByText(/2 command jobs are running\./)).toBeInTheDocument();
  });

  it('warns an administrator when the server is not connected to the command broker', () => {
    broker.data = { enabled: true, state: 'connecting' };
    renderCard();
    expect(screen.getByText(/not connected to the command broker/)).toBeInTheDocument();
  });

  it('says nothing about the broker when it is off on purpose', () => {
    broker.data = { enabled: false, state: 'disabled' };
    renderCard();
    expect(screen.queryByText(/command broker/)).not.toBeInTheDocument();
  });

  it('asks about the broker only for an administrator and about jobs only for those who may command', () => {
    role = 'viewer';
    setSummary(counts(3, 0));
    const a = renderCard();
    expect(enabledCalls).toEqual({ broker: false, jobs: false });
    a.unmount();
    role = 'operator';
    const b = renderCard();
    expect(enabledCalls).toEqual({ broker: false, jobs: true });
    b.unmount();
    role = 'admin';
    renderCard();
    expect(enabledCalls).toEqual({ broker: true, jobs: true });
  });

  it('does not appear for a person who has no screens', () => {
    role = 'operator';
    setSummary(counts(0, 0));
    const { container } = renderCard();
    expect(container).toBeEmptyDOMElement();
  });

  it('invites an administrator with no screens yet to add the first', () => {
    setSummary(counts(0, 0));
    renderCard();
    expect(screen.getByText(/No screens yet/)).toBeInTheDocument();
  });

  it('stays out of the way while loading and on a failure', () => {
    summary.isLoading = true; summary.data = undefined;
    const a = renderCard();
    expect(a.container).toBeEmptyDOMElement();
    a.unmount();
    summary.isLoading = false; summary.isError = true;
    expect(renderCard().container).toBeEmptyDOMElement();
  });
});
