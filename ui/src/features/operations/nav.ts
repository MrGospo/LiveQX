import type { SubNavItem } from '@/components/SubNav';

export const OPERATIONS_NAV: SubNavItem[] = [
  { to: '/operations/stress', labelKey: 'operations.stress', minRole: 'operator' },
  { to: '/operations/perf',   labelKey: 'operations.perf',   minRole: 'operator' },
  { to: '/operations/system', labelKey: 'operations.system', minRole: 'admin'    },
];
