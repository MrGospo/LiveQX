import React from 'react';
import { useAuthStore } from '@/stores/auth';

// Single-flight refresh: StrictMode двойной маунт + любой новый <AuthBootstrap>
// получают ту же promise — refresh-токен ротируется на бэке, второй вызов
// со старым RT упал бы 401.
let bootstrapPromise: Promise<void> | null = null;

function runBootstrap(): Promise<void> {
  if (bootstrapPromise) return bootstrapPromise;
  bootstrapPromise = (async () => {
    const s = useAuthStore.getState();
    if (s.accessToken)   return;            // уже залогинен в этой вкладке
    if (!s.refreshToken) return;            // нечего восстанавливать
    try {
      const res = await fetch('/api/auth/refresh', {
        method:  'POST',
        headers: { 'Content-Type': 'application/json' },
        body:    JSON.stringify({ refresh_token: s.refreshToken }),
      });
      if (!res.ok) throw new Error(`refresh ${res.status}`);
      const data = await res.json();
      useAuthStore.getState().setTokens(data.access_token, data.refresh_token, data.expires_in);
    } catch {
      // RT недействителен/протух — сбрасываем persist, RequireAuth уведёт на /login
      useAuthStore.getState().logout();
    }
  })();
  return bootstrapPromise;
}

export function AuthBootstrap({ children }: { children: React.ReactNode }) {
  const [ready, setReady] = React.useState(() => {
    // Если в localStorage нет RT — bootstrap не нужен, рендерим сразу
    const s = useAuthStore.getState();
    return s.accessToken !== null || s.refreshToken === null;
  });

  React.useEffect(() => {
    if (ready) return;
    let cancelled = false;
    runBootstrap().finally(() => { if (!cancelled) setReady(true); });
    return () => { cancelled = true; };
  }, [ready]);

  if (!ready) {
    return (
      <div className="flex items-center justify-center h-screen bg-canvas text-[var(--text-muted)] text-sm">
        …
      </div>
    );
  }
  return <>{children}</>;
}
