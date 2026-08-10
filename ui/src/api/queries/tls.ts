import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { api, postForm, getBlob } from '../client';
import type { TlsInfo, TlsRegenerateResponse, TlsImportResponse } from '../types';

// ─── /api/tls/info ───────────────────────────────────────────────────────────
// Admin only. Снимок текущего режима + метаданные server/CA cert.
// `server` и `ca` могут быть пустыми объектами — UI проверяет
// `'subject' in obj` перед чтением остальных полей.
export const useTlsInfo = () =>
  useQuery({
    queryKey: ['tls', 'info'],
    queryFn: () => api.get<TlsInfo>('/api/tls/info'),
    staleTime: 30_000,
  });

// ─── POST /api/tls/regenerate-server ─────────────────────────────────────────
// После 200 listener сам перезапустится; current request успеет получить
// тело ответа до rebind'а (rebind делается уже после res.send в backend'е).
// `san_extra` — опциональные дополнительные SAN'ы поверх auto-detect.
export const useRegenerateServerCert = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (extras: string[] | undefined) =>
      api.post<TlsRegenerateResponse>(
        '/api/tls/regenerate-server',
        extras && extras.length > 0 ? { san_extra: extras } : {},
      ),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tls', 'info'] });
    },
  });
};

// ─── POST /api/tls/import (multipart/form-data) ─────────────────────────────
// Использует postForm() из client.ts: refresh/auth работают так же, как у
// JSON-endpoint'ов, но Content-Type выставляет браузер (multipart с
// boundary).
export const useImportTls = () => {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async (input: { cert: File | string; key: File | string; ca?: File | string }) => {
      const fd = new FormData();
      const append = (k: string, v: File | string) => {
        if (typeof v === 'string') {
          fd.append(k, new Blob([v], { type: 'application/x-pem-file' }), `${k}.pem`);
        } else {
          fd.append(k, v);
        }
      };
      append('cert', input.cert);
      append('key', input.key);
      if (input.ca) append('ca', input.ca);
      return await postForm<TlsImportResponse>('/api/tls/import', fd);
    },
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tls', 'info'] });
    },
  });
};

// ─── GET /api/tls/ca-bundle ─────────────────────────────────────────────────
// Бинарный download (PEM). Не useQuery — это user-action, не cached state.
// Возвращает blob + filename из Content-Disposition, чтобы вызывающий код
// мог сделать <a download>.
export async function downloadCaBundle(): Promise<{ blob: Blob; filename: string }> {
  const r = await getBlob('/api/tls/ca-bundle');
  return { blob: r.blob, filename: r.filename ?? 'liveqx-ca.pem' };
}
