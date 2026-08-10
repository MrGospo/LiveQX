import type { SubNavItem } from '@/components/SubNav';

export const OBSERVABILITY_NAV: SubNavItem[] = [
  { to: '/observability/metrics', labelKey: 'observability.metrics' },
  { to: '/observability/events',  labelKey: 'observability.events'  },
  { to: '/observability/health',  labelKey: 'observability.health'  },
];
