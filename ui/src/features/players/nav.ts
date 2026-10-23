import type { SubNavItem } from '@/components/SubNav';

// Sections of the screens (players) area. More join as the screens do
// (rights, commands, audit).
export const PLAYERS_NAV: SubNavItem[] = [
  { to: '/players',          labelKey: 'nav.playersList',     end: true },          // every role: the server cuts it to their rights
  { to: '/players/jobs',     labelKey: 'nav.playersJobs',     minRole: 'operator' },
  { to: '/players/objects',  labelKey: 'nav.playersObjects',  minRole: 'admin' },
  { to: '/players/settings', labelKey: 'nav.playersSettings', minRole: 'admin' },
  { to: '/players/broker',   labelKey: 'nav.playersBroker',   minRole: 'admin' },
];
