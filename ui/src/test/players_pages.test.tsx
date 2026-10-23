/**
 * Screens (players): module settings and broker pages.
 *
 * What these protect:
 *   * only what was changed is sent, so one edit cannot reset another setting;
 *   * a value outside the server's limits never leaves the browser, and the
 *     server's own refusals land on the field they are about;
 *   * the broker page says in words what each state means and what is missing;
 *   * a new broker address is tried first: a failed try changes nothing, and
 *     saving without trying is a separate, deliberate tick.
 */
import React from 'react';
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { PlayersSettings, BrokerStatus } from '@/api/queries/players';

// ─── Stores ───────────────────────────────────────────────────────────────────
const toast = vi.fn();
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role: 'admin', user: { id: 1, username: 'admin', role: 'admin' } })),
}));
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({ addToast: toast })),
}));

// ─── Queries (replaced per test) ──────────────────────────────────────────────
const settingsState: { data?: PlayersSettings; isLoading: boolean; isError: boolean } = { isLoading: false, isError: false };
const save = vi.fn();
const brokerState: { data?: BrokerStatus; isLoading: boolean; isError: boolean } = { isLoading: false, isError: false };
const update = vi.fn();
const reconcile = vi.fn();
let updateVariables: unknown;

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    usePlayersSettings: () => settingsState,
    useSavePlayersSettings: () => ({ mutateAsync: save, isPending: false }),
    useBrokerStatus: () => brokerState,
    useBrokerUpdate: () => ({ mutateAsync: update, isPending: false, variables: updateVariables }),
    useBrokerReconcile: () => ({ mutateAsync: reconcile, isPending: false }),
    usePlayerTree: () => ({ data: [], isLoading: false, isError: false }),
  };
});

import PlayersSettingsPage from '@/features/players/PlayersSettingsPage';
import BrokerPage from '@/features/players/BrokerPage';

const baseSettings: PlayersSettings = {
  server_url: 'https://liveqx.example.lan', device_api_url: 'https://liveqx.example.lan:8444',
  mqtt_broker_url: 'mqtts://liveqx.example.lan:8883', ca_cert_pem: '',
  token_ttl_days: 7, clock_policy: 'strict',
  command_ttl_s: 300, confirm_timeout_restart_s: 300, confirm_timeout_reboot_s: 1200, confirm_timeout_reload_s: 120,
  command_batch_size: 10, command_batch_stop_percent: 30,
  events_retention_days: 90, commands_retention_days: 365,
};

const nowSec = () => Math.floor(Date.now() / 1000);
const baseBroker = (over: Partial<BrokerStatus> = {}): BrokerStatus => ({
  enabled: true, state: 'connected', detail: '',
  host: '127.0.0.1', port: 8883, verify_hostname: true, reconcile_interval_s: 300,
  last_reconcile_at: nowSec() - 120, last_reconcile_ok: true,
  last_report: { issued: 2, removed: 1, failed: 0, error: '' },
  crl: { mode: 'off' },
  readiness: { mosquitto_in_build: true, device_ca: true, trust_root: true, broker_url_configured: true },
  ...over,
});

const renderPage = (ui: React.ReactElement) => render(<MemoryRouter>{ui}</MemoryRouter>);

beforeEach(() => {
  toast.mockClear(); save.mockReset(); update.mockReset(); reconcile.mockReset();
  updateVariables = undefined;
  settingsState.data = { ...baseSettings }; settingsState.isLoading = false; settingsState.isError = false;
  brokerState.data = baseBroker(); brokerState.isLoading = false; brokerState.isError = false;
});

// ═════════════════════════════ settings ═══════════════════════════════════════
describe('PlayersSettingsPage', () => {
  it('warns that no screen can be paired while the three addresses are missing', () => {
    settingsState.data = { ...baseSettings, server_url: '', mqtt_broker_url: '' };
    renderPage(<PlayersSettingsPage />);
    expect(screen.getByText('The addresses are not set yet')).toBeInTheDocument();
  });

  it('shows no warning once the addresses are set', () => {
    renderPage(<PlayersSettingsPage />);
    expect(screen.queryByText('The addresses are not set yet')).not.toBeInTheDocument();
  });

  it('keeps Save off until something is changed, then sends only the changed setting', async () => {
    const user = userEvent.setup();
    save.mockResolvedValue({ ...baseSettings, token_ttl_days: 14 });
    renderPage(<PlayersSettingsPage />);
    const saveBtn = screen.getByRole('button', { name: 'Save' });
    expect(saveBtn).toBeDisabled();

    const ttl = screen.getByLabelText('Pairing token lifetime, days');
    await user.clear(ttl);
    await user.type(ttl, '14');
    expect(saveBtn).toBeEnabled();
    await user.click(saveBtn);
    expect(save).toHaveBeenCalledTimes(1);
    expect(save).toHaveBeenCalledWith({ token_ttl_days: 14 });
  });

  it('never sends a number outside the limits and says what the limits are', async () => {
    const user = userEvent.setup();
    renderPage(<PlayersSettingsPage />);
    const batch = screen.getByLabelText('Batch size for a command to many screens');
    await user.clear(batch);
    await user.type(batch, '500');
    expect(screen.getByText('From 1 to 100')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(save).not.toHaveBeenCalled();
    expect(toast).toHaveBeenCalledWith('Fix the marked fields', 'danger');
  });

  it('refuses an empty number rather than sending nothing for it', async () => {
    const user = userEvent.setup();
    renderPage(<PlayersSettingsPage />);
    await user.clear(screen.getByLabelText('Command lifetime, s'));
    expect(screen.getByText('Enter a number')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(save).not.toHaveBeenCalled();
  });

  it('wants the right kind of address in each field', async () => {
    const user = userEvent.setup();
    renderPage(<PlayersSettingsPage />);
    const broker = screen.getByLabelText('Command broker address (MQTT)');
    await user.clear(broker);
    await user.type(broker, 'http://wrong');
    expect(screen.getByText('The address must start with mqtts://')).toBeInTheDocument();
  });

  it('puts the server\'s own refusal on the field it is about', async () => {
    const user = userEvent.setup();
    save.mockRejectedValue(new ApiClientError(400, 'invalid_server_url'));
    renderPage(<PlayersSettingsPage />);
    const url = screen.getByLabelText('Server address (pairing and administration)');
    await user.clear(url);
    await user.type(url, 'https://other.example.lan');
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(await screen.findByText(/did not accept the address/)).toBeInTheDocument();
  });

  it('shows seconds in plain units beside the field', () => {
    renderPage(<PlayersSettingsPage />);
    expect(screen.getAllByText('≈ 5 min').length).toBeGreaterThanOrEqual(2);   // 300 s: lifetime and restart
    expect(screen.getByText('≈ 20 min')).toBeInTheDocument();                  // 1200 s: reboot
  });

  it('warns about the cost of the relaxed clock check', async () => {
    const user = userEvent.setup();
    renderPage(<PlayersSettingsPage />);
    expect(screen.queryByRole('note')).not.toBeInTheDocument();
    await user.selectOptions(screen.getByLabelText('Clock check on new screens'), 'relaxed');
    expect(screen.getByRole('note')).toHaveTextContent(/expired certificate/);
  });

  it('says so when the settings cannot be loaded', () => {
    settingsState.data = undefined; settingsState.isError = true;
    renderPage(<PlayersSettingsPage />);
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the settings.');
  });
});

// ═════════════════════════════ broker ═════════════════════════════════════════
describe('BrokerPage', () => {
  it('says in words that a connected broker means commands work', () => {
    renderPage(<BrokerPage />);
    expect(screen.getByText('Connected')).toBeInTheDocument();
    expect(screen.getByText(/commands go out/)).toBeInTheDocument();
    expect(screen.getByText('Succeeded')).toBeInTheDocument();
    expect(screen.getByText('Records issued: 2, removed: 1, failed: 0')).toBeInTheDocument();
  });

  it('names what is missing and what to do about it', () => {
    brokerState.data = baseBroker({
      state: 'waiting', detail: 'the device CA is not set up yet',
      readiness: { mosquitto_in_build: true, device_ca: false, trust_root: true, broker_url_configured: false },
    });
    renderPage(<BrokerPage />);
    expect(screen.getByText('Waiting')).toBeInTheDocument();
    expect(screen.getByText('the device CA is not set up yet')).toBeInTheDocument();
    expect(screen.getByText(/created once when the server is prepared/)).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Open settings' })).toHaveAttribute('href', '/players/settings');
    // What is fine has nothing to fix.
    expect(screen.queryByText(/built without the MQTT library/)).not.toBeInTheDocument();
  });

  it('when the link is off, shows the setup but offers no way to change or nudge it', () => {
    brokerState.data = baseBroker({ enabled: false, state: 'disabled', last_reconcile_at: 0, last_report: undefined });
    renderPage(<BrokerPage />);
    expect(screen.getByText('Off')).toBeInTheDocument();
    expect(screen.getByLabelText('Broker address')).toBeDisabled();
    expect(screen.queryByRole('button', { name: 'Test the connection' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Reconcile now' })).toBeDisabled();
    expect(screen.getByText('No reconciliation yet.')).toBeInTheDocument();
  });

  it('asks for a reconciliation', async () => {
    const user = userEvent.setup();
    reconcile.mockResolvedValue({ requested: true });
    renderPage(<BrokerPage />);
    await user.click(screen.getByRole('button', { name: 'Reconcile now' }));
    expect(reconcile).toHaveBeenCalledTimes(1);
    expect(toast).toHaveBeenCalledWith('Reconciliation requested', 'info');
  });

  it('only tests the connection when asked to test, and reports the answer', async () => {
    const user = userEvent.setup();
    update.mockResolvedValue({ checked: true, ok: false, detail: 'no answer from the broker in time',
                               connection: { host: 'broker.lan', port: 8883, verify_hostname: true, reconcile_interval_s: 300 } });
    renderPage(<BrokerPage />);
    const host = screen.getByLabelText('Broker address');
    await user.clear(host);
    await user.type(host, 'broker.lan');
    await user.click(screen.getByRole('button', { name: 'Test the connection' }));
    expect(update).toHaveBeenCalledWith(expect.objectContaining({ host: 'broker.lan', check_only: true }));
    expect(await screen.findByRole('status')).toHaveTextContent('Could not connect: no answer from the broker in time');
  });

  it('sends only the changed field when saving', async () => {
    const user = userEvent.setup();
    update.mockResolvedValue({ applied: true, checked: true,
                               connection: { host: '127.0.0.1', port: 8884, verify_hostname: true, reconcile_interval_s: 300 } });
    renderPage(<BrokerPage />);
    const port = screen.getByLabelText('Port');
    await user.clear(port);
    await user.type(port, '8884');
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(update).toHaveBeenCalledWith({ port: 8884 });
    expect(toast).toHaveBeenCalledWith('Connection changed; the server is reconnecting', 'success');
  });

  it('keeps the old address when the new one does not answer, and lets the administrator save anyway on purpose', async () => {
    const user = userEvent.setup();
    update.mockRejectedValueOnce(new ApiClientError(502, 'probe_failed', 'no answer from the broker in time'));
    renderPage(<BrokerPage />);
    const host = screen.getByLabelText('Broker address');
    await user.clear(host);
    await user.type(host, 'new-broker.lan');
    await user.click(screen.getByRole('button', { name: 'Save' }));

    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent('The broker did not answer at the new address');
    expect(alert).toHaveTextContent('Nothing was changed');
    expect(alert).toHaveTextContent('no answer from the broker in time');

    update.mockResolvedValueOnce({ applied: true, checked: false,
                                   connection: { host: 'new-broker.lan', port: 8883, verify_hostname: true, reconcile_interval_s: 300 } });
    await user.click(within(alert).getByRole('checkbox'));
    await user.click(screen.getByRole('button', { name: 'Save' }));
    expect(update).toHaveBeenLastCalledWith({ host: 'new-broker.lan', skip_check: true });
  });

  it('never ticks "skip the check" by itself', async () => {
    const user = userEvent.setup();
    update.mockRejectedValueOnce(new ApiClientError(502, 'probe_failed', 'x'));
    renderPage(<BrokerPage />);
    await user.type(screen.getByLabelText('Broker address'), 'x');
    await user.click(screen.getByRole('button', { name: 'Save' }));
    const alert = await screen.findByRole('alert');
    expect(within(alert).getByRole('checkbox')).not.toBeChecked();
  });

  it('warns when the broker\'s name is no longer checked', async () => {
    const user = userEvent.setup();
    renderPage(<BrokerPage />);
    expect(screen.queryByText(/will not notice a broker being replaced/)).not.toBeInTheDocument();
    await user.click(screen.getByRole('switch'));
    expect(screen.getByRole('note')).toHaveTextContent(/will not notice a broker being replaced/);
  });

  it('refuses an address the server would not take, before asking it', async () => {
    const user = userEvent.setup();
    renderPage(<BrokerPage />);
    const host = screen.getByLabelText('Broker address');
    await user.clear(host);
    await user.type(host, 'bad host');
    expect(screen.getByText(/Letters, digits, dot/)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Test the connection' })).toBeDisabled();
  });

  describe('the revocation list', () => {
    it('explains what it means to have it off', () => {
      renderPage(<BrokerPage />);
      expect(screen.getByText(/The list is not passed to the broker/)).toBeInTheDocument();
    });

    it('shows a failed signal to the broker in plain words', () => {
      brokerState.data = baseBroker({ crl: {
        mode: 'local', last_ok: true, last_publish_at: nowSec() - 60, revoked: 3, generations: 2,
        valid_until: nowSec() + 20 * 86400, reload_error: 'process_not_running' } });
      renderPage(<BrokerPage />);
      expect(screen.getByText('The list is written')).toBeInTheDocument();
      expect(screen.getByText(/the broker process is not running/)).toBeInTheDocument();
      expect(screen.getByText(/revoked: 3, authority generations: 2/)).toBeInTheDocument();
    });

    it('calls the time before the setup ceremony waiting, not a failure', () => {
      brokerState.data = baseBroker({ crl: { mode: 'local', last_ok: false, waiting: true, error: '', reload_error: '' } });
      renderPage(<BrokerPage />);
      expect(screen.getByText(/Waiting for the device authority/)).toBeInTheDocument();
      expect(screen.queryByText('The list is not written')).not.toBeInTheDocument();
      expect(screen.queryByText(/close to its end/)).not.toBeInTheDocument();
    });

    it('says in words when the broker is never told about a new list because its process-id file is not set', () => {
      brokerState.data = baseBroker({ crl: { mode: 'local', last_ok: true, last_publish_at: nowSec() - 60,
        valid_until: nowSec() + 80 * 86400, reload_error: 'pid_file_not_configured', revoked: 0, generations: 1 } });
      renderPage(<BrokerPage />);
      expect(screen.getByText(/process-id file is not set in the server's settings/)).toBeInTheDocument();
    });

    it('warns loudly when the list is close to running out, since that would lock everyone out', () => {
      brokerState.data = baseBroker({ crl: {
        mode: 'local', last_ok: false, error: 'generation_1: cannot be opened',
        last_publish_at: nowSec() - 25 * 86400, valid_until: nowSec() + 2 * 86400 } });
      renderPage(<BrokerPage />);
      expect(screen.getByText(/close to its end/)).toBeInTheDocument();
      expect(screen.getByText('generation_1: cannot be opened')).toBeInTheDocument();
      expect(screen.getByText('The list is not written')).toBeInTheDocument();
    });
  });
});
