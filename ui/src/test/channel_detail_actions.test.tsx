/**
 * Verifies the Play/Stop button visibility contract on ChannelDetailPage.
 *
 * Regression guard for the bug where a channel whose health flipped to
 * 'failed' (pipeline still running_) had its Stop button hidden because the
 * UI treated 'failed' as "already stopped". The backend contract is:
 *   state == 'stopped'  → pipeline is torn down
 *   state == anything else ('running'|'degraded'|'failed') → pipeline is live
 * so Stop must stay reachable for every non-'stopped' state.
 */
import React from 'react';
import { render, screen } from '@testing-library/react';
import { MemoryRouter, Routes, Route } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { describe, it, expect, vi } from 'vitest';

// i18next is initialized with real EN resources in src/test/setup.ts —
// assertions below use real translated strings.

// ─── Module mocks (mirrors smoke.test.tsx conventions) ───────────────────────
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({
    user: { id: 1, username: 'admin', role: 'admin' },
    accessToken: 'mock-token',
    role: 'admin',
    channelGrants: {},
    mustChangePassword: false,
    hasRole: (_r: string) => true,
    canAccessChannel: () => true,
    logout: vi.fn(),
    setTokens: vi.fn(),
    setUser: vi.fn(),
  })),
}));

vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({
    sidebarCollapsed: false, toasts: [], ldapDown: false,
    sseConnected: true, sseOverflowed: false,
    toggleSidebar: vi.fn(), addToast: vi.fn(), removeToast: vi.fn(),
    setLdapDown: vi.fn(), setSseConnected: vi.fn(), setSseOverflowed: vi.fn(),
  })),
}));

vi.mock('@/hooks/useToast', () => ({ useToast: () => vi.fn() }));
vi.mock('@/hooks/useEventStream', () => ({
  EventBusProvider: ({ children }: { children: React.ReactNode }) => <>{children}</>,
  useEventStream: () => ({ events: [], connected: false, overflowed: false }),
}));

vi.mock('@/api/queries/auth', () => ({
  useUsers: () => ({ data: [], isLoading: false }),
}));

vi.mock('@/api/queries/playlist', () => ({
  usePlaylist:       () => ({ data: [], isLoading: false }),
  useAppendPlaylist: () => ({ mutate: vi.fn() }),
  useDeletePlaylistItem: () => ({ mutate: vi.fn() }),
  useClearPlaylist:  () => ({ mutate: vi.fn() }),
  useNotifyDeleted:  () => ({ mutate: vi.fn() }),
  useReplacePlaylist:() => ({ mutate: vi.fn() }),
  useSchedule:       () => ({ data: [] }),
  useWatcherStatus:  () => ({ data: null }),
  useRescan:         () => ({ mutate: vi.fn(), isPending: false }),
  usePlaybackLog:    () => ({ data: { items: [] } }),
  usePlaybackLogStatus: () => ({ data: null }),
}));

// The channel returned by useChannel — state is driven per-test via
// setChannelState() below. All other fields are fixtures that satisfy the
// page's optional accesses.
const channelState = { value: 'running' as string };
function setChannelState(s: string) { channelState.value = s; }

vi.mock('@/api/queries/channels', () => ({
  useChannel: () => ({
    data: {
      id: 1,
      name: 'test',
      state: channelState.value,
      resolution: '1920x1080',
      fps_target: 25,
      fps_actual: 25,
      preset: 'veryfast',
      encoder_mode: 'auto',
      gpu_index: 0,
      video_codec: 'h264',
      audio_codec: 'aac',
      bitrate: 4000000,
      max_b_frames: 3,
      numa_node: 0,
      mpegts: {},
      outputs: [],
      current_clip_index: -1,
      preload_sec: 4,
    },
    isLoading: false,
  }),
  useChannels: () => ({ data: [], isLoading: false, refetch: vi.fn() }),
  usePlayChannel:  () => ({ mutate: vi.fn(), isPending: false }),
  useStopChannel:  () => ({ mutate: vi.fn(), isPending: false }),
  useNextClip:     () => ({ mutate: vi.fn(), isPending: false }),
  useDeleteChannel:() => ({ mutate: vi.fn(), isPending: false }),
  useAddOutput:    () => ({ mutate: vi.fn(), isPending: false }),
  usePatchOutput:  () => ({ mutate: vi.fn(), isPending: false }),
  useDeleteOutput: () => ({ mutate: vi.fn(), isPending: false }),
  useRestartOutput:() => ({ mutate: vi.fn(), isPending: false }),
}));

// ─── Helpers ─────────────────────────────────────────────────────────────────
function wrap(ui: React.ReactElement) {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  return (
    <QueryClientProvider client={qc}>
      <MemoryRouter initialEntries={['/channels/1']}>
        <Routes>
          <Route path="/channels/:id" element={ui} />
        </Routes>
      </MemoryRouter>
    </QueryClientProvider>
  );
}

// ─── Tests ───────────────────────────────────────────────────────────────────
describe('ChannelDetailPage Play/Stop buttons', () => {
  it('shows Stop (not Start) when state is running', async () => {
    setChannelState('running');
    const { default: ChannelDetailPage } = await import('@/features/channels/ChannelDetailPage');
    render(wrap(<ChannelDetailPage />));
    expect(screen.queryByText('Stop')).toBeTruthy();
    expect(screen.queryByText('Start')).toBeFalsy();
  });

  it('shows Stop (not Start) when state is degraded', async () => {
    setChannelState('degraded');
    const { default: ChannelDetailPage } = await import('@/features/channels/ChannelDetailPage');
    render(wrap(<ChannelDetailPage />));
    expect(screen.queryByText('Stop')).toBeTruthy();
    expect(screen.queryByText('Start')).toBeFalsy();
  });

  // The bug being guarded: 'failed' used to hide Stop and expose Start,
  // orphaning a Failed-but-still-transmitting channel.
  it('shows Stop (not Start) when state is failed', async () => {
    setChannelState('failed');
    const { default: ChannelDetailPage } = await import('@/features/channels/ChannelDetailPage');
    render(wrap(<ChannelDetailPage />));
    expect(screen.queryByText('Stop')).toBeTruthy();
    expect(screen.queryByText('Start')).toBeFalsy();
  });

  it('shows Start (not Stop) when state is stopped', async () => {
    setChannelState('stopped');
    const { default: ChannelDetailPage } = await import('@/features/channels/ChannelDetailPage');
    render(wrap(<ChannelDetailPage />));
    expect(screen.queryByText('Start')).toBeTruthy();
    expect(screen.queryByText('Stop')).toBeFalsy();
  });
});
