import React from 'react';
import { Outlet, NavLink, useNavigate } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import {
  LayoutDashboard, Tv2, Network, Puzzle,
  Activity, Wrench, Settings, ChevronLeft, ChevronRight,
  LogOut, User, AlertTriangle, X, BookOpen,
} from 'lucide-react';
import { useAuthStore } from '@/stores/auth';
import { useUiStore } from '@/stores/ui';
import { useUiStore as useToastUi } from '@/stores/ui';
import type { Role } from '@/api/types';

// ─── Nav item config ─────────────────────────────────────────────────────────
interface NavItem {
  path: string;
  labelKey: string;
  Icon: React.ElementType;
  minRole?: Role;
}

const NAV: NavItem[] = [
  { path: '/dashboard',             labelKey: 'nav.dashboard',     Icon: LayoutDashboard },
  { path: '/channels',              labelKey: 'nav.channels',       Icon: Tv2 },
  { path: '/gateways',              labelKey: 'nav.gateways',       Icon: Network,  minRole: 'admin' },
  { path: '/plugins',               labelKey: 'nav.plugins',        Icon: Puzzle,   minRole: 'admin' },
  { path: '/observability/metrics', labelKey: 'nav.observability',  Icon: Activity },
  { path: '/operations/stress',     labelKey: 'nav.operations',     Icon: Wrench,   minRole: 'operator' },
  { path: '/settings/users',        labelKey: 'nav.settings',       Icon: Settings },
];

const ROLE_RANK: Record<Role, number> = { viewer: 0, operator: 1, admin: 2 };
function hasMinRole(userRole: Role | null, minRole?: Role): boolean {
  if (!minRole) return true;
  if (!userRole) return false;
  return ROLE_RANK[userRole] >= ROLE_RANK[minRole];
}

// ─── Sidebar ─────────────────────────────────────────────────────────────────
function Sidebar() {
  const { t } = useTranslation();
  const collapsed = useUiStore(s => s.sidebarCollapsed);
  const toggle    = useUiStore(s => s.toggleSidebar);
  const role      = useAuthStore(s => s.role);

  const visible = NAV.filter(n => hasMinRole(role, n.minRole));

  return (
    <aside
      style={{ width: collapsed ? 64 : 220 }}
      className="flex-shrink-0 flex flex-col bg-canvas border-r border-[var(--border-subtle)] transition-all duration-200 overflow-hidden"
    >
      <nav className="flex-1 py-2 overflow-y-auto">
        {visible.map(({ path, labelKey, Icon }) => (
          <NavLink
            key={path}
            to={path}
            aria-label={collapsed ? t(labelKey) : undefined}
            className={({ isActive }) =>
              [
                'flex items-center gap-2.5 w-full h-10 px-3 text-sm font-medium transition-colors',
                'border-l-2 outline-none',
                isActive
                  ? 'bg-surface text-[var(--text-primary)] border-[var(--accent)]'
                  : 'text-[var(--text-muted)] border-transparent hover:bg-surface2 hover:text-[var(--text-primary)]',
              ].join(' ')
            }
          >
            <Icon size={20} className="flex-shrink-0" />
            {!collapsed && <span className="truncate">{t(labelKey)}</span>}
          </NavLink>
        ))}
      </nav>

      <NavLink
        to="/docs"
        aria-label={collapsed ? t('nav.docs') : undefined}
        className={({ isActive }) =>
          [
            'flex items-center gap-2.5 w-full h-10 px-3 text-sm font-medium transition-colors',
            'border-l-2 outline-none border-t border-t-[var(--border-subtle)]',
            isActive
              ? 'bg-surface text-[var(--text-primary)] border-l-[var(--accent)]'
              : 'text-[var(--text-muted)] border-l-transparent hover:bg-surface2 hover:text-[var(--text-primary)]',
          ].join(' ')
        }
      >
        <BookOpen size={20} className="flex-shrink-0" />
        {!collapsed && <span className="truncate">{t('nav.docs')}</span>}
      </NavLink>

      <button
        onClick={toggle}
        aria-label={collapsed ? 'Expand sidebar' : 'Collapse sidebar'}
        className="flex items-center justify-center h-10 text-[var(--text-muted)] hover:text-[var(--text-primary)] border-t border-[var(--border-subtle)] transition-colors"
      >
        {collapsed ? <ChevronRight size={16} /> : <ChevronLeft size={16} />}
      </button>
    </aside>
  );
}

// ─── Top bar ─────────────────────────────────────────────────────────────────
function TopBar() {
  const { t } = useTranslation();
  const navigate  = useNavigate();
  const user      = useAuthStore(s => s.user);
  const logout    = useAuthStore(s => s.logout);
  const ldapDown  = useUiStore(s => s.ldapDown);
  const setLdap   = useUiStore(s => s.setLdapDown);
  const addToast  = useToastUi(s => s.addToast);
  const sseConn   = useUiStore(s => s.sseConnected);

  const handleLogout = async () => {
    try { await fetch('/api/auth/logout', { method: 'POST', headers: { Authorization: `Bearer ${useAuthStore.getState().accessToken}` } }); }
    catch { /* ignore */ }
    logout();
    navigate('/login');
    addToast(t('common.sessionExpired'), 'info');
  };

  return (
    <header
      className="sticky top-0 z-50 flex-shrink-0 flex flex-col"
      style={{ height: ldapDown ? 'auto' : 56 }}
    >
      <div
        className="flex items-center gap-4 px-5 bg-surface border-b border-[var(--border-subtle)]"
        style={{ height: 56 }}
      >
        {/* Logo */}
        <div className="flex items-center gap-2.5 flex-shrink-0">
          <img src="/logo.svg" alt="LiveQX" className="w-7 h-7" />
          <span className="text-base font-bold text-[var(--text-primary)] tracking-wider">LiveQX</span>
        </div>

        <div className="flex-1" />

        {/* Live indicator */}
        <div className="flex items-center gap-1.5">
          <div
            className={`w-2 h-2 rounded-full ${sseConn ? 'bg-[var(--success)] animate-pulse' : 'bg-[var(--text-muted)]'}`}
            style={sseConn ? { boxShadow: '0 0 5px var(--success)' } : undefined}
          />
          <span className="text-xs text-[var(--text-muted)]">{sseConn ? 'LIVE' : 'offline'}</span>
        </div>

        {/* User menu */}
        {user && (
          <div className="flex items-center gap-2">
            <button
              onClick={() => navigate('/profile')}
              className="flex items-center gap-2 px-3 py-1.5 rounded-full bg-canvas border border-[var(--border-subtle)] text-sm font-medium text-[var(--text-primary)] hover:bg-surface2 transition-colors"
            >
              <div className="w-5 h-5 rounded-full bg-[var(--accent)]/20 flex items-center justify-center flex-shrink-0">
                <User size={11} className="text-[var(--accent)]" />
              </div>
              <span>{user.username}</span>
            </button>
            <button
              onClick={handleLogout}
              aria-label={t('auth.logout')}
              className="p-2 rounded-lg text-[var(--text-muted)] hover:text-[var(--danger)] hover:bg-[var(--danger)]/10 transition-colors"
            >
              <LogOut size={16} />
            </button>
          </div>
        )}
      </div>

      {/* LDAP banner */}
      {ldapDown && (
        <div className="flex items-center gap-2.5 px-5 py-2 bg-[var(--warning)]/10 border-b border-[var(--warning)]/30 text-sm">
          <AlertTriangle size={14} className="text-[var(--warning)] flex-shrink-0" />
          <span className="flex-1 text-[var(--text-primary)]">{t('ldap.banner')}</span>
          <button onClick={() => setLdap(false)} aria-label="Dismiss" className="text-[var(--text-muted)] hover:text-[var(--text-primary)]">
            <X size={14} />
          </button>
        </div>
      )}
    </header>
  );
}

// ─── Toast region ─────────────────────────────────────────────────────────────
function ToastRegion() {
  const toasts      = useUiStore(s => s.toasts);
  const removeToast = useUiStore(s => s.removeToast);

  const colorMap: Record<string, string> = {
    success: 'border-l-[var(--success)]',
    danger:  'border-l-[var(--danger)]',
    warning: 'border-l-[var(--warning)]',
    info:    'border-l-[var(--info)]',
  };

  return (
    <div
      aria-live="polite"
      aria-atomic="false"
      className="fixed bottom-4 right-4 z-[9999] flex flex-col gap-2 max-w-sm w-full pointer-events-none"
    >
      {toasts.map(t => (
        <div
          key={t.id}
          className={`animate-slide pointer-events-auto flex items-start gap-2.5 bg-surface border border-[var(--border-subtle)] border-l-4 ${colorMap[t.type] ?? colorMap.info} rounded-lg px-4 py-3 shadow-lg text-sm text-[var(--text-primary)]`}
        >
          <span className="flex-1 leading-relaxed">{t.message}</span>
          <button onClick={() => removeToast(t.id)} aria-label="Dismiss" className="text-[var(--text-muted)] hover:text-[var(--text-primary)] mt-0.5">
            <X size={13} />
          </button>
        </div>
      ))}
    </div>
  );
}

// ─── App shell ────────────────────────────────────────────────────────────────
export default function App() {
  return (
    <div className="flex flex-col h-screen overflow-hidden bg-canvas">
      <TopBar />
      <div className="flex flex-1 overflow-hidden">
        <Sidebar />
        <main className="flex-1 overflow-y-auto bg-canvas">
          <Outlet />
        </main>
      </div>
      <ToastRegion />
    </div>
  );
}
