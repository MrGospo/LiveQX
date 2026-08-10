/**
 * UI store — sidebar, toasts, LDAP banner, SSE connection state.
 */
import { create } from 'zustand';
import { persist, createJSONStorage } from 'zustand/middleware';

export type ToastType = 'success' | 'danger' | 'warning' | 'info';

export interface Toast {
  id: string;
  message: string;
  type: ToastType;
}

interface UiState {
  sidebarCollapsed: boolean;
  setSidebarCollapsed: (v: boolean) => void;
  toggleSidebar: () => void;

  toasts: Toast[];
  addToast: (message: string, type?: ToastType, durationMs?: number) => void;
  removeToast: (id: string) => void;

  ldapDown: boolean;
  lastLdapError: string | null;
  setLdapDown: (v: boolean, reason?: string) => void;

  sseConnected: boolean;
  sseOverflowed: boolean;
  setSseConnected: (v: boolean) => void;
  setSseOverflowed: (v: boolean) => void;
}

export const useUiStore = create<UiState>()(
  persist(
    (set, get) => ({
      sidebarCollapsed: false,
      setSidebarCollapsed: (v) => set({ sidebarCollapsed: v }),
      toggleSidebar: () => set((s) => ({ sidebarCollapsed: !s.sidebarCollapsed })),

      toasts: [],
      addToast: (message, type = 'info', durationMs = 5000) => {
        const id = `${Date.now()}-${Math.random()}`;
        set((s) => ({ toasts: [...s.toasts, { id, message, type }] }));
        if (durationMs > 0) {
          setTimeout(() => get().removeToast(id), durationMs);
        }
      },
      removeToast: (id) =>
        set((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) })),

      ldapDown: false,
      lastLdapError: null,
      setLdapDown: (v, reason) =>
        set({ ldapDown: v, lastLdapError: reason ?? null }),

      sseConnected: false,
      sseOverflowed: false,
      setSseConnected: (v) => set({ sseConnected: v }),
      setSseOverflowed: (v) => set({ sseOverflowed: v }),
    }),
    {
      name: 'sc-ui',
      storage: createJSONStorage(() => localStorage),
      partialize: (s) => ({ sidebarCollapsed: s.sidebarCollapsed }),
    },
  ),
);
