/**
 * Smoke tests — verify key pages render without crashing.
 * Run: npm test
 */
import React from 'react';
import { render, screen } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { describe, it, expect, vi } from 'vitest';

// ─── Mock i18next ─────────────────────────────────────────────────────────────
vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
    i18n: { language: 'en', changeLanguage: vi.fn() },
  }),
  initReactI18next: { type: '3rdParty', init: vi.fn() },
}));

// ─── Mock Zustand stores ──────────────────────────────────────────────────────
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
    sidebarCollapsed: false,
    toasts: [],
    ldapDown: false,
    sseConnected: true,
    sseOverflowed: false,
    toggleSidebar: vi.fn(),
    addToast: vi.fn(),
    removeToast: vi.fn(),
    setLdapDown: vi.fn(),
    setSseConnected: vi.fn(),
    setSseOverflowed: vi.fn(),
  })),
}));

// ─── Mock TanStack Query hooks ────────────────────────────────────────────────
vi.mock('@/api/queries/channels', () => ({
  useChannels: () => ({ data: [], isLoading: false, refetch: vi.fn() }),
  useChannel:  () => ({ data: null, isLoading: false }),
  usePlayChannel: () => ({ mutate: vi.fn(), isPending: false }),
  useStopChannel: () => ({ mutate: vi.fn(), isPending: false }),
  useNextClip:    () => ({ mutate: vi.fn(), isPending: false }),
  useChannelPermissions: () => ({ data: [] }),
}));

vi.mock('@/api/queries/auth', () => ({
  useUsers:      () => ({ data: [], isLoading: false }),
  useAuditEvents:() => ({ data: [], isLoading: false }),
  useLdapConfig: () => ({ data: null, isLoading: false }),
  useSmtpConfig: () => ({ data: null, isLoading: false }),
  useSaveLdapConfig: () => ({ mutateAsync: vi.fn(), isPending: false }),
  useSaveSmtpConfig: () => ({ mutateAsync: vi.fn(), isPending: false }),
  useTestLdap:   () => ({ mutateAsync: vi.fn(), isPending: false }),
  useTestSmtp:   () => ({ mutateAsync: vi.fn(), isPending: false }),
  usePurgeAudit: () => ({ mutateAsync: vi.fn(), isPending: false }),
  useDeleteUser: () => ({ mutate: vi.fn() }),
  useEnableUser: () => ({ mutate: vi.fn() }),
  useUnlockUser: () => ({ mutate: vi.fn() }),
  useChangePassword: () => ({ mutateAsync: vi.fn(), isPending: false }),
}));

vi.mock('@/api/queries/gateways', () => ({
  useGateways:   () => ({ data: [], isLoading: false }),
  usePlayGateway:() => ({ mutate: vi.fn() }),
  useStopGateway:() => ({ mutate: vi.fn() }),
  useDeleteGateway:() => ({ mutate: vi.fn() }),
  useCreateGateway:() => ({ mutate: vi.fn() }),
  useVersion:    () => ({ data: { version: '0.9.4', features: {}, attributions: [] }, isLoading: false }),
  useSystemStatus:() => ({ data: null, isLoading: false }),
  useNetworkInterfaces: () => ({ data: [] }),
  useGpuInfo: () => ({ data: {} }),
}));

vi.mock('@/api/queries/plugins', () => ({
  usePlugins:     () => ({ data: [], isLoading: false }),
  useUninstallPlugin: () => ({ mutate: vi.fn() }),
  useAcceptEula:  () => ({ mutate: vi.fn() }),
}));

vi.mock('@/api/queries/operations', () => ({
  useStressStatus: () => ({ data: { runner: { state: 'idle' } }, isLoading: false }),
  useStressReports:() => ({ data: [], isLoading: false }),
  useStartStress:  () => ({ mutate: vi.fn(), isPending: false }),
  useStopStress:   () => ({ mutate: vi.fn(), isPending: false }),
  useStressConfig: () => ({ data: { enabled: false, duration_sec: 3600, scenarios: [], scenario_options: {}, channels: 1, channel_resolution: '1280x720', pass: {} } }),
  useSaveStressConfig: () => ({ mutate: vi.fn(), isPending: false }),
  usePerfSnapshot: () => ({ data: null }),
  useStartPerf:    () => ({ mutate: vi.fn() }),
  useStopPerf:     () => ({ mutate: vi.fn() }),
  useHealthz:      () => ({ data: null, isFetching: false, refetch: vi.fn() }),
  useReadyz:       () => ({ data: null, isFetching: false, refetch: vi.fn() }),
  useLivez:        () => ({ data: null, isFetching: false, refetch: vi.fn() }),
}));

vi.mock('@/api/queries/playlist', () => ({
  usePlaylist:       () => ({ data: [], isLoading: false }),
  useAppendPlaylist: () => ({ mutate: vi.fn() }),
  useDeletePlaylistItem: () => ({ mutate: vi.fn() }),
  useClearPlaylist:  () => ({ mutate: vi.fn() }),
  useSchedule:       () => ({ data: [] }),
  useWatcherStatus:  () => ({ data: null }),
  useRescan:         () => ({ mutate: vi.fn(), isPending: false }),
  usePlaybackLog:    () => ({ data: { items: [] } }),
  usePlaybackLogStatus: () => ({ data: null }),
}));

vi.mock('@/hooks/useEventStream', () => ({
  EventBusProvider: ({ children }: { children: React.ReactNode }) => <>{children}</>,
  useEventStream: () => ({ events: [], connected: false, overflowed: false }),
}));

vi.mock('@/hooks/useToast', () => ({
  useToast: () => vi.fn(),
}));

// ─── Helpers ──────────────────────────────────────────────────────────────────
function createQC() {
  return new QueryClient({ defaultOptions: { queries: { retry: false } } });
}

function wrap(ui: React.ReactElement, initialPath = '/') {
  return (
    <QueryClientProvider client={createQC()}>
      <MemoryRouter initialEntries={[initialPath]}>
        {ui}
      </MemoryRouter>
    </QueryClientProvider>
  );
}

// ─── Tests ────────────────────────────────────────────────────────────────────

describe('DashboardPage', () => {
  it('renders without crashing', async () => {
    const { default: DashboardPage } = await import('@/features/channels/DashboardPage');
    render(wrap(<DashboardPage />));
    expect(screen.getByText('dashboard.stats.total')).toBeDefined();
  });

  it('shows stats cards', async () => {
    const { default: DashboardPage } = await import('@/features/channels/DashboardPage');
    render(wrap(<DashboardPage />));
    expect(screen.getByText('dashboard.stats.total')).toBeDefined();
  });
});

describe('UsersPage', () => {
  it('renders without crashing', async () => {
    const { default: UsersPage } = await import('@/features/settings/UsersPage');
    render(wrap(<UsersPage />));
    expect(screen.getByText('users.title')).toBeDefined();
  });
});

describe('LdapPage', () => {
  it('renders without crashing', async () => {
    const { default: LdapPage } = await import('@/features/settings/LdapPage');
    render(wrap(<LdapPage />));
    expect(screen.getByText('ldap.title')).toBeDefined();
  });
});

describe('AuditPage', () => {
  it('renders without crashing', async () => {
    const { default: AuditPage } = await import('@/features/settings/AuditPage');
    render(wrap(<AuditPage />));
    expect(screen.getByText('audit.title')).toBeDefined();
  });
});

describe('GatewaysListPage', () => {
  it('renders empty state', async () => {
    const { default: GatewaysListPage } = await import('@/features/gateways/GatewaysListPage');
    render(wrap(<GatewaysListPage />));
    expect(screen.getByText('Gateways')).toBeDefined();
  });
});

describe('PluginsListPage', () => {
  it('renders without crashing', async () => {
    const { default: PluginsListPage } = await import('@/features/plugins/PluginsListPage');
    render(wrap(<PluginsListPage />));
    expect(screen.getByText('Plugins')).toBeDefined();
  });
});

describe('MetricsPage', () => {
  it('renders without crashing', async () => {
    const { default: MetricsPage } = await import('@/features/observability/MetricsPage');
    render(wrap(<MetricsPage />));
    expect(screen.getByText('Metrics')).toBeDefined();
  });
});

describe('StressOverviewPage', () => {
  it('renders without crashing', async () => {
    const { default: StressOverviewPage } = await import('@/features/operations/StressOverviewPage');
    render(wrap(<StressOverviewPage />));
    expect(screen.getByText('stress.title')).toBeDefined();
  });
});
