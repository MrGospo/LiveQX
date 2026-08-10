import React from 'react';
import ReactDOM from 'react-dom/client';
import { BrowserRouter } from 'react-router-dom';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { ReactQueryDevtools } from '@tanstack/react-query-devtools';
import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import LanguageDetector from 'i18next-browser-languagedetector';

import { configureClient } from '@/api/client';
import { useAuthStore } from '@/stores/auth';
import { EventBusProvider } from '@/hooks/useEventStream';
import { AppRoutes } from '@/routes';
import { AuthBootstrap } from '@/components/AuthBootstrap';

import '@/styles/globals.css';
import enJson from '@/locales/en.json';
import ruJson from '@/locales/ru.json';

// ─── i18n ────────────────────────────────────────────────────────────────────
i18n
  .use(LanguageDetector)
  .use(initReactI18next)
  .init({
    resources: { en: { translation: enJson }, ru: { translation: ruJson } },
    fallbackLng: 'en',
    interpolation: { escapeValue: false },
    // Сначала смотрим явный выбор пользователя в localStorage, иначе
    // подхватываем locale браузера. Селектор языка в ProfilePage пишет
    // сюда же ключ 'i18nextLng' (см. i18next-browser-languagedetector).
    detection: {
      order: ['localStorage', 'navigator'],
      caches: ['localStorage'],
    },
  });

// ─── TanStack Query ───────────────────────────────────────────────────────────
const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      retry: 1,
      refetchOnWindowFocus: false,
      staleTime: 5_000,
    },
  },
});

// ─── API client wiring ────────────────────────────────────────────────────────
configureClient({
  getToken:       () => useAuthStore.getState().accessToken,
  doRefresh:      async () => {
    const rt = useAuthStore.getState().refreshToken;
    if (!rt) throw new Error('no refresh token');
    const res = await fetch('/api/auth/refresh', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ refresh_token: rt }),
    });
    if (!res.ok) throw new Error('refresh failed');
    const data = await res.json();
    useAuthStore.getState().setTokens(data.access_token, data.refresh_token, data.expires_in);
  },
  onUnauthorized: () => {
    useAuthStore.getState().logout();
    window.location.replace('/login');
  },
});

// ─── MSW (dev only) ───────────────────────────────────────────────────────────
async function prepare() {
  if (import.meta.env.DEV) {
    const { worker } = await import('@/mocks/browser');
    return worker.start({ onUnhandledRequest: 'bypass' });
  }
}

// ─── Root ─────────────────────────────────────────────────────────────────────
prepare().then(() => {
  ReactDOM.createRoot(document.getElementById('root')!).render(
    <React.StrictMode>
      <QueryClientProvider client={queryClient}>
        <BrowserRouter>
          <AuthBootstrap>
            <EventBusProvider>
              <AppRoutes />
            </EventBusProvider>
          </AuthBootstrap>
        </BrowserRouter>
        {import.meta.env.DEV && <ReactQueryDevtools initialIsOpen={false} />}
      </QueryClientProvider>
    </React.StrictMode>,
  );
});
