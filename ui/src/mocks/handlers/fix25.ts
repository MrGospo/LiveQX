/**
 * MSW handlers — extension batch for fix25 round 2 endpoints.
 *
 * Imported by mocks/handlers/index.ts. Provides mock responses for:
 *   - /api/auth/me/sessions          (Q1)
 *   - /api/channels/:id/live-status  (Q5)
 *   - /api/channels/:id/preview/*    (Q8)
 *   - /api/channels/:id/perf/*       (PerfPage)
 *   - /api/system/interfaces, /gpu   (SystemPage)
 *   - /api/stress/reports/:id        (StressReportPage)
 *   - /api/plugins/:name (full)      (PluginDetailPage)
 *   - /healthz, /readyz, /livez      (HealthPage)
 */
import { http, HttpResponse } from 'msw';

export const fix25Handlers = [
  // fix32 B2: own sessions. Endpoint live; mock возвращает 2 сессии,
  // одна current (этот браузер), вторая — другое устройство.
  http.get('/api/auth/me/sessions', () => {
    const now = Math.floor(Date.now() / 1000);
    return HttpResponse.json({
      items: [
        {
          jwt_id: 'mock-current-session-jwt-id',
          created_at: now - 600,
          expires_at: now + 86_400 * 30,
          last_seen_at: now - 5,
          ip: '127.0.0.1',
          user_agent: 'Mozilla/5.0 (Mock)',
          current: true,
        },
        {
          jwt_id: 'mock-other-session-jwt-id',
          created_at: now - 86_400,
          expires_at: now + 86_400 * 29,
          last_seen_at: now - 3_600,
          ip: '10.0.1.42',
          user_agent: 'curl/8.0.0',
          current: false,
        },
      ],
    });
  }),

  // DELETE — отвечает revoked.
  http.delete('/api/auth/me/sessions/:jwtId', ({ params }) =>
    HttpResponse.json({
      status: 'revoked',
      jwt_id: String(params.jwtId),
      current: String(params.jwtId) === 'mock-current-session-jwt-id',
    }),
  ),

  // fix32 B3 — master-key info. Stable mock fingerprint; created_at в районе
  // прошлой недели; last_rotated_at null (никаких .bak ещё нет).
  http.get('/api/auth/master-key/info', () => {
    const now = Math.floor(Date.now() / 1000);
    return HttpResponse.json({
      fingerprint: 'a1b2c3d4e5f60718',
      algorithm: 'xsalsa20poly1305',
      source: 'file',
      created_at: now - 86_400 * 7,
      last_rotated_at: null,
    });
  }),

  // Q5: per-channel live-status
  http.get('/api/channels/:id/live-status', ({ params }) => {
    const id = Number(params.id);
    return HttpResponse.json({
      channel_id: id,
      state: id % 2 === 0 ? 'live' : 'idle',
      input: id % 2 === 0
        ? { type: 'rtmp', url: 'rtmp://ingest.local/live/cam-' + id }
        : null,
      since: id % 2 === 0 ? Math.floor(Date.now() / 1000) - 1234 : null,
      last_loss_sec: id % 2 === 0 ? 0 : null,
    });
  }),

  // Q8: WebRTC preview offer — happy path
  http.post('/api/channels/:id/preview/offer', () =>
    HttpResponse.json({
      session_id: `sess-${Math.random().toString(36).slice(2, 9)}`,
      sdp: 'v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n',
      type: 'answer',
    }),
  ),
  http.delete('/api/channels/:id/preview/:sid', () =>
    new HttpResponse(null, { status: 204 }),
  ),
  http.get('/api/channels/:id/preview/stats', () =>
    HttpResponse.json({
      sessions: [{ session_id: 'sess-mock', ice_state: 'connected', bytes_sent: 12345678 }],
      fps: 30, kbps: 4200,
    }),
  ),

  // Perf
  http.get('/api/channels/:id/perf', ({ params }) =>
    HttpResponse.json({
      mode: 'sampling',
      running: true,
      channel_id: Number(params.id),
      active_ms: 12_345,
      sampled_hits: { decode: 102, compose: 87, encode: 211, output: 30, total: 430, none: 12 },
      stage_us:     { decode: 4123, compose: 1822, encode: 9871, output: 612, total: 16428, none: 0 },
      stage_count:  { decode: 102, compose: 102, encode: 102, output: 102, total: 102, none: 0 },
    }),
  ),
  http.post('/api/channels/:id/perf/start', async ({ request }) => {
    const body = await request.json() as { mode?: string };
    return HttpResponse.json({ mode: body.mode ?? 'sampling', running: true });
  }),
  http.post('/api/channels/:id/perf/stop', () =>
    HttpResponse.json({ mode: 'off', running: false }),
  ),

  // System
  http.get('/api/system/interfaces', () =>
    HttpResponse.json([
      { name: 'lo',   up: true, loopback: true, addresses: ['127.0.0.1', '::1'] },
      { name: 'eth0', up: true, addresses: ['10.0.1.42', 'fe80::1234'] },
      { name: 'eth1', up: false, addresses: [] },
    ]),
  ),
  http.get('/api/system/gpu', () =>
    HttpResponse.json({
      nvenc:  { built_in: true,  codec_registered: true  },
      qsv:    { built_in: true,  codec_registered: false },
      vaapi:  { built_in: false, codec_registered: false },
    }),
  ),

  // Stress reports detail
  http.get('/api/stress/reports/:id', ({ params }) =>
    HttpResponse.json({
      summary: {
        id: String(params.id),
        started_at: Math.floor(Date.now() / 1000) - 3600,
        duration_sec: 600,
        scenario: 'sustained',
        channel_count: 8,
        status: 'completed',
        drops_total: 12,
        underruns_total: 3,
      },
      config: { scenario: 'sustained', channels: 8, duration_sec: 600 },
      per_channel: Array.from({ length: 8 }, (_, i) => ({
        channel_id: i + 1, drops: i, underruns: i % 2, avg_fps: 29.97,
      })),
    }),
  ),

  // Plugin detail
  http.get('/api/plugins/:name', ({ params }) =>
    HttpResponse.json({
      name: String(params.name),
      version: '1.2.3',
      sha256: 'a'.repeat(64),
      installed_at: Math.floor(Date.now() / 1000) - 86_400,
      forced: false,
      eula_accepted: true,
      output_drivers: ['srt-pro'],
      input_drivers: [],
    }),
  ),

  // Health probes
  http.get('/healthz', () => HttpResponse.json({ status: 'ok' })),
  http.get('/readyz',  () => HttpResponse.json({ status: 'ok', checks: { db: 'ok', encoder: 'ok' } })),
  http.get('/livez',   () => HttpResponse.json({ status: 'ok' })),

  // ── Round 3 — CRUD endpoints ──────────────────────────────────────────────
  // Echo-back semantics: POST returns { id, ...input } so the redirect-to-
  // detail page immediately shows what the user just typed.

  // Create channel
  http.post('/api/channels', async ({ request }) => {
    const body = await request.json() as Record<string, unknown>;
    return HttpResponse.json(
      { id: crypto.randomUUID(), ...body, state: 'stopped' },
      { status: 201 },
    );
  }),

  // Create gateway
  http.post('/api/gateways', async ({ request }) => {
    const body = await request.json() as Record<string, unknown>;
    return HttpResponse.json(
      { id: crypto.randomUUID(), ...body, state: 'stopped' },
      { status: 201 },
    );
  }),

  // Plugin install (wizard step 5)
  http.post('/api/plugins', async ({ request }) => {
    const body = await request.json().catch(() => ({})) as Record<string, unknown>;
    const name = (body.name as string) ?? 'unnamed-plugin';
    return HttpResponse.json(
      {
        name,
        version: (body.version as string) ?? '1.0.0',
        sha256: (body.sha256 as string) ?? 'a'.repeat(64),
        sig_status: 'ok',
        state: 'loaded',
      },
      { status: 201 },
    );
  }),

  // Plugin uninstall
  http.delete('/api/plugins/:name', () =>
    new HttpResponse(null, { status: 204 }),
  ),

  // Playlist re-scan (DeletedFilesAlert)
  http.post('/api/channels/:id/playlist/notify-deleted', () =>
    HttpResponse.json({ removed_count: 3 }),
  ),

  // Schedule upcoming — covers all 3 action discriminants
  http.get('/api/channels/:id/schedule/upcoming', () => {
    const now = Math.floor(Date.now() / 1000);
    return HttpResponse.json([
      {
        entry_id: 'sch-1',
        fire_at: now + 5 * 60,
        action: { type: 'playlist_override', playlist: 'evening-block.m3u' },
      },
      {
        entry_id: 'sch-2',
        fire_at: now + 30 * 60,
        action: { type: 'live_switch', input: { type: 'rtmp', url: 'rtmp://ingest.local/live/cam-1' } },
      },
      {
        entry_id: 'sch-3',
        fire_at: now + 2 * 60 * 60,
        action: { type: 'pause' },
      },
    ]);
  }),
];
