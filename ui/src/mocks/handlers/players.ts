/**
 * MSW handlers — screens (players): module settings and the broker link.
 *
 * Dev-only stand-ins for GET/PUT /api/players/settings and the /api/players/broker
 * family, shaped like the answers in openapi.yaml. The state is kept in memory so a
 * save is seen by the next read, and the broker answers like the real one does:
 * a new address is tried first, and an address containing "dead" does not answer.
 */
import { http, HttpResponse, delay } from 'msw';

let settings = {
  server_url: 'https://liveqx.example.lan',
  device_api_url: 'https://liveqx.example.lan:8444',
  mqtt_broker_url: '',
  ca_cert_pem: '',
  token_ttl_days: 7,
  clock_policy: 'strict',
  command_ttl_s: 300,
  confirm_timeout_restart_s: 300,
  confirm_timeout_reboot_s: 1200,
  confirm_timeout_reload_s: 120,
  command_batch_size: 10,
  command_batch_stop_percent: 30,
  events_retention_days: 90,
  commands_retention_days: 365,
};

let connection = { host: '127.0.0.1', port: 8883, verify_hostname: true, reconcile_interval_s: 300 };
const nowSec = () => Math.floor(Date.now() / 1000);

const brokerStatus = () => ({
  enabled: true,
  state: settings.mqtt_broker_url ? 'connected' : 'waiting',
  detail: settings.mqtt_broker_url ? '' : 'the device CA is not set up yet',
  ...connection,
  last_reconcile_at: nowSec() - 95,
  last_reconcile_ok: true,
  last_report: { issued: 2, removed: 0, failed: 0, error: '' },
  crl: {
    mode: 'local', last_publish_at: nowSec() - 3600, last_ok: true, error: '', reload_error: '',
    revoked: 1, generations: 1, valid_until: nowSec() + 29 * 86400,
  },
  readiness: {
    mosquitto_in_build: true,
    device_ca: !!settings.mqtt_broker_url,
    trust_root: true,
    broker_url_configured: !!settings.mqtt_broker_url,
  },
});

// The tree of objects and groups, kept in memory like the rest. Deleting a group
// with screens or an object with groups is refused the way the real server does.
type MockGroup = { id: number; object_id: number; name: string; created_at: number; players: number };
type MockObject = { id: number; name: string; created_at: number };
let nextId = 10;
let objects: MockObject[] = [
  { id: 1, name: 'Торговый зал', created_at: nowSec() - 86400 },
  { id: 2, name: 'Склад', created_at: nowSec() - 3600 },
];
let groups: MockGroup[] = [
  { id: 1, object_id: 1, name: 'Касса', created_at: nowSec() - 86000, players: 3 },
  { id: 2, object_id: 1, name: 'Витрина', created_at: nowSec() - 80000, players: 2 },
];
const nameOf = async (request: Request) => String(((await request.json()) as { name?: unknown }).name ?? '').trim();
const bad = (code: string, status: number) => HttpResponse.json({ error: code }, { status });

export const playersHandlers = [
  http.get('/api/player-objects', async () => {
    await delay(120);
    return HttpResponse.json({
      objects: objects.map(o => ({
        ...o,
        can: { command_object: true },
        groups: groups.filter(g => g.object_id === o.id).map(g => ({ ...g, can: { move: true, command_group: true } })),
      })),
    });
  }),
  http.post('/api/player-objects', async ({ request }) => {
    const name = await nameOf(request);
    if (!name) return bad('invalid_name', 400);
    if (objects.some(o => o.name === name)) return bad('name_taken', 409);
    const o = { id: nextId++, name, created_at: nowSec() };
    objects = [...objects, o];
    return HttpResponse.json(o, { status: 201 });
  }),
  http.put('/api/player-objects/:id', async ({ request, params }) => {
    const id = Number(params.id); const name = await nameOf(request);
    if (!name) return bad('invalid_name', 400);
    if (!objects.some(o => o.id === id)) return bad('not_found', 404);
    if (objects.some(o => o.name === name && o.id !== id)) return bad('name_taken', 409);
    objects = objects.map(o => (o.id === id ? { ...o, name } : o));
    return HttpResponse.json({ id, name });
  }),
  http.delete('/api/player-objects/:id', async ({ params }) => {
    const id = Number(params.id);
    if (!objects.some(o => o.id === id)) return bad('not_found', 404);
    if (groups.some(g => g.object_id === id)) return bad('not_empty', 409);
    objects = objects.filter(o => o.id !== id);
    return HttpResponse.json({ id });
  }),
  http.post('/api/player-objects/:id/groups', async ({ request, params }) => {
    const objectId = Number(params.id); const name = await nameOf(request);
    if (!name) return bad('invalid_name', 400);
    if (!objects.some(o => o.id === objectId)) return bad('not_found', 404);
    if (groups.some(g => g.object_id === objectId && g.name === name)) return bad('name_taken', 409);
    const g = { id: nextId++, object_id: objectId, name, created_at: nowSec(), players: 0 };
    groups = [...groups, g];
    return HttpResponse.json(g, { status: 201 });
  }),
  http.put('/api/player-groups/:id', async ({ request, params }) => {
    const id = Number(params.id); const name = await nameOf(request);
    const cur = groups.find(g => g.id === id);
    if (!name) return bad('invalid_name', 400);
    if (!cur) return bad('not_found', 404);
    if (groups.some(g => g.object_id === cur.object_id && g.name === name && g.id !== id)) return bad('name_taken', 409);
    groups = groups.map(g => (g.id === id ? { ...g, name } : g));
    return HttpResponse.json({ id, name });
  }),
  http.delete('/api/player-groups/:id', async ({ params }) => {
    const id = Number(params.id); const cur = groups.find(g => g.id === id);
    if (!cur) return bad('not_found', 404);
    if (cur.players > 0) return bad('not_empty', 409);
    groups = groups.filter(g => g.id !== id);
    return HttpResponse.json({ id });
  }),

  http.get('/api/players/settings', async () => { await delay(120); return HttpResponse.json(settings); }),

  http.put('/api/players/settings', async ({ request }) => {
    await delay(150);
    const patch = await request.json() as Record<string, unknown>;
    if (typeof patch.server_url === 'string' && patch.server_url && !patch.server_url.startsWith('https://'))
      return HttpResponse.json({ error: 'invalid_server_url' }, { status: 400 });
    settings = { ...settings, ...patch } as typeof settings;
    return HttpResponse.json(settings);
  }),

  http.get('/api/players/broker', async () => { await delay(120); return HttpResponse.json(brokerStatus()); }),

  http.post('/api/players/broker/reconcile', async () => { await delay(100); return HttpResponse.json({ requested: true }, { status: 202 }); }),

  http.put('/api/players/broker', async ({ request }) => {
    const body = await request.json() as Partial<typeof connection> & { check_only?: boolean; skip_check?: boolean };
    const next = { ...connection, ...body };
    const reachable = !String(next.host).includes('dead');
    if (body.check_only) {
      await delay(900);
      return HttpResponse.json({ checked: true, ok: reachable, detail: reachable ? '' : 'no answer from the broker in time', connection: next });
    }
    const moved = next.host !== connection.host || next.port !== connection.port || next.verify_hostname !== connection.verify_hostname;
    if (moved && !body.skip_check) {
      await delay(900);
      if (!reachable)
        return HttpResponse.json({ error: 'probe_failed', detail: 'no answer from the broker in time', connection }, { status: 502 });
    }
    await delay(150);
    connection = { host: next.host, port: next.port, verify_hostname: next.verify_hostname, reconcile_interval_s: next.reconcile_interval_s };
    return HttpResponse.json({
      applied: true, checked: moved && !body.skip_check, connection,
      ...(connection.verify_hostname ? {} : { warning: 'verify_hostname_disabled' }),
    });
  }),
];

// ─── Screens: list, one screen, its commands ───────────────────────────────────
const nowS = () => Math.floor(Date.now() / 1000);
const screen = (id: string, name: string, groupId: number | null, state: 'pending' | 'active' | 'revoked',
                online: boolean, ago: number, version = '1.4.2', report: Record<string, unknown> = {}) => {
  const g = groups.find(x => x.id === groupId);
  const o = g && objects.find(x => x.id === g.object_id);
  return {
    id, name, state, clock_policy: 'strict', created_at: nowS() - 30 * 86400,
    ...(g ? { group_id: g.id, group_name: g.name, object_id: g.object_id, object_name: o?.name } : {}),
    ...(state !== 'pending' ? { activated_at: nowS() - 29 * 86400 } : {}),
    ...(state === 'revoked' ? { revoked_at: nowS() - 86400, revoke_reason: 'revoked_by_admin' } : {}),
    ...(state === 'active' ? { status: { online, version, updated_at: nowS() - ago, state: { health: 'ok', watchdog_state: 'ok', uptime_seconds: 93000, os_uptime_seconds: 400000, current_source: 'https://liveqx.example.lan/hls/main.m3u8', buffer_progress: 0.92, ...report } } } : {}),
  };
};
const screens = () => [
  screen('3b1f0c9e-0000-4000-8000-000000000001', 'Касса 1', 1, 'active', true, 8),
  screen('3b1f0c9e-0000-4000-8000-000000000002', 'Касса 2', 1, 'active', true, 12),
  screen('3b1f0c9e-0000-4000-8000-000000000003', 'Касса 3', 1, 'active', false, 7200, '1.4.1', { health: 'critical', last_error: 'no signal from the stream' }),
  screen('3b1f0c9e-0000-4000-8000-000000000004', 'Витрина слева', 2, 'pending', false, 0),
  screen('3b1f0c9e-0000-4000-8000-000000000005', 'Старый экран', 2, 'revoked', false, 0),
];

// The screens live in memory so that adding, moving and revoking show in the next read.
let screenDb = screens();
const pairing = (id: string, name: string, clockPolicy = 'strict') => ({
  schema: 1, player_id: id, server_url: settings.server_url || 'https://liveqx.example.lan',
  ca_cert_pem: '-----BEGIN CERTIFICATE-----\nMIIB…(sample)…\n-----END CERTIFICATE-----\n',
  device_api_url: settings.device_api_url || 'https://liveqx.example.lan:8444',
  mqtt_broker_url: settings.mqtt_broker_url || 'mqtts://liveqx.example.lan:8883',
  bootstrap_token: `sample-token-for-${name}`,
  token_expires_at: new Date(Date.now() + 7 * 86400_000).toISOString(),
  clock_policy: clockPolicy,
});

export const playersListHandlers = [
  http.post('/api/players', async ({ request }) => {
    await delay(300);
    const b = await request.json() as { name?: string; group_id?: number; clock_policy?: string };
    if (!b.name || !b.name.trim()) return bad('invalid_name', 400);
    if (!groups.some(g => g.id === b.group_id)) return bad('group_not_found', 404);
    const id = crypto.randomUUID();
    const row = screen(id, b.name.trim(), b.group_id ?? null, 'pending', false, 0);
    screenDb = [...screenDb, { ...row, clock_policy: b.clock_policy ?? 'strict' }];
    return HttpResponse.json(pairing(id, row.name, b.clock_policy), { status: 201 });
  }),
  http.post('/api/players/:id/reissue-token', async ({ params }) => {
    await delay(200);
    const s = screenDb.find(x => x.id === params.id);
    if (!s) return bad('not_found', 404);
    if (s.state !== 'pending') return bad('not_pending', 409);
    return HttpResponse.json(pairing(s.id, s.name, s.clock_policy));
  }),
  http.post('/api/players/:id/re-enroll', async ({ params }) => {
    await delay(250);
    const s = screenDb.find(x => x.id === params.id);
    if (!s) return bad('not_found', 404);
    if (s.state === 'pending') return bad('already_pending', 409);
    screenDb = screenDb.map(x => (x.id === s.id
      ? { ...x, state: 'pending' as const, revoked_at: undefined, revoke_reason: undefined, activated_at: undefined, status: undefined }
      : x));
    return HttpResponse.json(pairing(s.id, s.name, s.clock_policy));
  }),
  http.post('/api/players/:id/revoke-cert', async ({ params, request }) => {
    await delay(200);
    const b = await request.json().catch(() => ({})) as { reason?: string };
    const s = screenDb.find(x => x.id === params.id);
    if (!s) return bad('not_found', 404);
    screenDb = screenDb.map(x => (x.id === s.id
      ? { ...x, state: 'revoked' as const, revoked_at: nowS(), revoke_reason: b.reason ?? 'revoked_by_admin', status: undefined }
      : x));
    return HttpResponse.json({ revoked_certs: 1 });
  }),
  http.delete('/api/players/:id', async ({ params }) => {
    await delay(200);
    if (!screenDb.some(x => x.id === params.id)) return bad('not_found', 404);
    screenDb = screenDb.filter(x => x.id !== params.id);
    return HttpResponse.json({ revoked_certs: 1 });
  }),
  http.put('/api/players/:id/group', async ({ params, request }) => {
    await delay(200);
    const b = await request.json() as { group_id?: number };
    const g = groups.find(x => x.id === b.group_id);
    if (!g) return bad('group_not_found', 404);
    const s = screenDb.find(x => x.id === params.id);
    if (!s) return bad('not_found', 404);
    const o = objects.find(x => x.id === g.object_id);
    const moved = { ...s, group_id: g.id, group_name: g.name, object_id: g.object_id, object_name: o?.name };
    screenDb = screenDb.map(x => (x.id === s.id ? moved : x));
    return HttpResponse.json(moved);
  }),
  http.get('/api/players', async ({ request }) => {
    await delay(150);
    const u = new URL(request.url);
    const g = u.searchParams.get('group_id'); const o = u.searchParams.get('object_id');
    const status = u.searchParams.get('status'); const q = (u.searchParams.get('q') ?? '').toLowerCase();
    const limit = Number(u.searchParams.get('limit') ?? 200); const offset = Number(u.searchParams.get('offset') ?? 0);
    const display = (s: ReturnType<typeof screens>[number]) =>
      s.state !== 'active' ? s.state : (s.status?.online ? 'online' : 'offline');
    const inPlace = screenDb
      .filter(s => (!g || String(s.group_id) === g) && (!o || String(s.object_id) === o))
      .filter(s => !q || `${s.name} ${s.group_name ?? ''} ${s.object_name ?? ''}`.toLowerCase().includes(q));
    const counts = { online: 0, offline: 0, pending: 0, revoked: 0 };
    for (const s of inPlace) counts[display(s) as keyof typeof counts]++;
    const matching = inPlace.filter(s => !status || display(s) === status)
      .sort((a, b) => a.name.localeCompare(b.name, 'ru'));
    return HttpResponse.json({ players: matching.slice(offset, offset + limit), total: matching.length, counts });
  }),
  http.get('/api/players/:id/commands', async ({ params }) => {
    await delay(100);
    const now = nowS();
    return HttpResponse.json({ commands: [
      { id: 'c1', player_id: params.id, type: 'restart_player', state: 'executed', result: 'executed', issued_at: now - 7200, ttl_seconds: 300, deadline_at: now - 6900, finished_at: now - 7190 },
      { id: 'c2', player_id: params.id, type: 'reboot_screen', state: 'timeout', result: 'no_confirmation', issued_at: now - 90000, ttl_seconds: 300, deadline_at: now - 89700, finished_at: now - 88000 },
    ] });
  }),
  http.get('/api/players/:id', async ({ params }) => {
    await delay(150);
    const s = screenDb.find(x => x.id === params.id);
    if (!s) return HttpResponse.json({ error: 'not_found' }, { status: 404 });
    const now = nowS();
    return HttpResponse.json({
      ...s,
      can: { move: true, command: true },
      certificates: s.state === 'pending' ? [] : [
        { serial: '5f3a9c01b7', generation: 1, issued_at: now - 29 * 86400, not_before: now - 29 * 86400, not_after: now + 336 * 86400 },
        ...(s.state === 'revoked' ? [{ serial: '1a2b3c4d5e', generation: 1, issued_at: now - 40 * 86400, not_before: now - 40 * 86400, not_after: now + 20 * 86400, revoked_at: now - 86400, revoke_reason: 'revoked_by_admin' }] : []),
      ],
      events: [
        { id: 3, ts: now - 7200, kind: 'offline', details: JSON.stringify({ reason: 'heartbeat_timeout', last_seen: now - 7260 }) },
        { id: 2, ts: now - 90000, kind: 'version_changed', details: JSON.stringify({ from: '1.4.0', to: '1.4.1' }) },
        { id: 1, ts: now - 29 * 86400, kind: 'online', details: '' },
      ],
    });
  }),
];
