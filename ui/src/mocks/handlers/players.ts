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

export const playersHandlers = [
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
