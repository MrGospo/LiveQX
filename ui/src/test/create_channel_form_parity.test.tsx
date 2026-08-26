/**
 * Guards that the Create Channel wizard exposes the same audio, encoder and
 * MPEG-TS knobs that the Detail settings tab exposes. The wizard was missing
 * audio bitrate/sample_rate, max_b_frames, and most MPEG-TS identity/mux
 * fields — this test locks the parity down so we don't regress.
 *
 * MPEG-TS extras only render when output_type === 'multicast', mirroring the
 * conditional in CreateChannelPage.
 */
import React from 'react';
import { fireEvent, render } from '@testing-library/react';
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

describe('CreateChannelPage form parity with Detail', () => {
  it('exposes audio bitrate, sample rate, max_b_frames and gop_size on step 1', async () => {
    const { default: CreateChannelPage } = await import('@/features/channels/CreateChannelPage');
    const { container } = render(wrap(<CreateChannelPage />));
    // Step 1 renders by default.
    const audioBitrate = container.querySelector('input[name="audio_bitrate_kbps"]') as HTMLInputElement;
    const audioSr     = container.querySelector('select[name="audio_sample_rate"]') as HTMLSelectElement;
    const maxB        = container.querySelector('input[name="max_b_frames"]') as HTMLInputElement;
    const gop         = container.querySelector('input[name="gop_size"]') as HTMLInputElement;
    expect(audioBitrate).toBeTruthy();
    expect(audioSr).toBeTruthy();
    expect(maxB).toBeTruthy();
    expect(gop).toBeTruthy();
    // Defaults must match Encoder::Config: 128 kbps, 48 kHz, 0 B-frames, 0 (auto) GOP.
    expect(audioBitrate.value).toBe('128');
    expect(audioSr.value).toBe('48000');
    expect(maxB.value).toBe('0');
    expect(gop.value).toBe('0');
    // Sample-rate options must include both broadcast rates.
    const srValues = Array.from(audioSr.options).map(o => Number(o.value));
    expect(srValues).toContain(44100);
    expect(srValues).toContain(48000);
  });

  it('exposes MPEG-TS identity and mux fields on step 2 when output is multicast', async () => {
    const { default: CreateChannelPage } = await import('@/features/channels/CreateChannelPage');
    const { container, getByText } = render(wrap(<CreateChannelPage />));
    // Advance to Step 2.
    fireEvent.click(getByText(/next/i));
    // Switch output type to multicast so MPEG-TS block renders.
    const outputType = container.querySelector('select[name="output_type"]') as HTMLSelectElement;
    fireEvent.change(outputType, { target: { value: 'multicast' } });

    for (const name of [
      'mpegts_service_id',
      'mpegts_service_name',
      'mpegts_service_provider',
      'mpegts_transport_stream_id',
      'mpegts_original_network_id',
      'mpegts_mux_rate_kbps',
      'mpegts_sdt_period_ms',
      'mpegts_pat_period_ms',
    ]) {
      expect(container.querySelector(`[name="${name}"]`)).toBeTruthy();
    }
    // Defaults must match Encoder::Config so cfg.json stays honest.
    const provider = container.querySelector('input[name="mpegts_service_provider"]') as HTMLInputElement;
    const tsid     = container.querySelector('input[name="mpegts_transport_stream_id"]') as HTMLInputElement;
    const onid     = container.querySelector('input[name="mpegts_original_network_id"]') as HTMLInputElement;
    const muxRate  = container.querySelector('input[name="mpegts_mux_rate_kbps"]') as HTMLInputElement;
    const sdt      = container.querySelector('input[name="mpegts_sdt_period_ms"]') as HTMLInputElement;
    const pat      = container.querySelector('input[name="mpegts_pat_period_ms"]') as HTMLInputElement;
    expect(provider.value).toBe('LiveQX');
    expect(tsid.value).toBe('1');
    expect(onid.value).toBe('1');
    expect(muxRate.value).toBe('0');
    expect(sdt.value).toBe('0');
    expect(pat.value).toBe('0');
  });
});
