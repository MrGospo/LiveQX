/**
 * Verifies that the FPS dropdown offers low integer values (1, 2, 5, 10, 15)
 * in addition to the standard broadcast rates (24/25/30/50/60). Low rates
 * are needed for slideshow / static-image channels.
 */
import React from 'react';
import { render } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { describe, it, expect, vi } from 'vitest';

// i18next is initialized with real EN resources in src/test/setup.ts.

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

describe('CreateChannelPage FPS dropdown', () => {
  it('offers low integer FPS values for slideshow-style channels', async () => {
    const { default: CreateChannelPage } = await import('@/features/channels/CreateChannelPage');
    const { container } = render(wrap(<CreateChannelPage />));
    const fpsSelect = container.querySelector('select[name="fps"]') as HTMLSelectElement;
    expect(fpsSelect).toBeTruthy();
    const values = Array.from(fpsSelect.options).map((o) => Number(o.value));
    // Slideshow-friendly low rates and standard broadcast rates must all appear.
    for (const expected of [1, 2, 5, 10, 15, 24, 25, 30, 50, 60]) {
      expect(values).toContain(expected);
    }
    // No zero and no fractional strings.
    expect(values).not.toContain(0);
    for (const v of values) expect(Number.isInteger(v)).toBe(true);
  });
});
