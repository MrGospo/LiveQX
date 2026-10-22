import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
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
    staleTime: 2_000,
    refetchInterval: 5_000,                 // the state changes without any event to tell us
    refetchIntervalInBackground: false,
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
