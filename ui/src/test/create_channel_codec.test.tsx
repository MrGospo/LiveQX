/**
 * Verifies that the H.264 preset dropdown is only rendered when the
 * selected video codec is H.264. For MPEG-2 the preset is a no-op
 * (Mpeg2VideoEncoder discards any x264 preset), so the UI hides it and
 * shows an explanatory note instead.
 */
import React from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { describe, it, expect, vi } from 'vitest';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({
    t: (key: string) => key,
    i18n: { language: 'en', changeLanguage: vi.fn() },
  }),
  initReactI18next: { type: '3rdParty', init: vi.fn() },
}));

vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({
    user: { id: 1, username: 'admin', role: 'admin' },
    role: 'admin', hasRole: () => true,
    canAccessChannel: () => true,
    logout: vi.fn(),
  })),
}));

vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({
    sidebarCollapsed: false, toasts: [],
    ldapDown: false, sseConnected: true, sseOverflowed: false,
    toggleSidebar: vi.fn(), addToast: vi.fn(), removeToast: vi.fn(),
  })),
}));

vi.mock('@/hooks/useToast', () => ({ useToast: () => vi.fn() }));

vi.mock('@/api/queries/channels', () => ({
  useCreateChannel: () => ({ mutateAsync: vi.fn(), isPending: false }),
}));

vi.mock('@/api/queries/system', () => ({
  useGpuInfo: () => ({ data: {} }),
  useNetworkInterfaces: () => ({ data: [], isLoading: false }),
}));

function wrap(ui: React.ReactElement) {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false } } });
  return (
    <QueryClientProvider client={qc}>
      <MemoryRouter initialEntries={['/channels/new']}>{ui}</MemoryRouter>
    </QueryClientProvider>
  );
}

describe('CreateChannelPage preset field', () => {
  it('shows the preset dropdown for H.264 (default)', async () => {
    const { default: CreateChannelPage } = await import('@/features/channels/CreateChannelPage');
    const { container } = render(wrap(<CreateChannelPage />));
    // preset dropdown is a <select> containing the x264 preset options.
    expect(screen.getByText('ultrafast')).toBeDefined();
    expect(screen.getByText('veryslow')).toBeDefined();
    // The MPEG-2 note must NOT be present.
    expect(screen.queryByText('channels.config.presetNotApplicableMpeg2')).toBeFalsy();
    // Sanity: the preset select is registered.
    expect(container.querySelector('select[name="preset"]')).toBeTruthy();
  });

  it('hides the preset dropdown and shows a note for MPEG-2', async () => {
    const { default: CreateChannelPage } = await import('@/features/channels/CreateChannelPage');
    const { container } = render(wrap(<CreateChannelPage />));
    // Switch codec to MPEG-2.
    const codecSelect = container.querySelector('select[name="video_codec"]') as HTMLSelectElement;
    expect(codecSelect).toBeTruthy();
    fireEvent.change(codecSelect, { target: { value: 'mpeg2video' } });
    // x264 preset options must be gone.
    expect(screen.queryByText('ultrafast')).toBeFalsy();
    expect(screen.queryByText('veryslow')).toBeFalsy();
    expect(container.querySelector('select[name="preset"]')).toBeFalsy();
    // Explanatory note takes their place.
    expect(screen.queryByText('channels.config.presetNotApplicableMpeg2')).toBeTruthy();
  });
});
