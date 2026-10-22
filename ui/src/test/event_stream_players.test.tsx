/**
 * What the live event stream does to the screens pages: which events refresh
 * which lists, and that a stream that came back looks at everything again (events
 * missed while it was down are not replayed). The transport is replaced; the
 * test feeds the provider the same frames the server sends.
 */
import { render } from '@testing-library/react';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import type { SseRunOptions } from '@/api/sseClient';

let captured: SseRunOptions | null = null;
vi.mock('@/api/sseClient', () => ({
  runSseLoop: vi.fn(async (opts: SseRunOptions) => { captured = opts; }),
}));
vi.mock('@/stores/auth', () => ({
  useAuthStore: Object.assign(
    vi.fn((selector: (s: object) => unknown) => selector({ accessToken: 'token' })),
    { getState: () => ({ logout: vi.fn() }) },
  ),
}));
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) =>
    selector({ setSseConnected: vi.fn(), setSseOverflowed: vi.fn(), addToast: vi.fn() })),
}));

import { EventBusProvider } from '@/hooks/useEventStream';

let qc: QueryClient;
let seen: string[] = [];

const frame = (type: string) => captured!.onMessage({ event: 'message', data: JSON.stringify({ type }) } as never);
const keysInvalidated = () => seen;

beforeEach(() => {
  captured = null;
  qc = new QueryClient();
  seen = [];
  qc.invalidateQueries = ((f?: { queryKey?: unknown }) => {
    seen.push(JSON.stringify(f?.queryKey ?? 'ALL'));
    return Promise.resolve();
  }) as typeof qc.invalidateQueries;
  render(<QueryClientProvider client={qc}><EventBusProvider><p>x</p></EventBusProvider></QueryClientProvider>);
});

describe('the event stream and the screens pages', () => {
  it('refreshes the broker page when the broker link changes, and only that', () => {
    frame('players_broker_status');
    expect(keysInvalidated()).toEqual([JSON.stringify(['players-admin', 'broker'])]);
  });

  it('refreshes the lists of screens, not the broker page, when a screen comes online', () => {
    frame('player_online');
    expect(keysInvalidated()).toEqual([JSON.stringify(['players'])]);
  });

  it('does not look at everything on the first connection', () => {
    captured!.onOpen!();
    expect(keysInvalidated()).toEqual([]);
  });

  it('looks at everything again when the stream comes back after a break', () => {
    captured!.onOpen!();
    captured!.onDisconnect!('network');
    captured!.onOpen!();
    expect(keysInvalidated()).toEqual(['"ALL"']);
  });
});
