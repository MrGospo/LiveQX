import { NavLink } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { useAuthStore } from '@/stores/auth';
import type { Role } from '@/api/types';

export interface SubNavItem {
  to: string;
  labelKey: string;
  minRole?: Role;
}

const ROLE_RANK: Record<Role, number> = { viewer: 0, operator: 1, admin: 2 };

function hasMinRole(userRole: Role | null, minRole?: Role): boolean {
  if (!minRole) return true;
  if (!userRole) return false;
  return ROLE_RANK[userRole] >= ROLE_RANK[minRole];
}

export function SubNav({ items }: { items: SubNavItem[] }) {
  const { t } = useTranslation();
  const role = useAuthStore(s => s.role);
  const visible = items.filter(it => hasMinRole(role, it.minRole));

  return (
    <nav className="flex gap-1 bg-surface border-b border-[var(--border-subtle)] -mx-7 -mt-7 px-7 mb-2" aria-label="Section navigation">
      {visible.map(({ to, labelKey }) => (
        <NavLink
          key={to}
          to={to}
          className={({ isActive }) =>
            [
              'relative px-3 py-2.5 text-sm font-medium whitespace-nowrap transition-colors outline-none',
              isActive
                ? 'text-[var(--accent)]'
                : 'text-[var(--text-muted)] hover:text-[var(--text-primary)]',
              isActive ? 'after:content-[""] after:absolute after:left-0 after:right-0 after:-bottom-px after:h-0.5 after:bg-[var(--accent)]' : '',
            ].join(' ')
          }
        >
          {t(labelKey)}
        </NavLink>
      ))}
    </nav>
  );
}
