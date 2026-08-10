import { useMutation } from '@tanstack/react-query';
import { api } from '../client';

// ─── Types ────────────────────────────────────────────────────────────────────
// The /api/streams/probe endpoint isn't in openapi.yaml yet (added in the
// mux-composer feature), so shapes are hand-written here. Move to
// components['schemas'] once the schema is regenerated.

export interface ProbeStreamRequest {
  address: string;
  port: number;
  interface_name?: string;
  interface_addr?: string;
  duration_ms?: number;     // clamped 200..10000 server-side
  recv_buffer_kb?: number;
}

export interface ProbeStreamInfo {
  pid: number;
  stream_type: number;
  codec: string;
  language: string;
  bitrate_bps: number;
}

export interface ProbeProgramInfo {
  program_number: number;
  pmt_pid: number;
  pcr_pid: number;
  service_name: string;
  provider_name: string;
  streams: ProbeStreamInfo[];
}

export interface ProbeStreamResult {
  transport_stream_id: number;
  original_network_id: number;
  total_bitrate_bps: number;
  bytes_received: number;
  packets_received: number;
  duration_ms: number;
  programs: ProbeProgramInfo[];
}

// ─── Hook ─────────────────────────────────────────────────────────────────────
// Mutation rather than useQuery — the probe is an explicit user action, not
// derived server state to cache. Each click sends a fresh sniff and the result
// is meaningful only for the config the user just typed.
export const useProbeStream = () =>
  useMutation({
    mutationFn: (body: ProbeStreamRequest) =>
      api.post<ProbeStreamResult>('/api/streams/probe', body),
  });
