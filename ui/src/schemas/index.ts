/**
 * Zod schemas for all forms.
 * Shared between UI validation and type narrowing.
 */
import { z } from 'zod';

// ─── Auth ─────────────────────────────────────────────────────────────────────

export const loginSchema = z.object({
  username: z.string().min(1, 'Required').max(64).regex(/^[a-zA-Z0-9._-]+$/, 'Invalid characters'),
  password: z.string().min(1, 'Required'),
});

export const changePasswordSchema = z
  .object({
    current_password: z.string().min(1, 'Required'),
    new_password: z.string().min(12, 'Minimum 12 characters'),
    confirm_password: z.string(),
  })
  .refine((d) => d.new_password === d.confirm_password, {
    message: 'Passwords do not match',
    path: ['confirm_password'],
  });

// ─── Users ────────────────────────────────────────────────────────────────────

export const createUserSchema = z.object({
  username: z
    .string()
    .min(1, 'Required')
    .max(64)
    .regex(/^[a-zA-Z0-9._-]+$/, 'Letters, digits, . _ - only'),
  email: z.string().email('Invalid email').optional().or(z.literal('')),
  role: z.enum(['viewer', 'operator', 'admin']),
  source: z.enum(['local', 'ldap']),
  password: z.string().min(8).optional().or(z.literal('')),
  must_change_password: z.boolean().default(true),
});

// ─── LDAP ─────────────────────────────────────────────────────────────────────

export const ldapConfigSchema = z.object({
  enabled: z.boolean(),
  server: z.string().url('Must be a valid URL (ldaps://…)').optional().or(z.literal('')),
  tls_mode: z.enum(['ldaps', 'starttls', 'none']).optional(),
  bind_dn: z.string().optional(),
  bind_password: z.string().optional(),
  base_dn: z.string().optional(),
  user_filter: z.string().optional(),
  email_attribute: z.string().optional(),
  group_attribute: z.string().optional(),
  recheck_groups_on_refresh: z.boolean().optional(),
});

// ─── SMTP ─────────────────────────────────────────────────────────────────────

export const smtpConfigSchema = z.object({
  enabled: z.boolean(),
  server: z.string().min(1, 'Required').optional().or(z.literal('')),
  port: z.number().int().min(1).max(65535).optional(),
  tls_mode: z.enum(['none', 'starttls', 'smtps']).optional(),
  username: z.string().optional(),
  password: z.string().optional(),
  from_email: z.string().email().optional().or(z.literal('')),
  from_name: z.string().optional(),
});

// ─── Channel ──────────────────────────────────────────────────────────────────

export const channelConfigSchema = z.object({
  name: z.string().min(1).max(64).regex(/^[a-zA-Z0-9_-]+$/),
  resolution: z.string().regex(/^\d+x\d+$/, 'e.g. 1920x1080'),
  fps: z.number().int().min(1).max(120),
  bitrate_kbps: z.number().int().min(100).max(100_000),
  encoder_mode: z.enum(['auto', 'cpu', 'nvenc', 'qsv', 'vaapi']),
  numa_node: z.number().int().min(0).optional(),
});

// ─── Output ───────────────────────────────────────────────────────────────────

export const outputSrtSchema = z.object({
  id: z.string().min(1).max(64),
  type: z.literal('srt'),
  address: z.string().min(1),
  port: z.number().int().min(1).max(65535),
  mode: z.enum(['caller', 'listener', 'rendezvous']).optional(),
  latency_ms: z.number().int().min(20).max(8000).optional(),
});

export const outputRtmpSchema = z.object({
  id: z.string().min(1).max(64),
  type: z.literal('rtmp'),
  url: z.string().url(),
  mode: z.enum(['client', 'server']).optional(),
});

export const outputHlsSchema = z.object({
  id: z.string().min(1).max(64),
  type: z.literal('hls'),
  dir: z.string().min(1),
  segment_duration_sec: z.number().min(0.5).max(30).optional(),
});

// ─── Stress ───────────────────────────────────────────────────────────────────

export const stressConfigSchema = z.object({
  scenario: z.enum(['smoke', 'sustained', 'spike']),
  channels: z.number().int().min(1),
  duration_sec: z.number().int().min(1),
  cron: z.string().nullable().optional(),
  warmup_sec: z.number().int().min(0).optional(),
});

export type LoginFormValues          = z.infer<typeof loginSchema>;
export type ChangePasswordFormValues = z.infer<typeof changePasswordSchema>;
export type CreateUserFormValues     = z.infer<typeof createUserSchema>;
export type LdapConfigFormValues     = z.infer<typeof ldapConfigSchema>;
export type SmtpConfigFormValues     = z.infer<typeof smtpConfigSchema>;
export type ChannelConfigFormValues  = z.infer<typeof channelConfigSchema>;
export type StressConfigFormValues   = z.infer<typeof stressConfigSchema>;
