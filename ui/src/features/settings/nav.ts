import type { SubNavItem } from '@/components/SubNav';

export const SETTINGS_NAV: SubNavItem[] = [
  { to: '/settings/users',      labelKey: 'nav.settingsUsers',     minRole: 'admin' },
  { to: '/settings/ldap',       labelKey: 'nav.settingsLdap',      minRole: 'admin' },
  { to: '/settings/smtp',       labelKey: 'nav.settingsSmtp',      minRole: 'admin' },
  { to: '/settings/audit',       labelKey: 'nav.settingsAudit',      minRole: 'admin' },
  { to: '/settings/audit-trail', labelKey: 'nav.settingsAuditTrail', minRole: 'admin' },
  { to: '/settings/master-key', labelKey: 'nav.settingsMasterKey', minRole: 'admin' },
  { to: '/settings/tls',        labelKey: 'nav.settingsTls',       minRole: 'admin' },
  { to: '/settings/storage',    labelKey: 'nav.settingsStorage',   minRole: 'admin' },
  { to: '/settings/time',       labelKey: 'nav.settingsTime',      minRole: 'admin' },
  { to: '/profile',             labelKey: 'nav.settingsProfile' },
  { to: '/settings/about',      labelKey: 'nav.settingsAbout' },
];
