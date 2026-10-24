/**
 * Commands to screens and the jobs that carry them. The server is replaced at the hooks;
 * what is checked is what the person is told before anything is sent, that the typed
 * confirmation really gates the send, and that what the server refuses is said in words.
 */
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { ApiClientError } from '@/api/client';
import type { BulkPlan, BulkReport, CommandJob } from '@/api/queries/players';

const toast = vi.fn();
vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn((selector: (s: object) => unknown) => selector({ role: 'admin', user: { id: 1, username: 'a', role: 'admin' } })),
}));
vi.mock('@/stores/ui', () => ({
  useUiStore: vi.fn((selector: (s: object) => unknown) => selector({ addToast: toast })),
}));

const plan = vi.fn();
const send = vi.fn();
const one = vi.fn();
const cancel = vi.fn();
const jobsState: { data?: CommandJob[]; isLoading: boolean; isError: boolean } = { isLoading: false, isError: false };
const jobState: { data?: CommandJob; isLoading: boolean; isError: boolean; error?: unknown } = { isLoading: false, isError: false };

vi.mock('@/api/queries/players', async (orig) => {
  const real = await orig<typeof import('@/api/queries/players')>();
  return {
    ...real,
    useBulkPlan: () => ({ mutateAsync: plan, isPending: false }),
    useBulkSend: () => ({ mutateAsync: send, isPending: false }),
    useScreenCommand: () => ({ mutateAsync: one, isPending: false }),
    useCommandJobs: () => jobsState,
    useCommandJob: () => jobState,
    useCancelCommandJob: () => ({ mutateAsync: cancel, isPending: false }),
    usePlayerTree: () => ({ data: [], isLoading: false, isError: false }),
  };
});

import { BulkCommandDialog, ScreenCommandDialog } from '@/features/players/CommandDialogs';
import { typesForTarget, phraseMatches } from '@/features/players/commands';
import JobsPage from '@/features/players/JobsPage';
import JobDetailPage from '@/features/players/JobDetailPage';

const PHRASE = 'restart_player group Shop / Tills';
const planOf = (over: Partial<BulkPlan> = {}): BulkPlan => ({
  target_name: 'Shop / Tills', required_phrase: PHRASE, expected_count: 6, online: 5, offline: ['x'], busy: [], ...over,
});
const reportOf = (over: Partial<BulkReport> = {}): BulkReport => ({
  type: 'restart_player', target: 'Shop / Tills', sent: [{ player_id: 'a', command_id: 'c' }, { player_id: 'b', command_id: 'd' }],
  skipped_offline: ['x'], failed: [], job_id: 'job-1', queued: 3, batch_size: 2, ...over,
});

const renderBulk = (types: ('restart_player' | 'reboot_screen' | 'reload_playlist')[] = ['restart_player', 'reboot_screen', 'reload_playlist']) => {
  const onClose = vi.fn();
  render(<MemoryRouter><BulkCommandDialog target={{ kind: 'group', id: 11 }} targetLabel="Shop / Tills" types={types} onClose={onClose} /></MemoryRouter>);
  return onClose;
};

beforeEach(() => {
  toast.mockClear(); plan.mockReset(); send.mockReset(); one.mockReset(); cancel.mockReset();
  jobsState.data = undefined; jobsState.isLoading = false; jobsState.isError = false;
  jobState.data = undefined; jobState.isLoading = false; jobState.isError = false; jobState.error = undefined;
});

// ═════════════════════ who may send what ══════════════════════════════════════
describe('typesForTarget', () => {
  const ALL = ['restart_player', 'reboot_screen', 'reload_playlist'];
  it('gives a group every command with the mark, and an operator without it only the playlist', () => {
    expect(typesForTarget({ kind: 'group', id: 1 }, 'operator', { commandGroup: true })).toEqual(ALL);
    expect(typesForTarget({ kind: 'group', id: 1 }, 'operator', { commandGroup: false })).toEqual(['reload_playlist']);
  });
  it('gives an object commands only with the object mark, which the group mark does not replace', () => {
    expect(typesForTarget({ kind: 'object', id: 1 }, 'operator', { commandObject: true })).toEqual(ALL);
    expect(typesForTarget({ kind: 'object', id: 1 }, 'operator', { commandGroup: true })).toEqual([]);
  });
  it('gives "everyone" to administrators alone', () => {
    expect(typesForTarget({ kind: 'all' }, 'admin', {})).toEqual(ALL);
    expect(typesForTarget({ kind: 'all' }, 'operator', { commandObject: true, commandGroup: true })).toEqual([]);
  });
  it('gives a viewer nothing, whatever the marks', () => {
    expect(typesForTarget({ kind: 'group', id: 1 }, 'viewer', { commandGroup: true })).toEqual([]);
    expect(typesForTarget({ kind: 'all' }, null, {})).toEqual([]);
  });
});

describe('phraseMatches', () => {
  it('ignores case and spaces, as the server does, and nothing else', () => {
    expect(phraseMatches('  RESTART_player   group shop/tills ', PHRASE)).toBe(true);
    expect(phraseMatches('restart_player group Shop / Till', PHRASE)).toBe(false);
    expect(phraseMatches('', PHRASE)).toBe(false);
  });
});

// ═════════════════════ a group, an object, everyone ═══════════════════════════
describe('BulkCommandDialog', () => {
  it('first only checks: it names the target and asks the server what would happen', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf());
    renderBulk();
    expect(screen.getByText(/Nothing is sent until you confirm/)).toBeInTheDocument();
    await user.click(screen.getByRole('radio', { name: /Reboot the screen/ }));
    await user.click(screen.getByRole('button', { name: 'Check' }));
    expect(plan).toHaveBeenCalledWith({ target: { kind: 'group', id: 11 }, type: 'reboot_screen' });
    expect(send).not.toHaveBeenCalled();
    expect(screen.getByText('5 screens that are online now will get the command.')).toBeInTheDocument();
    expect(screen.getByText('1 screen is offline: it will not get the command.')).toBeInTheDocument();
  });

  it('keeps Send off until the exact phrase is typed, then sends it with the count it was shown', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf());
    send.mockResolvedValue(reportOf());
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    const sendBtn = screen.getByRole('button', { name: 'Send' });
    expect(sendBtn).toBeDisabled();
    expect(screen.getByText(PHRASE)).toBeInTheDocument();
    await user.type(screen.getByLabelText('To confirm, type the phrase'), 'restart_player group Shop / Till');
    expect(sendBtn).toBeDisabled();
    await user.type(screen.getByLabelText('To confirm, type the phrase'), 's');
    expect(sendBtn).toBeEnabled();
    await user.click(sendBtn);
    expect(send).toHaveBeenCalledWith({ target: { kind: 'group', id: 11 }, type: 'restart_player', confirm_phrase: PHRASE, expected_count: 6 });
  });

  it('reports what went out, what waits in the queue and what was skipped, with a way to follow the job', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf());
    send.mockResolvedValue(reportOf());
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    await user.type(screen.getByLabelText('To confirm, type the phrase'), PHRASE);
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(screen.getByText(/Sent: 2 screens/)).toBeInTheDocument();
    expect(screen.getByText(/3 more are queued \(in batches of 2\)/)).toBeInTheDocument();
    expect(screen.getByText('Offline, skipped: 1 screen.')).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Follow the progress' })).toHaveAttribute('href', '/players/jobs/job-1');
  });

  it('refuses to go on while screens still wait for the same command, and names them', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf({ busy: [{ player_id: 'p1', name: 'Till 3', command_id: 'c' }] }));
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    expect(screen.getByRole('alert')).toHaveTextContent('1 screen is still waiting for the same command');
    expect(screen.getByRole('alert')).toHaveTextContent('Till 3');
    expect(screen.queryByLabelText('To confirm, type the phrase')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  });

  it('says so when no screen is online, and offers no way to send', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf({ online: 0, expected_count: 1 }));
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Nobody to send to');
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  });

  it('goes back to checking, with the new number, when the screens changed while the person typed', async () => {
    const user = userEvent.setup();
    plan.mockResolvedValue(planOf());
    send.mockRejectedValue(new ApiClientError(409, 'expected_count_mismatch', undefined, { error: 'expected_count_mismatch', actual: 8 }));
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    await user.type(screen.getByLabelText('To confirm, type the phrase'), PHRASE);
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(screen.getByRole('alert')).toHaveTextContent('changed while you were confirming (now 8)');
    expect(screen.getByRole('button', { name: 'Check' })).toBeInTheDocument();       // back to the first step
  });

  it('offers only the commands it was given', () => {
    renderBulk(['reload_playlist']);
    expect(screen.getAllByRole('radio')).toHaveLength(1);
    expect(screen.getByRole('radio', { name: /Reload the playlist/ })).toBeChecked();
  });

  it('says in words when the server refuses to check', async () => {
    const user = userEvent.setup();
    plan.mockRejectedValue(new ApiClientError(403, 'forbidden'));
    renderBulk();
    await user.click(screen.getByRole('button', { name: 'Check' }));
    expect(screen.getByRole('alert')).toHaveTextContent('no right to send this command here');
  });
});

// ═════════════════════ one screen ═════════════════════════════════════════════
describe('ScreenCommandDialog', () => {
  const renderOne = (online: boolean) => {
    const onClose = vi.fn();
    render(<MemoryRouter><ScreenCommandDialog player={{ id: 'p1', name: 'Till 1' }} online={online} onClose={onClose} /></MemoryRouter>);
    return onClose;
  };

  it('sends the chosen command at once and closes', async () => {
    const user = userEvent.setup();
    one.mockResolvedValue({});
    const onClose = renderOne(true);
    await user.selectOptions(screen.getByLabelText('What to do'), 'reload_playlist');
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(one).toHaveBeenCalledWith({ id: 'p1', type: 'reload_playlist' });
    expect(toast).toHaveBeenCalledWith(expect.stringContaining('Command sent'), 'success');
    expect(onClose).toHaveBeenCalled();
  });

  it('warns that a command to an offline screen is lost if it does not come back', () => {
    renderOne(false);
    expect(screen.getByRole('status')).toHaveTextContent('is lost if the screen does not come back');
  });

  it('says in words that the same command still waits, and stays open', async () => {
    const user = userEvent.setup();
    one.mockRejectedValue(new ApiClientError(409, 'command_in_progress'));
    const onClose = renderOne(true);
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(screen.getByRole('alert')).toHaveTextContent('still waiting for an answer');
    expect(onClose).not.toHaveBeenCalled();
  });
});

// ═════════════════════ jobs ═══════════════════════════════════════════════════
const job = (over: Partial<CommandJob> = {}): CommandJob => ({
  id: 'job-1', type: 'restart_player', target: { kind: 'group', id: 11, name: 'Shop / Tills' }, state: 'running', stop_reason: '',
  batch_size: 2, stop_percent: 30, created_at: 1_800_000_000, created_by_name: 'ann',
  summary: { total: 6, queued: 1, pending: 2, executed: 2, unconfirmed: 0, failed: 0, skipped_offline: 1, cancelled: 0 },
  ...over,
});

describe('JobsPage', () => {
  it('lists the jobs with who started them and how far they got, not counting screens that were offline', () => {
    jobsState.data = [job(), job({ id: 'job-2', type: 'reboot_screen', target: { kind: 'all', id: 0, name: '' }, state: 'stopped' })];
    render(<MemoryRouter><JobsPage /></MemoryRouter>);
    expect(screen.getAllByText('Confirmed 2 of 5')).toHaveLength(2);       // 6 screens, 1 was offline
    expect(screen.getByText('Running')).toBeInTheDocument();
    expect(screen.getByText('Stopped')).toBeInTheDocument();
    expect(screen.getByText('every screen')).toBeInTheDocument();
    expect(screen.getAllByText('ann')).toHaveLength(2);
  });

  it('says there are no jobs yet', () => {
    jobsState.data = [];
    render(<MemoryRouter><JobsPage /></MemoryRouter>);
    expect(screen.getByText('No jobs yet')).toBeInTheDocument();
  });

  it('shows a plain failure when the jobs cannot be loaded', () => {
    jobsState.isError = true;
    render(<MemoryRouter><JobsPage /></MemoryRouter>);
    expect(screen.getByRole('alert')).toHaveTextContent('Could not load the jobs.');
  });
});

describe('JobDetailPage', () => {
  const renderDetail = () => render(
    <MemoryRouter initialEntries={['/players/jobs/job-1']}>
      <Routes><Route path="/players/jobs/:id" element={<JobDetailPage />} /></Routes>
    </MemoryRouter>,
  );

  it('shows each screen by name and batch with what it answered, in the order of the batches', () => {
    jobState.data = job({ items: [
      { player_id: 'b', player_name: 'Till 2', batch: 1, state: 'sent', command_state: 'pending' },
      { player_id: 'a', player_name: 'Till 1', batch: 0, state: 'sent', command_state: 'executed', command_result: 'executed' },
      { player_id: 'c', player_name: 'Till 3', batch: -1, state: 'skipped_offline' },
      { player_id: 'd', player_name: 'Till 4', batch: 2, state: 'queued' },
    ] });
    renderDetail();
    const rows = screen.getAllByRole('row').slice(1).map(r => r.textContent);
    expect(rows[0]).toContain('Till 3');                         // -1: not connected at the start, first
    expect(rows[0]).toContain('offline at the start');
    expect(rows[1]).toContain('Till 1');
    expect(rows[1]).toContain('done');
    expect(rows[3]).toContain('waiting for its turn');
  });

  it('says plainly that the job stopped because too many did not answer', () => {
    jobState.data = job({ state: 'stopped', stop_reason: 'too_many_unconfirmed', items: [] });
    renderDetail();
    expect(screen.getByRole('alert')).toHaveTextContent('more than 30 % of the last batch did not confirm');
    expect(screen.queryByRole('button', { name: 'Cancel the rest' })).not.toBeInTheDocument();
  });

  it('says plainly that the job stopped because the broker stayed away, and not that the screens did not answer', () => {
    jobState.data = job({ state: 'stopped', stop_reason: 'broker_unavailable', items: [] });
    renderDetail();
    expect(screen.getByRole('alert')).toHaveTextContent('could not reach the command broker for too long');
    expect(screen.queryByText(/did not confirm/)).not.toBeInTheDocument();
  });

  it('asks before cancelling the rest, and cancels only after the answer', async () => {
    const user = userEvent.setup();
    cancel.mockResolvedValue({});
    jobState.data = job({ items: [] });
    renderDetail();
    await user.click(screen.getByRole('button', { name: 'Cancel the rest' }));
    expect(cancel).not.toHaveBeenCalled();
    expect(screen.getByText(/already reached screens stays/)).toBeInTheDocument();
    const confirm = (() => { const all = screen.getAllByRole("button", { name: "Cancel the rest" }); return all[all.length - 1]; })();
    await user.click(confirm);
    expect(cancel).toHaveBeenCalledWith('job-1');
    expect(toast).toHaveBeenCalledWith(expect.stringContaining('The rest is cancelled'), 'success');
  });

  it('says "not found" for a job that is not the person\'s', () => {
    jobState.isError = true; jobState.error = new ApiClientError(404, 'not_found');
    renderDetail();
    expect(screen.getByRole('alert')).toHaveTextContent('Job not found.');
  });
});
