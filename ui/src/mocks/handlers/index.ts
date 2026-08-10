import { http, HttpResponse, delay } from 'msw';
import type {
  ChannelStatus, User, AuditEvent, GatewayStatus,
  PluginListing, StressStatus, StressReportSummary,
  LdapConfig, SmtpConfig, VersionInfo, SystemStatus,
  ProfileSnapshot, WatcherStatus, NetworkInterface, GpuInfo,
} from '@/api/types';
import { fix25Handlers } from './fix25';

const D = 200; // simulated network delay ms

// ─── Mock data ────────────────────────────────────────────────────────────────
//
// Все объекты ниже соответствуют openapi.yaml после A3.0 audit'а:
// поля и enum-значения отражают то что реально эмитит backend
// (см. fix/openapi_audit/deletions_log.md для истории drift'а).

// ChannelStatus.outputs → массив OutputStatus с обязательными полями
// id/queue_drops/queue_bytes_used/queue_bytes_limit/healthy + transport
// или mode='ndi'.
const SRT_OUT = (id: string, healthy: boolean) => ({
  id, transport: 'srt' as const, healthy,
  queue_drops: healthy ? 0 : 12,
  queue_bytes_used: healthy ? 0 : 4096,
  queue_bytes_limit: 1_000_000,
  bytes_sent: healthy ? 12_345_678 : 0,
  rtt_ms: healthy ? 23 : 0,
});
const RTMP_OUT = (id: string, healthy: boolean) => ({
  id, transport: 'rtmp' as const, healthy,
  queue_drops: 0,
  queue_bytes_used: 0,
  queue_bytes_limit: 1_000_000,
  bytes_sent: healthy ? 87_654_321 : 0,
  connected: healthy,
});
const HLS_OUT = (id: string) => ({
  id, transport: 'hls' as const, healthy: true,
  queue_drops: 0,
  queue_bytes_used: 0,
  queue_bytes_limit: 0,
  segments_present: 6,
  last_segment: 'segment_142.ts',
  last_segment_age_ms: 1200,
});

const CHANNELS: ChannelStatus[] = [
  {
    id: 1, name: 'sport-hd', state: 'running',
    resolution: '1920x1080', fps_target: 25, fps_actual: 25.0,
    numa_node: 0, bitrate: 4_200_000, preset: 'veryfast', max_b_frames: 2,
    outputs: [SRT_OUT('srt-main', true), RTMP_OUT('rtmp-yt', true)],
    content_source: { mode: 'cache', share_path: '/srv/share/sport', cache_path: '/var/lib/liveqx/cache/1' },
    current_clip_index: 0, current_clip_remaining_sec: 16.0,
    srt_connected: true,
  },
  {
    id: 2, name: 'news-4k', state: 'degraded',
    resolution: '3840x2160', fps_target: 25, fps_actual: 24.6,
    numa_node: 0, bitrate: 8_000_000, preset: 'veryfast', max_b_frames: 2,
    outputs: [SRT_OUT('srt-out', true), RTMP_OUT('rtmp-fb', false)],
    content_source: { mode: 'cache', share_path: '/srv/share/news', cache_path: '/var/lib/liveqx/cache/2' },
    current_clip_index: 3, current_clip_remaining_sec: 33.0,
    srt_connected: true,
  },
  {
    id: 3, name: 'studio-a', state: 'stopped',
    resolution: '1920x1080', fps_target: 25,
    numa_node: 1, bitrate: 2_000_000, preset: 'veryfast', max_b_frames: 0,
    outputs: [],
    content_source: { mode: 'cache', share_path: '/srv/share/studio', cache_path: '/var/lib/liveqx/cache/3' },
    current_clip_index: -1, current_clip_remaining_sec: 0,
    srt_connected: false,
  },
  {
    id: 4, name: 'events-live', state: 'running',
    resolution: '1920x1080', fps_target: 25, fps_actual: 25.0,
    numa_node: 1, bitrate: 6_000_000, preset: 'veryfast', max_b_frames: 2,
    outputs: [HLS_OUT('hls-out'), SRT_OUT('srt-bck', true)],
    content_source: { mode: 'passthrough', source_path: '/srv/share/events' },
    current_clip_index: 0, current_clip_remaining_sec: 0,
    srt_connected: true,
  },
  {
    id: 5, name: 'test-stream', state: 'failed',
    resolution: '1280x720', fps_target: 25,
    numa_node: 0, bitrate: 3_000_000, preset: 'veryfast', max_b_frames: 0,
    outputs: [SRT_OUT('srt-t', false)],
    content_source: { mode: 'cache', share_path: '/srv/share/test', cache_path: '/var/lib/liveqx/cache/5' },
    current_clip_index: -1, current_clip_remaining_sec: 0,
    srt_connected: false,
  },
];

let channels = [...CHANNELS];

const USERS: User[] = [
  { id: 1, username: 'admin',          email: 'admin@corp.local', role: 'admin',    source: 'local', disabled: false, must_change_password: false, failed_login_count: 0, created_at: 1743465600, last_login_at: Math.floor(Date.now()/1000) - 120,   last_login_ip: '10.0.0.5' },
  { id: 2, username: 'alice.operator', email: 'alice@corp.local', role: 'operator', source: 'local', disabled: false, must_change_password: false, failed_login_count: 0, created_at: 1743552000, last_login_at: Math.floor(Date.now()/1000) - 86400, last_login_ip: '10.0.0.12' },
  { id: 3, username: 'bob.viewer',     email: '',                 role: 'viewer',   source: 'ldap',  disabled: false, must_change_password: false, failed_login_count: 0, created_at: 1743638400, last_login_at: Math.floor(Date.now()/1000) - 10800, last_login_ip: '10.0.1.5' },
  { id: 4, username: 'carol.locked',   email: 'carol@corp.local', role: 'operator', source: 'local', disabled: false, must_change_password: false, failed_login_count: 5, locked_until: Math.floor(Date.now()/1000) + 600, created_at: 1743724800, last_login_at: Math.floor(Date.now()/1000) - 7200, last_login_ip: '192.168.1.22' },
  { id: 5, username: 'dave.disabled',  email: 'dave@corp.local',  role: 'viewer',   source: 'local', disabled: true,  must_change_password: false, failed_login_count: 0, created_at: 1743811200 },
];

// AuditEvent.details (parsed) или details_raw (fallback) — НЕ details_json.
const AUDIT: AuditEvent[] = [
  { id: 1, ts: Math.floor(Date.now()/1000) - 30,    event: 'login.ok',        username: 'admin',          ip: '10.0.0.5',  details: { source: 'local' } },
  { id: 2, ts: Math.floor(Date.now()/1000) - 120,   event: 'login.fail',      username: 'unknown',        ip: '10.0.0.99', details: { reason: 'bad_password' } },
  { id: 3, ts: Math.floor(Date.now()/1000) - 300,   event: 'user.create',     username: 'admin',          ip: '10.0.0.5',  details: { target: 'alice.operator' } },
  { id: 4, ts: Math.floor(Date.now()/1000) - 3600,  event: 'plugin.install',  username: 'admin',          ip: '10.0.0.5',  details: { name: 'ndi', sha256: '0a1b2c3d...' } },
  { id: 5, ts: Math.floor(Date.now()/1000) - 7200,  event: 'password.change', username: 'alice.operator', ip: '10.0.0.12' },
  { id: 6, ts: Math.floor(Date.now()/1000) - 86400, event: 'ldap.bind',       username: 'admin',          ip: '10.0.0.5',  details: { ok: true, latency_ms: 43 } },
];

// GatewayStatus per backend Gateway.cpp:334 — running/pkt_in/pkt_out/etc.
const GATEWAYS: GatewayStatus[] = [
  {
    id: 1, name: 'studio-relay', running: true,
    input:  { address: '239.1.1.1', port: 5000, recv_buffer_kb: 1024, interface: 'eth0' },
    outputs: [{ address: '239.2.2.2', port: 5000, ttl: 4, interface: 'eth1', send_buffer_kb: 256 }],
    pkt_in: 120045, bytes_in: 45_678_901,
    pkt_out: 120045, bytes_out: 45_678_901,
    drops: 0,
  },
  {
    id: 2, name: 'backup-out', running: false,
    input:  { address: '10.0.0.5', port: 9000, recv_buffer_kb: 1024 },
    outputs: [{ address: '239.3.3.3', port: 9000, ttl: 1, interface: 'eth0', send_buffer_kb: 256 }],
    pkt_in: 0, bytes_in: 0,
    pkt_out: 0, bytes_out: 0,
    drops: 0,
  },
];

const PLUGINS: PluginListing[] = [
  {
    name: 'ndi', version: '5.6.1',
    sha256: '0a1b2c3d4e5f6789abcdef0123456789abcdef01',
    installed_at: 1743465600,
    forced: false, eula_accepted: true, pending_unload: false,
    output_drivers: ['ndi'], input_drivers: ['ndi'],
  },
];

// StressStatus per StressService.cpp:220 — runner.state, не верхне-уровневый running.
const STRESS_STATUS: StressStatus = {
  enabled: true, schedule_cron: '0 2 * * *',
  scheduler_armed: true, report_dir: '/var/lib/liveqx/stress',
  last_report_id: '2026-05-07T02-00-00Z',
  runner: {
    state: 'idle',
    last_pass: true, last_verdict: 'ok',
    last_ended_ms: Date.now() - 3000_000,
  },
};

// StressReportSummary: id/started_at_ms/ended_at_ms/pass/verdict per backend.
const STRESS_REPORTS: StressReportSummary[] = [
  { id: '2026-05-07T02-00-00Z', started_at_ms: Date.now() - 3600_000,    ended_at_ms: Date.now() - 3000_000,    pass: true,  verdict: 'ok' },
  { id: '2026-05-06T02-00-00Z', started_at_ms: Date.now() - 3600_000*25, ended_at_ms: Date.now() - 3000_000*25, pass: false, verdict: 'fps_drop_pct=4.2 > 2' },
  { id: '2026-05-05T02-00-00Z', started_at_ms: Date.now() - 3600_000*49, ended_at_ms: Date.now() - 3000_000*49, pass: true,  verdict: 'aborted' },
];

// VersionInfo + BuildFeatures per MetricsCollector.cpp:419.
const VERSION: VersionInfo = {
  name: 'liveqx', version: '0.9.4', build_commit: '962fafa3',
  build_time: '2026-05-07T03:14:00Z', build_type: 'Release',
  features: {
    gpu: true, nvenc: true, qsv: false, vaapi: true,
    systemd: true, ldap: true, smtp: true,
    simd: 'avx2', preview: true, sse: true,
  },
  attributions: ['FFmpeg 6.1 (LGPL 2.1)', 'libsrt 1.5.3 (MPL 2.0)', 'OpenSSL 3.2 (Apache 2.0)'],
};

const GPU: GpuInfo = {
  nvenc: { built_in: true, codec_registered: true },
  qsv:   { built_in: false, codec_registered: false },
  vaapi: { built_in: true, codec_registered: true },
  x264:  { built_in: true, codec_registered: true },
};

// NetworkInterface — добавлено required `multicast`.
const INTERFACES: NetworkInterface[] = [
  { name: 'lo',    up: true,  loopback: true,  multicast: false, addresses: ['127.0.0.1'] },
  { name: 'eth0',  up: true,  loopback: false, multicast: true,  addresses: ['192.168.10.5'] },
  { name: 'eth1',  up: true,  loopback: false, multicast: true,  addresses: ['10.0.0.5'] },
  { name: 'bond0', up: false, loopback: false, multicast: true,  addresses: [] },
];

const PERF: ProfileSnapshot = {
  mode: 'sampling', running: true, channel_id: 1, active_ms: 12340,
  sampled_hits: { decode: 4300, compose: 2200, encode: 5100, output: 600, none: 140, total: 12340 },
  stage_us:     { decode: 4200000, compose: 1800000, encode: 5100000, output: 210000, total: 11310000 },
  stage_count:  { decode: 14750,   compose: 14750,   encode: 14750,   output: 4920,   total: 49170 },
};

// WatcherStatus per ContentSync.cpp:127 — mode=cache|passthrough.
const WATCHER: WatcherStatus = {
  channel_id: 1, mode: 'cache', source_path: '/mnt/share/sport',
  scan_interval_ms: 5000, numa_node: 0, current_backoff_ms: 0,
  running: true,
};

// LdapConfig (configured variant) per ldapConfigToJson() ControlApi.cpp:2063.
const LDAP: LdapConfig = {
  configured: true, enabled: true,
  server: 'ldaps://ad.corp.local:636', tls_mode: 'ldaps',
  base_dn: 'OU=Users,DC=corp,DC=local',
  bind_dn: 'CN=svc-streaming,OU=Service,DC=corp,DC=local',
  bind_password: '***', bind_password_set: true,
  user_filter: '(sAMAccountName=%s)',
  group_attribute: 'memberOf', email_attribute: 'mail',
  network_timeout_sec: 10, recheck_groups_on_refresh: true,
  group_role_map: {
    'CN=streaming-admins,OU=Groups,DC=corp,DC=local': 'admin',
    'CN=streaming-ops,OU=Groups,DC=corp,DC=local':    'operator',
  },
  channel_acl: [],
};

// SmtpConfig per smtpConfigToJson() ControlApi.cpp:2649 — security (НЕ tls_mode),
// нет поля configured.
const SMTP: SmtpConfig = {
  enabled: true, server: 'smtp.corp.local', port: 587, security: 'starttls',
  username: 'notify@corp.local', password_set: true,
  from_email: 'notify@corp.local', from_name: 'LiveQX',
  timeout_sec: 30,
};

// ─── Handlers ─────────────────────────────────────────────────────────────────

export const handlers = [

  // Auth — TokenPair: token_type/access_expires_at/refresh_expires_at.
  http.post('/api/auth/login', async ({ request }) => {
    await delay(D);
    const body = await request.json() as { username: string; password: string };
    const user = USERS.find(u => u.username === body.username && !u.disabled);
    if (!user) return HttpResponse.json({ error: 'invalid_credentials' }, { status: 401 });
    const now = Math.floor(Date.now()/1000);
    return HttpResponse.json({
      access_token: 'mock-access-token', refresh_token: 'mock-refresh-token',
      token_type: 'Bearer',
      access_expires_at:  now + 900,
      refresh_expires_at: now + 2592000,
      user: { id: user.id, username: user.username, role: user.role, must_change_password: user.must_change_password },
    });
  }),

  http.post('/api/auth/refresh', async () => {
    await delay(D);
    const now = Math.floor(Date.now()/1000);
    return HttpResponse.json({
      access_token: 'mock-access-token', refresh_token: 'mock-refresh-token',
      token_type: 'Bearer',
      access_expires_at:  now + 900,
      refresh_expires_at: now + 2592000,
    });
  }),

  http.post('/api/auth/logout', async () => {
    await delay(D);
    return HttpResponse.json({ status: 'logged_out' });
  }),

  http.post('/api/auth/me/password', async () => {
    await delay(D * 2);
    return HttpResponse.json({ status: 'password_changed' });
  }),

  // Users
  http.get('/api/auth/users', async () => {
    await delay(D);
    return HttpResponse.json({ users: USERS });
  }),

  http.get('/api/auth/users/:id', async ({ params }) => {
    await delay(D);
    const user = USERS.find(u => u.id === Number(params.id));
    if (!user) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    return HttpResponse.json({ ...user, channel_grants: [] });
  }),

  http.post('/api/auth/users', async ({ request }) => {
    await delay(D);
    const body = await request.json() as Partial<User>;
    const newUser: User = {
      id: Date.now(),
      username: body.username ?? '',
      email: body.email ?? '',
      role: body.role ?? 'viewer',
      source: body.source ?? 'local',
      disabled: false,
      must_change_password: true,
      failed_login_count: 0,
      created_at: Math.floor(Date.now()/1000),
    };
    USERS.push(newUser);
    return HttpResponse.json({ ...newUser, initial_password: 'Tmp#Pass123!' }, { status: 201 });
  }),

  http.put('/api/auth/users/:id', async ({ params, request }) => {
    await delay(D);
    const idx = USERS.findIndex(u => u.id === Number(params.id));
    if (idx < 0) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    const body = await request.json() as Partial<User>;
    USERS[idx] = { ...USERS[idx], ...body };
    return HttpResponse.json(USERS[idx]);
  }),

  http.post('/api/auth/users/:id/enable',  async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'enabled',   id: Number(params.id) }); }),
  http.delete('/api/auth/users/:id',       async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'disabled',  id: Number(params.id) }); }),
  http.post('/api/auth/users/:id/unlock',  async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'unlocked',  id: Number(params.id) }); }),
  http.post('/api/auth/users/:id/reset-password', async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'password_reset', id: Number(params.id), initial_password: 'New#Pass456!' }); }),

  http.get('/api/auth/users/:id/channels', async () => {
    await delay(D);
    return HttpResponse.json({ items: [] });
  }),

  // Audit
  http.get('/api/auth/audit', async () => {
    await delay(D);
    return HttpResponse.json({ events: AUDIT });
  }),

  http.post('/api/auth/audit/purge', async () => {
    await delay(D);
    return HttpResponse.json({ removed: 42, older_than_days: 90 });
  }),

  // LDAP
  http.get('/api/auth/ldap/config', async () => { await delay(D); return HttpResponse.json(LDAP); }),
  http.put('/api/auth/ldap/config', async ({ request }) => { await delay(D); const b = await request.json(); return HttpResponse.json({ ...LDAP, ...b as object }); }),
  http.post('/api/auth/ldap/test',  async () => { await delay(D * 4); return HttpResponse.json({ ping: { ok: true, latency_ms: 43 } }); }),

  // SMTP
  http.get('/api/auth/smtp/config', async () => { await delay(D); return HttpResponse.json(SMTP); }),
  http.put('/api/auth/smtp/config', async ({ request }) => { await delay(D); const b = await request.json(); return HttpResponse.json({ ...SMTP, ...b as object }); }),
  http.post('/api/auth/smtp/test',  async () => { await delay(D * 5); return HttpResponse.json({ ok: true }); }),

  // Channels
  http.get('/api/channels', async () => {
    await delay(D);
    channels = channels.map(ch => ({
      ...ch,
      fps_actual: ch.state === 'running'
        ? parseFloat(((ch.fps_actual ?? 25) + (Math.random() - 0.5) * 0.3).toFixed(2))
        : 0,
    }));
    return HttpResponse.json(channels);
  }),

  http.get('/api/channels/:id', async ({ params }) => {
    await delay(D);
    const ch = channels.find(c => c.id === Number(params.id));
    if (!ch) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    return HttpResponse.json(ch);
  }),

  http.post('/api/channels/:id/play', async ({ params }) => {
    await delay(D * 2);
    const idx = channels.findIndex(c => c.id === Number(params.id));
    if (idx >= 0) channels[idx] = { ...channels[idx], state: 'running', fps_actual: 25.0 };
    return HttpResponse.json(channels[idx]);
  }),

  http.post('/api/channels/:id/stop', async ({ params }) => {
    await delay(D * 2);
    const idx = channels.findIndex(c => c.id === Number(params.id));
    if (idx >= 0) channels[idx] = {
      ...channels[idx], state: 'stopped', fps_actual: 0,
      current_clip_index: -1, current_clip_remaining_sec: 0,
    };
    return HttpResponse.json(channels[idx]);
  }),

  http.post('/api/channels/:id/next', async () => { await delay(D); return HttpResponse.json({ ok: true }); }),

  http.put('/api/channels/:id/config', async ({ params, request }) => {
    await delay(D);
    const idx = channels.findIndex(c => c.id === Number(params.id));
    if (idx < 0) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    const body = await request.json() as Partial<ChannelStatus>;
    channels[idx] = { ...channels[idx], ...body };
    return HttpResponse.json(channels[idx]);
  }),

  // Playlist
  http.get('/api/channels/:id/playlist', async () => {
    await delay(D);
    return HttpResponse.json([
      { type: 'file', path: 'promo/intro.mp4',      transition_in: { type: 'crossfade', duration_sec: 1.0, easing: 'ease_in_out' } },
      { type: 'file', path: 'promo/main_loop.mp4',  transition_in: { type: 'crossfade', duration_sec: 0.5, easing: 'linear'       } },
      { type: 'live', id: 'rtmp-in-1', input: { type: 'rtmp', url: 'rtmp://0.0.0.0:1935/live/stream1' }, transition_in: { type: 'hardcut' } },
      { type: 'file', path: 'promo/outro.mp4',      transition_in: { type: 'crossfade', duration_sec: 1.5, easing: 'ease_out'      } },
    ]);
  }),

  http.post('/api/channels/:id/playlist',         async ({ request }) => { await delay(D); return HttpResponse.json(await request.json()); }),
  http.post('/api/channels/:id/playlist/append',  async ({ request }) => { await delay(D); const items = await request.json() as unknown[]; return HttpResponse.json({ first_idx: 4, playlist: items }); }),
  http.delete('/api/channels/:id/playlist',       async () => { await delay(D); return new HttpResponse(null, { status: 204 }); }),
  http.delete('/api/channels/:id/playlist/:idx',  async () => { await delay(D); return HttpResponse.json({ was_active: false, playlist: [] }); }),

  // Schedule — see Recurrence shape: kind=once|daily|weekly|monthly.
  http.get('/api/channels/:id/schedule', async () => {
    await delay(D);
    return HttpResponse.json([
      { id: 'morning',       playlist: 'morning_set', priority: 500, hard_switch: false, loop_mode: 'loop',
        transition: { type: 'crossfade', mode: 'live_mix', duration: 1.5, easing: 'ease_in_out' },
        recurrence: { kind: 'daily', start_time: '07:00', end_time: '11:00' } },
      { id: 'weekend-show',  playlist: 'weekend_show', priority: 600, hard_switch: true, loop_mode: 'loop',
        transition: { type: 'hardcut', mode: 'hard_cut', duration: 0, easing: 'linear' },
        recurrence: { kind: 'weekly', days_of_week: [6, 7], start_time: '18:00', end_time: '22:00' } },
      { id: 'special-event', playlist: 'special_event', priority: 900, hard_switch: false, loop_mode: 'loop',
        transition: { type: 'crossfade', mode: 'live_mix', duration: 2, easing: 'ease_in_out' },
        recurrence: { kind: 'once', start_at: new Date(Date.now() + 86400_000).toISOString(),
                                    end_at:   new Date(Date.now() + 90000_000).toISOString() } },
    ]);
  }),

  http.put('/api/channels/:id/schedule', async ({ request }) => { await delay(D); return HttpResponse.json(await request.json()); }),
  http.get('/api/channels/:id/schedule/active', async () => {
    await delay(D);
    return HttpResponse.json({
      mode: 'schedule',
      entry_id: 'morning',
      window_end_ns: BigInt(Date.now() + 1800_000) * 1_000_000n + '',
    });
  }),

  // Playback log
  http.get('/api/channels/:id/playback-log/status', async () => { await delay(D); return HttpResponse.json({ sink: 'file', count: 142, last_event_ts_ns: Date.now() * 1e6 }); }),
  http.get('/api/channels/:id/playback-log', async () => {
    await delay(D);
    return HttpResponse.json({ items: [
      { ts_ns: (Date.now() - 60000) * 1e6, event: 'clip_start', path: 'promo/intro.mp4', duration_sec: 30 },
      { ts_ns: (Date.now() - 30000) * 1e6, event: 'clip_end',   path: 'promo/intro.mp4', duration_sec: 30 },
      { ts_ns: (Date.now() -  1000) * 1e6, event: 'clip_start', path: 'promo/main_loop.mp4', duration_sec: 120 },
    ], truncated: false });
  }),

  http.get('/api/channels/:id/live-status', async ({ params }) => {
    await delay(D);
    const id = Number(params.id);
    const isLive = id === 4;
    return HttpResponse.json({
      channel_id: id,
      live_inputs: isLive ? [{
        id: 'rtmp-in-1',
        playlist_index: 2,
        input: { type: 'rtmp', url: 'rtmp://0.0.0.0:1935/live/stream1' },
        state: 'Live',
        last_packet_ts_ns: Date.now() * 1_000_000,
        first_packet_ts_ns: (Date.now() - 312_000) * 1_000_000,
        loss_seconds: 0,
        reconnect_count: 0,
      }] : [],
    });
  }),

  // Watcher
  http.get('/api/channels/:id/watcher/status', async ({ params }) => {
    await delay(D);
    return HttpResponse.json({ ...WATCHER, channel_id: Number(params.id) });
  }),
  http.post('/api/channels/:id/watcher/rescan', async () => { await delay(D * 5); return new HttpResponse(null, { status: 204 }); }),

  // Outputs
  http.get('/api/channels/:id/outputs', async ({ params }) => {
    await delay(D);
    const ch = channels.find(c => c.id === Number(params.id));
    return HttpResponse.json({ channel_id: Number(params.id), outputs: ch?.outputs ?? [] });
  }),
  http.post('/api/channels/:id/outputs',            async ({ request }) => { await delay(D); return HttpResponse.json(await request.json()); }),
  http.delete('/api/channels/:id/outputs/:oid',     async () => { await delay(D); return new HttpResponse(null, { status: 204 }); }),
  http.post('/api/channels/:id/outputs/:oid/restart', async () => { await delay(D); return new HttpResponse(null, { status: 204 }); }),

  // Permissions
  http.get('/api/channels/:id/permissions', async ({ params }) => {
    await delay(D);
    return HttpResponse.json({ channel_id: Number(params.id), items: [] });
  }),

  // Perf
  http.get('/api/channels/:id/perf',       async () => { await delay(D); return HttpResponse.json(PERF); }),
  http.post('/api/channels/:id/perf/start',async () => { await delay(D); return HttpResponse.json({ ...PERF, running: true }); }),
  http.post('/api/channels/:id/perf/stop', async () => { await delay(D); return HttpResponse.json({ ...PERF, mode: 'off', running: false }); }),

  // Preview
  http.post('/api/channels/:id/preview/offer', async () => {
    await delay(D * 2);
    return HttpResponse.json({ error: 'feature_disabled' }, { status: 503 });
  }),

  // Gateways
  http.get('/api/gateways',     async () => { await delay(D); return HttpResponse.json(GATEWAYS); }),
  http.get('/api/gateways/:id', async ({ params }) => {
    await delay(D);
    const gw = GATEWAYS.find(g => g.id === Number(params.id));
    if (!gw) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    return HttpResponse.json(gw);
  }),
  http.post('/api/gateways',         async () => { await delay(D); return HttpResponse.json({ id: 3 }, { status: 201 }); }),
  http.delete('/api/gateways/:id',   async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'deleted', id: Number(params.id) }); }),
  http.post('/api/gateways/:id/play',async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'playing', id: Number(params.id) }); }),
  http.post('/api/gateways/:id/stop',async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'stopped', id: Number(params.id) }); }),

  // Plugins
  http.get('/api/plugins',         async () => { await delay(D); return HttpResponse.json({ items: PLUGINS }); }),
  http.get('/api/plugins/:name',   async ({ params }) => {
    await delay(D);
    const p = PLUGINS.find(x => x.name === params.name);
    if (!p) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    return HttpResponse.json(p);
  }),
  http.post('/api/plugins/:name/install',     async () => { await delay(D * 8); return HttpResponse.json({ status: 'installed', name: 'ndi', sha256: '0a1b2c...' }, { status: 201 }); }),
  http.delete('/api/plugins/:name',           async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'pending_unload', name: params.name }); }),
  http.post('/api/plugins/:name/eula/accept', async ({ params }) => { await delay(D); return HttpResponse.json({ status: 'ok', name: params.name }); }),

  // Stress
  http.get('/api/stress/status',  async () => { await delay(D); return HttpResponse.json(STRESS_STATUS); }),
  http.get('/api/stress/config',  async () => { await delay(D); return HttpResponse.json({ scenario: 'smoke', channels: 50, duration_sec: 600, cron: '0 2 * * *' }); }),
  http.put('/api/stress/config',  async ({ request }) => { await delay(D); return HttpResponse.json(await request.json()); }),
  http.post('/api/stress/start',  async () => {
    await delay(D);
    return HttpResponse.json({
      ...STRESS_STATUS,
      runner: { state: 'running', started_at_ms: Date.now() },
    });
  }),
  http.post('/api/stress/stop',   async () => { await delay(D); return HttpResponse.json(STRESS_STATUS); }),
  http.get('/api/stress/reports', async () => { await delay(D); return HttpResponse.json(STRESS_REPORTS); }),
  http.get('/api/stress/reports/:id', async ({ params }) => {
    await delay(D);
    const s = STRESS_REPORTS.find(r => r.id === params.id);
    if (!s) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    return HttpResponse.json({ summary: s, config: { scenario: 'smoke', channels: 50, duration_sec: 600 }, per_channel: [] });
  }),

  // System — SystemStatus per renderStatus() MetricsCollector.cpp:523.
  http.get('/api/system/interfaces', async () => { await delay(D); return HttpResponse.json(INTERFACES); }),
  http.get('/api/system/gpu',        async () => { await delay(D); return HttpResponse.json(GPU); }),
  http.get('/api/version',           async () => { return HttpResponse.json(VERSION); }),
  http.get('/api/status', async () => {
    await delay(D);
    let outputs_total = 0, outputs_failed = 0, channels_running = 0, channels_failed = 0;
    for (const ch of channels) {
      if (ch.state === 'running')  channels_running++;
      if (ch.state === 'failed')   channels_failed++;
      for (const o of ch.outputs ?? []) {
        outputs_total++;
        if (!o.healthy) outputs_failed++;
      }
    }
    const status: SystemStatus = {
      version: VERSION.version,
      build_commit: VERSION.build_commit,
      build_time:   VERSION.build_time,
      uptime_seconds: 187432,
      process: { rss_bytes: 512*1024*1024, vsize_bytes: 1.2*1024*1024*1024, open_fds: 84, threads: 42, cpu_seconds_total: 3421 },
      channels: channels.map(ch => ({
        id: ch.id, name: ch.name,
        state: ch.state === 'running' ? 'running' : 'stopped',
        outputs: (ch.outputs ?? []).map(o => ({
          id: o.id,
          type: o.transport ?? (o.mode === 'ndi' ? 'ndi' : 'srt'),
          healthy: o.healthy,
          bytes_total: o.bytes_sent ?? 0,
          drops_total: o.queue_drops ?? 0,
          queue_bytes_used: o.queue_bytes_used ?? 0,
          queue_bytes_limit: o.queue_bytes_limit ?? 0,
        })),
      })),
      gateways: [],
      summary: {
        channels_total: channels.length,
        channels_running, channels_failed,
        outputs_total,    outputs_failed,
        gateways_total: 0, gateways_running: 0,
      },
    };
    return HttpResponse.json(status);
  }),

  // Health probes (public)
  http.get('/healthz', async () => HttpResponse.json({ status: 'ok' })),
  http.get('/readyz',  async () => HttpResponse.json({ status: 'ready' })),
  http.get('/livez',   async () => HttpResponse.json({ status: 'alive' })),
  http.get('/api/health', async () => HttpResponse.json({ overall: 'ok' })),

  // fix25 round 2 batch (Q1 sessions, Q5 live-status, Q8 preview, perf, system, stress, plugin detail, healthz)
  ...fix25Handlers,
];
