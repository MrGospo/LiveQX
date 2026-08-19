/**
 * Hand-curated barrel over the auto-generated openapi.ts.
 *
 * Why a barrel rather than the raw generated file:
 *   - openapi-typescript v7 emits types only as `components["schemas"]["Foo"]`,
 *     which is verbose at every call site.
 *   - SseEvent and ApiError are frontend-only shapes — they have no openapi
 *     counterpart yet (SSE is a wire protocol, ApiError is the normalised
 *     error envelope returned by api/client.ts).
 *
 * Regen flow:
 *   `npm run gen:api-types` overwrites only `openapi.ts`. This file is safe
 *   to edit by hand. After a regen, add new aliases here as needed.
 */
import type { components } from './openapi';

export type { components, paths } from './openapi';

// ─── Auth / users ──────────────────────────────────────────────────────────────
export type Role           = components['schemas']['Role'];
export type User           = components['schemas']['User'];
export type UserDetail     = components['schemas']['UserDetail'];
export type TokenPair      = components['schemas']['TokenPair'];
export type LoginResponse  = components['schemas']['LoginResponse'];
export type AuditEvent     = components['schemas']['AuditEvent'];
export type ChannelPermissionRow = components['schemas']['ChannelPermissionRow'];
export type OwnSession      = components['schemas']['OwnSession'];
export type MasterKeyInfo   = components['schemas']['MasterKeyInfo'];

// ─── Channel runtime ──────────────────────────────────────────────────────────
export type ChannelState   = components['schemas']['ChannelState'];
export type OutputState    = components['schemas']['OutputState'];
export type LiveInputState = components['schemas']['LiveInputState'];
export type HealthState    = components['schemas']['HealthState'];

// MPEG-TS / IPTV broadcast knobs surfaced by ChannelInstance::status() under
// the `mpegts` key. Held here (rather than in openapi.ts) until the next
// gen:api-types run, mirroring the pattern used for MountSpec below.
export interface MpegtsConfig {
  service_name?: string;
  service_provider?: string;
  service_id?: number;
  transport_stream_id?: number;
  original_network_id?: number;
  mux_rate?: number;
  sdt_period_ms?: number;
  pat_period_ms?: number;
}

export type ChannelStatus  = components['schemas']['ChannelStatus'] & {
  mpegts?: MpegtsConfig;
  // "h264" (default) or "mpeg2video". Held here (rather than openapi.ts)
  // until the next gen:api-types run — mirrors the pattern used for
  // mpegts / MountSpec above.
  video_codec?: 'h264' | 'mpeg2video';
  audio_codec?: 'aac' | 'mp2';
};
export type LiveInputStatus = components['schemas']['LiveInputStatus'];
export type OutputStatus   = components['schemas']['OutputStatus'];

// ─── Playlist / schedule / watcher / playback log ─────────────────────────────
export type Playlist          = components['schemas']['Playlist'];
export type PlaylistEntry     = components['schemas']['PlaylistEntry'];
export type Schedule          = components['schemas']['Schedule'];
export type ScheduleEntry     = components['schemas']['ScheduleEntry'];
export type ScheduleTransition = components['schemas']['ScheduleTransition'];
export type Recurrence        = components['schemas']['Recurrence'];
export type ScheduleActive    = components['schemas']['ScheduleActive'];
export type WatcherStatus     = components['schemas']['WatcherStatus'];
export type PlaybackLogStatus = components['schemas']['PlaybackLogStatus'];
export type PlaybackLogEntry  = components['schemas']['PlaybackLogEntry'];

// ─── Gateways / system ────────────────────────────────────────────────────────
export type GatewayConfig    = components['schemas']['GatewayConfig'];
export type GatewayStatus    = components['schemas']['GatewayStatus'];
export type GatewayMode      = components['schemas']['GatewayMode'];
export type DemuxCfg         = components['schemas']['DemuxCfg'];
export type DemuxRule        = components['schemas']['DemuxRule'];
export type RemuxCfg         = components['schemas']['RemuxCfg'];
export type PidRemapEntry    = components['schemas']['PidRemapEntry'];
export type TranscodeCfg     = components['schemas']['TranscodeCfg'];
export type FecCfg           = components['schemas']['FecCfg'];
export type NetworkInterface = components['schemas']['NetworkInterface'];
export type GpuInfo          = components['schemas']['GpuInfo'];
export type VersionInfo      = components['schemas']['VersionInfo'];
export type SystemStatus     = components['schemas']['SystemStatus'];

// ─── Settings: TLS ────────────────────────────────────────────────────────────
export type TlsCertInfo            = components['schemas']['TlsCertInfo'];
export type TlsInfo                = components['schemas']['TlsInfo'];
export type TlsRegenerateResponse  = components['schemas']['TlsRegenerateResponse'];
export type TlsImportResponse      = components['schemas']['TlsImportResponse'];

// ─── Settings: LDAP / SMTP ────────────────────────────────────────────────────
export type LdapConfig        = components['schemas']['LdapConfig'];
export type LdapConfigRequest = components['schemas']['LdapConfigRequest'];
export type LdapTestResponse  = components['schemas']['LdapTestResponse'];
export type SmtpConfig        = components['schemas']['SmtpConfig'];
export type SmtpConfigRequest = components['schemas']['SmtpConfigRequest'];
export type SmtpTestResponse  = components['schemas']['SmtpTestResponse'];

// ─── Plugins ──────────────────────────────────────────────────────────────────
export type PluginListing = components['schemas']['PluginListing'];

// ─── Settings: Mounts (fix41) ────────────────────────────────────────────────
// fix41 — mount-каталог появился позже последнего gen:api-types; держим
// типы вручную тут до следующего регена, чтобы не блокировать UI на
// openapi-update.
export interface MountCifsCreds {
  username: string;
  password?: string;     // только при write; backend никогда не отдаёт
  domain?: string;
}

export interface MountSpec {
  id?: number;           // 0 / undefined для create
  fs_type: 'cifs' | 'nfs';
  source: string;        // "//host/share" (CIFS) | "host:/path" (NFS)
  target: string;        // абсолютный путь под /mnt/liveqx/
  options: string;       // comma-separated mount opts
  ro: boolean;
  enabled?: boolean;
  cifs?: MountCifsCreds;
}

export interface MountPublic {
  id: number;
  fs_type: 'cifs' | 'nfs';
  source: string;
  target: string;
  options: string;
  ro: boolean;
  enabled: boolean;
  has_password: boolean;
  cifs_username?: string;
  cifs_domain?: string;
  created_at: number;
  updated_at: number;
  active_state?: string;       // "active" / "inactive" / "failed" / "activating"
  last_status_at?: number;
  helper_status?: string;
}

export interface MountTestResponse {
  ok: boolean;
  helper_status?: string;
  error?: string;
}

// ─── Stress / profiler ────────────────────────────────────────────────────────
export type StressConfig        = components['schemas']['StressConfig'];
export type StressStatus        = components['schemas']['StressStatus'];
export type StressReportSummary = components['schemas']['StressReportSummary'];
export type StressReport        = components['schemas']['StressReport'];
export type ProfileSnapshot     = components['schemas']['ProfileSnapshot'];

// ─── Frontend-only types (no openapi counterpart) ─────────────────────────────

/**
 * Normalised error envelope returned by /api/* endpoints.
 * Maps onto the backend's `ErrorEnvelope` schema (RFC-7807-ish).
 * Kept hand-defined so the API client can construct/throw it cleanly.
 */
export interface ApiError {
  error: string;
  detail?: string;
}

/**
 * SSE event union — the wire shape of `/api/events/stream`.
 * Fields reflect the backend `EventBus` payload (src/api/EventBus.cpp).
 * Frontend code branches on `type` for query invalidation.
 */
export interface SseEvent {
  type: string;
  channel_id?: number;
  ts?: number;
  // Discriminated payload — bus emits arbitrary fields per event type.
  // Consumers cast or narrow by `type` as needed.
  [key: string]: unknown;
}
