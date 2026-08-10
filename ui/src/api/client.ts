/**
 * API client — fetch wrapper with:
 *  - Bearer JWT from auth store
 *  - 401 → attempt refresh → retry once
 *  - Normalised ApiError on 4xx/5xx
 *  - Type-safe `request<T>()` helper
 */

import type { ApiError } from './types';

export class ApiClientError extends Error {
  constructor(
    public readonly status: number,
    public readonly code: string,
    public readonly detail?: string,
  ) {
    super(detail ?? code);
    this.name = 'ApiClientError';
  }
}

// Injected at runtime to avoid circular import with auth store.
let _getToken: () => string | null = () => null;
let _doRefresh: () => Promise<void> = async () => {};
let _onUnauthorized: () => void = () => {};

export function configureClient(opts: {
  getToken: () => string | null;
  doRefresh: () => Promise<void>;
  onUnauthorized: () => void;
}) {
  _getToken = opts.getToken;
  _doRefresh = opts.doRefresh;
  _onUnauthorized = opts.onUnauthorized;
}

async function parseError(res: Response): Promise<ApiClientError> {
  let body: ApiError = { error: 'unknown_error' };
  try { body = await res.json(); } catch { /* empty */ }
  return new ApiClientError(res.status, body.error ?? 'unknown_error', body.detail);
}

async function fetchOnce(url: string, init: RequestInit): Promise<Response> {
  const token = _getToken();
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...(init.headers as Record<string, string> ?? {}),
  };
  if (token) headers['Authorization'] = `Bearer ${token}`;
  return fetch(url, { ...init, headers });
}

// Like fetchOnce but does NOT inject Content-Type — caller controls it
// (FormData → boundary auto, binary download → no body). Used by raw
// helpers below.
async function fetchOnceRaw(url: string, init: RequestInit): Promise<Response> {
  const token = _getToken();
  const headers: Record<string, string> = {
    ...(init.headers as Record<string, string> ?? {}),
  };
  if (token) headers['Authorization'] = `Bearer ${token}`;
  return fetch(url, { ...init, headers });
}

export async function request<T>(
  method: string,
  path: string,
  body?: unknown,
  opts: { noAuth?: boolean; skipRetry?: boolean } = {},
): Promise<T> {
  const init: RequestInit = {
    method,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  };

  let res = await fetchOnce(path, init);

  // 401 → try refresh once
  if (res.status === 401 && !opts.noAuth && !opts.skipRetry) {
    try {
      await _doRefresh();
      res = await fetchOnce(path, init);
    } catch {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
    if (res.status === 401) {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
  }

  if (!res.ok) throw await parseError(res);

  // 204 No Content
  if (res.status === 204) return undefined as T;

  return res.json() as Promise<T>;
}

// Raw POST для multipart/binary — повторяет refresh/error семантику
// request<T>(), но ничего не сериализует и не добавляет Content-Type
// (для FormData браузер сам выставит boundary).
export async function postForm<T>(path: string, form: FormData): Promise<T> {
  const init: RequestInit = { method: 'POST', body: form };
  let res = await fetchOnceRaw(path, init);
  if (res.status === 401) {
    try {
      await _doRefresh();
      res = await fetchOnceRaw(path, init);
    } catch {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
    if (res.status === 401) {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
  }
  if (!res.ok) throw await parseError(res);
  if (res.status === 204) return undefined as T;
  return res.json() as Promise<T>;
}

// Бинарный GET для download'ов (CA bundle, экспорты). Возвращает blob +
// filename из Content-Disposition (если есть). Авторизация + refresh —
// как в request<T>().
export async function getBlob(
  path: string,
): Promise<{ blob: Blob; filename: string | null; contentType: string | null }> {
  const init: RequestInit = { method: 'GET' };
  let res = await fetchOnceRaw(path, init);
  if (res.status === 401) {
    try {
      await _doRefresh();
      res = await fetchOnceRaw(path, init);
    } catch {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
    if (res.status === 401) {
      _onUnauthorized();
      throw new ApiClientError(401, 'unauthorized');
    }
  }
  if (!res.ok) throw await parseError(res);
  const cd = res.headers.get('Content-Disposition') ?? '';
  const m = /filename="?([^";]+)"?/i.exec(cd);
  return {
    blob: await res.blob(),
    filename: m?.[1] ?? null,
    contentType: res.headers.get('Content-Type'),
  };
}

export const api = {
  get:    <T>(path: string, opts?: Parameters<typeof request>[3]) => request<T>('GET',    path, undefined, opts),
  post:   <T>(path: string, body?: unknown) => request<T>('POST',   path, body),
  put:    <T>(path: string, body?: unknown) => request<T>('PUT',    path, body),
  patch:  <T>(path: string, body?: unknown) => request<T>('PATCH',  path, body),
  delete: <T>(path: string) => request<T>('DELETE', path),
};
