/**
 * Auth store — JWT in-memory, refresh token in localStorage,
 * per-channel grants cache, RBAC helpers.
 */
import { create } from 'zustand';
import { persist, createJSONStorage } from 'zustand/middleware';
import type { Role, User } from '@/api/types';

interface AuthState {
  // JWT (in-memory only — NOT persisted)
  accessToken: string | null;
  expiresAt: number | null;          // unix ms

  // Stored in localStorage via zustand/persist
  refreshToken: string | null;
  user: Pick<User, 'id' | 'username' | 'role'> & { must_change_password?: boolean } | null;
  role: Role | null;

  // Per-channel grants (loaded after login, refreshed on SSE AuthAudit)
  channelGrants: Record<number, 'view' | 'operate'>;

  // Actions
  setTokens: (access: string, refresh: string, expiresIn: number) => void;
  setUser: (user: AuthState['user']) => void;
  setMustChangePassword: (v: boolean) => void;
  setChannelGrants: (grants: Record<number, 'view' | 'operate'>) => void;
  logout: () => void;

  // Helpers
  hasRole: (minRole: Role) => boolean;
  canAccessChannel: (channelId: number) => boolean;
}

const ROLE_RANK: Record<Role, number> = { viewer: 0, operator: 1, admin: 2 };

// Only persist refresh token + user summary
const persistedKeys = ['refreshToken', 'user', 'role'];

export const useAuthStore = create<AuthState>()(
  persist(
    (set, get) => ({
      accessToken: null,
      expiresAt: null,
      refreshToken: null,
      user: null,
      role: null,
      channelGrants: {},

      setTokens: (access, refresh, expiresIn) =>
        set({
          accessToken: access,
          refreshToken: refresh,
          expiresAt: Date.now() + expiresIn * 1000,
        }),

      setUser: (user) =>
        set({ user, role: user?.role ?? null }),

      setMustChangePassword: (v) =>
        set((s) => ({ user: s.user ? { ...s.user, must_change_password: v } : null })),

      setChannelGrants: (grants) => set({ channelGrants: grants }),

      logout: () =>
        set({
          accessToken: null,
          expiresAt: null,
          refreshToken: null,
          user: null,
          role: null,
          channelGrants: {},
        }),

      hasRole: (minRole) => {
        const r = get().role;
        if (!r) return false;
        return ROLE_RANK[r] >= ROLE_RANK[minRole];
      },

      canAccessChannel: (channelId) => {
        const state = get();
        // Admin sees all
        if (state.role === 'admin') return true;
        // Operator without explicit grant still has access (role overrides)
        if (state.role === 'operator') return true;
        // Viewer needs explicit grant
        return channelId in state.channelGrants;
      },
    }),
    {
      name: 'sc-auth',
      storage: createJSONStorage(() => localStorage),
      partialize: (s) =>
        Object.fromEntries(
          Object.entries(s).filter(([k]) => persistedKeys.includes(k)),
        ) as Partial<AuthState>,
    },
  ),
);
