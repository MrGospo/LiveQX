import { useQuery, useMutation, useQueryClient, keepPreviousData } from '@tanstack/react-query';
import { api } from '../client';

// Screens (players) — module settings and the MQTT broker link.
//
// Query keys: ['players', …] belong to what live events change (the list of
// screens, their commands). Settings and the broker state are administrator
// pages that are not changed by a screen coming and going, so they live under
// their own root and are not refetched on every event.

// ─── Module settings (GET/PUT /api/players/settings) ──────────────────────────

export type ClockPolicy = 'strict' | 'relaxed';

export interface PlayersSettings {
  server_url:      string;
  device_api_url:  string;
  mqtt_broker_url: string;
  ca_cert_pem:     string;
  token_ttl_days:  number;
  clock_policy:    ClockPolicy;
  command_ttl_s:              number;
  confirm_timeout_restart_s:  number;
  confirm_timeout_reboot_s:   number;
  confirm_timeout_reload_s:   number;
  command_batch_size:         number;
  command_batch_stop_percent: number;
  events_retention_days:      number;
  commands_retention_days:    number;
}

// The server takes only the keys that are present; an empty string or null
// puts the setting back to its default.
export type PlayersSettingsPatch = Partial<{
  [K in keyof PlayersSettings]: PlayersSettings[K] | '' | null;
}>;

export const usePlayersSettings = () =>
  useQuery({
    queryKey: ['players-admin', 'settings'],
    queryFn: () => api.get<PlayersSettings>('/api/players/settings'),
    staleTime: 10_000,
  });

export const useSavePlayersSettings = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (patch: PlayersSettingsPatch) => api.put<PlayersSettings>('/api/players/settings', patch),
    onSuccess: (saved) => {
      qc.setQueryData(['players-admin', 'settings'], saved);
      qc.invalidateQueries({ queryKey: ['players-admin', 'broker'] });   // the readiness list depends on it
    },
  });
};

// ─── Broker link (GET /api/players/broker and friends) ────────────────────────

export type BrokerState = 'disabled' | 'waiting' | 'connecting' | 'connected' | 'stopped';

export interface BrokerCrlStatus {
  mode:             'off' | 'local';
  last_publish_at?: number;
  last_ok?:         boolean;
  error?:           string;
  reload_error?:    string;
  revoked?:         number;
  generations?:     number;
  valid_until?:     number;
}

export interface BrokerStatus {
  enabled:  boolean;
  state:    BrokerState;
  detail?:  string;
  host?:    string;
  port?:    number;
  verify_hostname?:      boolean;
  reconcile_interval_s?: number;
  last_reconcile_at?: number;
  last_reconcile_ok?: boolean;
  last_report?: { issued: number; removed: number; failed: number; error: string };
  crl?: BrokerCrlStatus;
  readiness?: {
    mosquitto_in_build:    boolean;
    device_ca:             boolean;
    trust_root:            boolean;
    broker_url_configured: boolean;
  };
}

export interface BrokerConnection {
  host: string;
  port: number;
  verify_hostname: boolean;
  reconcile_interval_s: number;
}

export interface BrokerConnectionPatch {
  host?: string;
  port?: number;
  verify_hostname?: boolean;
  reconcile_interval_s?: number;
  /** Only try the connection; change nothing. */
  check_only?: boolean;
  /** Apply without trying (the broker is not up yet). */
  skip_check?: boolean;
}

export interface BrokerCheckResult {
  checked: true;
  ok: boolean;
  detail: string;
  connection: BrokerConnection;
}
export interface BrokerApplyResult {
  applied: boolean;
  unchanged?: boolean;
  checked?: boolean;
  warning?: string;
  connection: BrokerConnection;
}

export const useBrokerStatus = () =>
  useQuery({
    queryKey: ['players-admin', 'broker'],
    queryFn: () => api.get<BrokerStatus>('/api/players/broker'),
    staleTime: 10_000,                      // the server announces every change (players_broker_status)
  });

export const useBrokerReconcile = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: () => api.post<{ requested: boolean }>('/api/players/broker/reconcile'),
    // The pass runs on the server's own thread; look again shortly.
    onSuccess: () => { setTimeout(() => qc.invalidateQueries({ queryKey: ['players-admin', 'broker'] }), 1_500); },
  });
};

export const useBrokerUpdate = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (patch: BrokerConnectionPatch) =>
      api.put<BrokerCheckResult | BrokerApplyResult>('/api/players/broker', patch),
    onSuccess: (_r, patch) => {
      if (!patch.check_only) qc.invalidateQueries({ queryKey: ['players-admin', 'broker'] });
    },
  });
};

// ─── Objects and groups (/api/player-objects, /api/player-groups) ─────────────

export interface PlayerGroup {
  id:         number;
  object_id:  number;
  name:       string;
  created_at: number;
  players:    number;
  // What THIS person may do in the group; the server still decides every action.
  can?:       { move: boolean; command_group: boolean };
}

export interface PlayerObject {
  id:         number;
  name:       string;
  created_at: number;
  groups:     PlayerGroup[];
  can?:       { command_object: boolean };
}

// A shared root so one invalidation refreshes the tree after any change; the
// screens' own events also land here (a screen moving between groups changes counts).
export const PLAYER_TREE_KEY = ['players', 'tree'] as const;

export const usePlayerTree = () =>
  useQuery({
    queryKey: PLAYER_TREE_KEY,
    queryFn: async () => (await api.get<{ objects: PlayerObject[] }>('/api/player-objects')).objects,
    staleTime: 5_000,
  });

export const usePlayerTreeMutations = () => {
  const qc = useQueryClient();
  const refresh = () => qc.invalidateQueries({ queryKey: PLAYER_TREE_KEY });
  return {
    createObject: useMutation({
      mutationFn: (name: string) => api.post<{ id: number; name: string }>('/api/player-objects', { name }),
      onSuccess: refresh,
    }),
    renameObject: useMutation({
      mutationFn: (v: { id: number; name: string }) => api.put(`/api/player-objects/${v.id}`, { name: v.name }),
      onSuccess: refresh,
    }),
    deleteObject: useMutation({
      mutationFn: (id: number) => api.delete(`/api/player-objects/${id}`),
      onSuccess: refresh,
    }),
    createGroup: useMutation({
      mutationFn: (v: { objectId: number; name: string }) =>
        api.post<{ id: number; name: string }>(`/api/player-objects/${v.objectId}/groups`, { name: v.name }),
      onSuccess: refresh,
    }),
    renameGroup: useMutation({
      mutationFn: (v: { id: number; name: string }) => api.put(`/api/player-groups/${v.id}`, { name: v.name }),
      onSuccess: refresh,
    }),
    deleteGroup: useMutation({
      mutationFn: (id: number) => api.delete(`/api/player-groups/${id}`),
      onSuccess: refresh,
    }),
  };
};

// ─── Screens (GET /api/players, /api/players/{id}, …/commands) ────────────────

export type PlayerLifecycle = 'pending' | 'active' | 'revoked';
export type PlayerHealth = 'ok' | 'warning' | 'critical';

// What the screen last reported about itself (a cleaned heartbeat; every field is optional).
export interface PlayerReport {
  health?:             PlayerHealth;
  watchdog_state?:     'ok' | 'stale' | 'not_running';
  uptime_seconds?:     number;
  os_uptime_seconds?:  number;
  position_ms?:        number;
  buffer_progress?:    number;
  current_source?:     string;
  media_status?:       string;
  last_error?:         string;
  last_renew_error?:   string;
  last_command_id?:    string;
  last_command_result?: { id: string; status: string };
}

export interface PlayerStatus {
  online:     boolean;
  version:    string;
  updated_at: number;
  state?:     PlayerReport;
}

export interface Player {
  id:             string;
  name:           string;
  state:          PlayerLifecycle;
  clock_policy:   ClockPolicy;
  created_at:     number;
  group_id?:      number;
  group_name?:    string;
  object_id?:     number;
  object_name?:   string;
  created_by?:    number;
  activated_at?:  number;
  revoked_at?:    number;
  revoke_reason?: string;
  status?:        PlayerStatus;
  // What THIS person may do with the screen (the card only).
  can?:           { move: boolean; command: boolean };
}

export interface PlayerCertificate {
  serial:         string;
  generation:     number;
  issued_at:      number;
  not_before:     number;
  not_after:      number;
  revoked_at?:    number;
  revoke_reason?: string;
}

// `details` is JSON text (or empty) whose shape depends on `kind`.
export interface PlayerEvent { id: number; ts: number; kind: string; details: string }

export interface PlayerDetails extends Player {
  certificates: PlayerCertificate[];
  events:       PlayerEvent[];
}

export type CommandState = 'pending' | 'executed' | 'failed' | 'timeout' | 'released' | 'not_sent';

export interface PlayerCommand {
  id:          string;
  player_id:   string;
  type:        string;
  state:       CommandState;
  result:      string;
  issued_at:   number;
  ttl_seconds: number;
  deadline_at: number;
  issued_by?:  number;
  finished_at?: number;
}

export type ScreenStatusFilter = 'online' | 'offline' | 'pending' | 'revoked';

export interface PlayerListQuery {
  objectId?: number;
  groupId?:  number;
  status?:   ScreenStatusFilter;
  q?:        string;
  page:      number;               // 1-based
}

export const PLAYER_PAGE_SIZE = 50;

export interface PlayerPage {
  players: Player[];
  total:   number;                                    // matching every filter, before the page is cut
  counts:  Record<ScreenStatusFilter, number>;        // by status, for the place and the search (not the status filter)
}

// Filtering, search, counting and paging are the server's work, so a page of a
// large park is complete and the counts are true for all of it.
export const usePlayers = (f: PlayerListQuery) =>
  useQuery({
    queryKey: ['players', 'list', f.objectId ?? null, f.groupId ?? null, f.status ?? null, f.q ?? '', f.page],
    queryFn: () => {
      const q = new URLSearchParams({
        limit: String(PLAYER_PAGE_SIZE), offset: String((f.page - 1) * PLAYER_PAGE_SIZE),
      });
      if (f.groupId  != null) q.set('group_id',  String(f.groupId));
      if (f.objectId != null) q.set('object_id', String(f.objectId));
      if (f.status) q.set('status', f.status);
      if (f.q) q.set('q', f.q);
      return api.get<PlayerPage>(`/api/players?${q}`);
    },
    staleTime: 5_000,
    placeholderData: keepPreviousData,          // the table stays while the next page loads
  });

export const usePlayer = (id: string) =>
  useQuery({
    queryKey: ['players', 'one', id],
    queryFn: () => api.get<PlayerDetails>(`/api/players/${encodeURIComponent(id)}`),
    staleTime: 5_000,
  });

export const usePlayerCommands = (id: string) =>
  useQuery({
    queryKey: ['players', 'commands', id],
    queryFn: async () => (await api.get<{ commands: PlayerCommand[] }>(`/api/players/${encodeURIComponent(id)}/commands`)).commands,
    staleTime: 5_000,
  });

// ─── Actions on screens ───────────────────────────────────────────────────────

// The pairing package: everything a new screen needs, the one-time token included.
// The server shows it once, so it is never put in a query cache or in storage.
export interface PairingPackage {
  schema:          number;
  player_id:       string;
  server_url:      string;
  ca_cert_pem:     string;
  device_api_url:  string;
  mqtt_broker_url: string;
  bootstrap_token: string;
  token_expires_at: string;
  clock_policy:    ClockPolicy;
  ntp_hint?:       unknown;
}

export interface NewPlayerInput { name: string; group_id: number; clock_policy?: ClockPolicy }

export const usePlayerActions = () => {
  const qc = useQueryClient();
  const refresh = () => qc.invalidateQueries({ queryKey: ['players'] });
  const enc = encodeURIComponent;
  return {
    // gcTime 0: a package must not linger in the cache once the dialog that shows it is gone.
    create: useMutation({
      mutationFn: (v: NewPlayerInput) => api.post<PairingPackage>('/api/players', v),
      onSuccess: refresh, gcTime: 0,
    }),
    reissueToken: useMutation({
      mutationFn: (id: string) => api.post<PairingPackage>(`/api/players/${enc(id)}/reissue-token`),
      onSuccess: refresh, gcTime: 0,
    }),
    // Binds an enrolled or revoked screen again under the same identity.
    reEnroll: useMutation({
      mutationFn: (v: { id: string; reason?: string }) =>
        api.post<PairingPackage>(`/api/players/${enc(v.id)}/re-enroll`, v.reason ? { reason: v.reason } : {}),
      onSuccess: refresh, gcTime: 0,
    }),
    move: useMutation({
      mutationFn: (v: { id: string; groupId: number }) => api.put(`/api/players/${enc(v.id)}/group`, { group_id: v.groupId }),
      onSuccess: refresh,
    }),
    revoke: useMutation({
      mutationFn: (v: { id: string; reason?: string }) =>
        api.post<{ revoked_certs: number }>(`/api/players/${enc(v.id)}/revoke-cert`, v.reason ? { reason: v.reason } : {}),
      onSuccess: refresh,
    }),
    remove: useMutation({
      mutationFn: (id: string) => api.delete<{ revoked_certs: number }>(`/api/players/${enc(id)}`),
      onSuccess: refresh,
    }),
  };
};
