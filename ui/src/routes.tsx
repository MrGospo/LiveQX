import React, { Suspense } from 'react';
import { Routes, Route, Navigate } from 'react-router-dom';
import App from '@/App';
import { useAuthStore } from '@/stores/auth';
import type { Role } from '@/api/types';

// ─── Lazy imports ─────────────────────────────────────────────────────────────
const LoginPage                = React.lazy(() => import('@/features/auth/LoginPage'));
const ProfilePage              = React.lazy(() => import('@/features/auth/ProfilePage'));
const ForceChangePasswordPage  = React.lazy(() => import('@/features/auth/ForceChangePasswordPage'));

const DashboardPage            = React.lazy(() => import('@/features/channels/DashboardPage'));
const ChannelsListPage         = React.lazy(() => import('@/features/channels/ChannelsListPage'));
const ChannelDetailPage        = React.lazy(() => import('@/features/channels/ChannelDetailPage'));
const CreateChannelPage        = React.lazy(() => import('@/features/channels/CreateChannelPage'));
const ChannelPreviewPage       = React.lazy(() => import('@/features/channels/ChannelPreviewPage'));


const GatewaysListPage         = React.lazy(() => import('@/features/gateways/GatewaysListPage'));
const GatewayCreatePage        = React.lazy(() => import('@/features/gateways/GatewayCreatePage'));
const GatewayDetailPage        = React.lazy(() => import('@/features/gateways/GatewayDetailPage'));

const PluginsListPage          = React.lazy(() => import('@/features/plugins/PluginsListPage'));
const PluginDetailPage         = React.lazy(() => import('@/features/plugins/PluginDetailPage'));
const PluginInstallPage        = React.lazy(() => import('@/features/plugins/PluginInstallPage'));

const MetricsPage              = React.lazy(() => import('@/features/observability/MetricsPage'));
const EventsPage               = React.lazy(() => import('@/features/observability/EventsPage'));
const HealthPage               = React.lazy(() => import('@/features/observability/HealthPage'));

const StressOverviewPage       = React.lazy(() => import('@/features/operations/StressOverviewPage'));
const StressReportPage         = React.lazy(() => import('@/features/operations/StressReportPage'));
const PerfPage                 = React.lazy(() => import('@/features/operations/PerfPage'));
const SystemPage               = React.lazy(() => import('@/features/operations/SystemPage'));

const DocsPage                 = React.lazy(() => import('@/features/docs/DocsPage'));

const UsersPage                = React.lazy(() => import('@/features/settings/UsersPage'));
const CreateUserPage           = React.lazy(() => import('@/features/settings/CreateUserPage'));
const UserDetailPage           = React.lazy(() => import('@/features/settings/UserDetailPage'));
const LdapPage                 = React.lazy(() => import('@/features/settings/LdapPage'));
const SmtpPage                 = React.lazy(() => import('@/features/settings/SmtpPage'));
const AuditPage                = React.lazy(() => import('@/features/settings/AuditPage'));
const MasterKeyPage            = React.lazy(() => import('@/features/settings/MasterKeyPage'));
const TlsPage                  = React.lazy(() => import('@/features/settings/TlsPage'));
const StoragePage              = React.lazy(() => import('@/features/settings/StoragePage'));
const SystemTimePage           = React.lazy(() => import('@/features/settings/SystemTimePage'));
const AboutPage                = React.lazy(() => import('@/features/settings/AboutPage'));

// ─── Guards ───────────────────────────────────────────────────────────────────
const ROLE_RANK: Record<Role, number> = { viewer: 0, operator: 1, admin: 2 };

function RequireAuth({ children }: { children: React.ReactNode }) {
  const token = useAuthStore(s => s.accessToken);
  const must  = useAuthStore(s => s.user?.must_change_password);
  if (!token) return <Navigate to="/login" replace />;
  // must_change_password guard: redirect to dedicated full-page route (§ 13.2)
  if (must)   return <Navigate to="/profile/change-password" replace />;
  return <>{children}</>;
}

function RequireRole({ minRole, children }: { minRole: Role; children: React.ReactNode }) {
  const role = useAuthStore(s => s.role);
  if (!role || ROLE_RANK[role] < ROLE_RANK[minRole]) return <Navigate to="/dashboard" replace />;
  return <>{children}</>;
}

function ForceChangeRoute() {
  const token = useAuthStore(s => s.accessToken);
  if (!token) return <Navigate to="/login" replace />;
  return <ForceChangePasswordPage />;
}

const Loader = () => (
  <div className="flex items-center justify-center h-64 text-[var(--text-muted)] text-sm">
    Loading…
  </div>
);

// ─── Routes ───────────────────────────────────────────────────────────────────
export function AppRoutes() {
  return (
    <Suspense fallback={<Loader />}>
      <Routes>
        {/* ── Public ─────────────────────────────────────────────────────── */}
        <Route path="/login" element={<LoginPage />} />

        {/* ── Force-change-password — no shell (sidebar hidden per § 13.2) ─ */}
        <Route path="/profile/change-password" element={<ForceChangeRoute />} />

        {/* ── Protected — wrapped in App shell ───────────────────────────── */}
        <Route element={<RequireAuth><App /></RequireAuth>}>
          <Route index element={<Navigate to="/dashboard" replace />} />
          <Route path="/dashboard" element={<DashboardPage />} />
          <Route path="/profile"   element={<ProfilePage />} />

          {/* Channels */}
          <Route path="/channels"           element={<ChannelsListPage />} />
          <Route path="/channels/new"       element={<RequireRole minRole="admin"><CreateChannelPage /></RequireRole>} />
          <Route path="/channels/:id"       element={<ChannelDetailPage />} />
          <Route path="/channels/:id/preview" element={<ChannelPreviewPage />} />

          {/* Gateways */}
          <Route path="/gateways"           element={<RequireRole minRole="admin"><GatewaysListPage /></RequireRole>} />
          <Route path="/gateways/new"       element={<RequireRole minRole="admin"><GatewayCreatePage /></RequireRole>} />
          <Route path="/gateways/:id"       element={<RequireRole minRole="admin"><GatewayDetailPage /></RequireRole>} />

          {/* Plugins */}
          <Route path="/plugins"            element={<RequireRole minRole="admin"><PluginsListPage /></RequireRole>} />
          <Route path="/plugins/install"    element={<RequireRole minRole="admin"><PluginInstallPage /></RequireRole>} />
          <Route path="/plugins/:name"      element={<RequireRole minRole="admin"><PluginDetailPage /></RequireRole>} />


          {/* Observability */}
          <Route path="/observability/metrics" element={<MetricsPage />} />
          <Route path="/observability/events"  element={<EventsPage />} />
          <Route path="/observability/health"  element={<HealthPage />} />

          {/* Operations */}
          <Route path="/operations/stress"            element={<RequireRole minRole="operator"><StressOverviewPage /></RequireRole>} />
          <Route path="/operations/stress/:reportId"  element={<RequireRole minRole="admin"><StressReportPage /></RequireRole>} />
          <Route path="/operations/perf"              element={<RequireRole minRole="operator"><PerfPage /></RequireRole>} />
          <Route path="/operations/system"            element={<RequireRole minRole="admin"><SystemPage /></RequireRole>} />

          {/* Settings */}
          <Route path="/settings/users"      element={<RequireRole minRole="admin"><UsersPage /></RequireRole>} />
          <Route path="/settings/users/new"  element={<RequireRole minRole="admin"><CreateUserPage /></RequireRole>} />
          <Route path="/settings/users/:id"  element={<RequireRole minRole="admin"><UserDetailPage /></RequireRole>} />
          <Route path="/settings/ldap"       element={<RequireRole minRole="admin"><LdapPage /></RequireRole>} />
          <Route path="/settings/smtp"       element={<RequireRole minRole="admin"><SmtpPage /></RequireRole>} />
          <Route path="/settings/audit"      element={<RequireRole minRole="admin"><AuditPage /></RequireRole>} />
          <Route path="/settings/master-key" element={<RequireRole minRole="admin"><MasterKeyPage /></RequireRole>} />
          <Route path="/settings/tls"        element={<RequireRole minRole="admin"><TlsPage /></RequireRole>} />
          <Route path="/settings/storage"    element={<RequireRole minRole="admin"><StoragePage /></RequireRole>} />
          <Route path="/settings/time"       element={<RequireRole minRole="admin"><SystemTimePage /></RequireRole>} />
          <Route path="/settings/about"      element={<AboutPage />} />

          {/* Docs */}
          <Route path="/docs" element={<DocsPage />} />

          <Route path="*" element={<Navigate to="/dashboard" replace />} />
        </Route>
      </Routes>
    </Suspense>
  );
}

