import type { SubNavItem } from '@/components/SubNav';

// Sections of the screens (players) area. More join as the screens do
// (the list, the tree, rights, commands, audit).
export const PLAYERS_NAV: SubNavItem[] = [
  { to: '/players/settings', labelKey: 'nav.playersSettings', minRole: 'admin' },
  { to: '/players/broker',   labelKey: 'nav.playersBroker',   minRole: 'admin' },
];
