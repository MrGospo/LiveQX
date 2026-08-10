/**
 * SSE-over-fetch client.
 *
 * Browser EventSource cannot send custom headers, so we cannot pass the
 * Bearer JWT through it. Implementing SSE on top of fetch + ReadableStream
 * lets the bearer token flow through the same auth path as REST — one
 * authentication mechanism for the whole UI, no separate ticket endpoint
 * or cookie-based session.
 *
 * Features:
 *  - Authorization: Bearer <jwt> on every (re)connect
 *  - Last-Event-ID resume on reconnect
 *  - 401 → refresh-token retry (single attempt) before giving up
 *  - Auto-reconnect with fixed back-off
 *  - AbortSignal-based teardown
 *  - Standards-compliant SSE parser (data:/event:/id:/retry:, comments,
 *    \n and \r\n line endings, multi-line data)
 */

export interface SseMessage {
  data:  string;
  event: string;          // empty string when no `event:` field present
  id:    string | null;   // last seen id at the time of this dispatch
}

export interface SseRunOptions {
  url: string;
  signal: AbortSignal;
  getToken: () => string | null;

  /** Called when the HTTP response opens with status 200. */
  onOpen?:    () => void;
  /** Called for each SSE event dispatched. */
  onMessage:  (msg: SseMessage) => void;
  /** Called whenever the connection drops (either error or HTTP non-2xx). */
  onDisconnect?: (reason: string) => void;

  /**
   * Refresh the access token. Called once when the server returns 401.
   * Should mutate the auth store so getToken() returns a new token on the
   * next iteration. If this throws or the retry still 401s, the loop
   * surfaces the failure via onAuthFailed and exits.
   */
  refreshToken?: () => Promise<void>;
  /**
   * Called when authentication is permanently broken (refresh failed or
   * second 401 after refresh). The caller is expected to logout.
   */
  onAuthFailed?: () => void;

  /** Reconnect backoff in ms (default 3000). */
  reconnectDelayMs?: number;
}

/**
 * Run the SSE loop until `signal` aborts. Resolves cleanly on abort.
 * Never rejects — all errors are reported via onDisconnect / onAuthFailed.
 */
export async function runSseLoop(opts: SseRunOptions): Promise<void> {
  const reconnectDelayMs = opts.reconnectDelayMs ?? 3000;
  let lastEventId: string | null = null;

  while (!opts.signal.aborted) {
    const token = opts.getToken();
    if (!token) {
      opts.onAuthFailed?.();
      return;
    }

    const headers: Record<string, string> = {
      Accept:        'text/event-stream',
      Authorization: `Bearer ${token}`,
    };
    if (lastEventId) headers['Last-Event-ID'] = lastEventId;

    let res: Response;
    try {
      res = await fetch(opts.url, {
        method: 'GET',
        headers,
        signal: opts.signal,
        cache:  'no-store',
      });
    } catch {
      if (opts.signal.aborted) return;
      opts.onDisconnect?.('network');
      await sleepWithAbort(reconnectDelayMs, opts.signal);
      continue;
    }

    // Auth failure: try refresh once, then give up.
    if (res.status === 401) {
      if (opts.refreshToken) {
        try {
          await opts.refreshToken();
          continue;                   // retry immediately with new token
        } catch {
          opts.onAuthFailed?.();
          return;
        }
      }
      opts.onAuthFailed?.();
      return;
    }

    if (!res.ok || !res.body) {
      opts.onDisconnect?.(`http_${res.status}`);
      await sleepWithAbort(reconnectDelayMs, opts.signal);
      continue;
    }

    opts.onOpen?.();

    try {
      const finalId = await readSseStream(res.body, lastEventId, opts.onMessage);
      lastEventId = finalId;
      opts.onDisconnect?.('eof');
    } catch (e) {
      if (opts.signal.aborted) return;
      opts.onDisconnect?.('stream_error');
    }

    if (opts.signal.aborted) return;
    await sleepWithAbort(reconnectDelayMs, opts.signal);
  }
}

/**
 * Parse an SSE stream and dispatch events through `onMessage`. Returns
 * the last seen event id so the caller can use it for Last-Event-ID on
 * reconnect.
 *
 * Implements the dispatch rules from the WHATWG HTML/SSE spec:
 *   - lines split on \n; trailing \r is stripped
 *   - lines beginning with ':' are comments
 *   - field/value separated by ':'; one optional leading space stripped
 *   - empty line dispatches the buffered event (data lines joined by \n)
 *   - id: updates the connection's "last event id" (sticky)
 */
async function readSseStream(
  body: ReadableStream<Uint8Array>,
  initialLastId: string | null,
  onMessage: (msg: SseMessage) => void,
): Promise<string | null> {
  const reader  = body.getReader();
  const decoder = new TextDecoder('utf-8');

  let buffer        = '';
  let dataLines:    string[] = [];
  let pendingEvent  = '';
  let lastEventId   = initialLastId;

  const dispatch = () => {
    if (dataLines.length === 0) {
      pendingEvent = '';
      return;
    }
    onMessage({
      data:  dataLines.join('\n'),
      event: pendingEvent,
      id:    lastEventId,
    });
    dataLines = [];
    pendingEvent = '';
  };

  for (;;) {
    const { value, done } = await reader.read();
    if (done) {
      // Process any remaining text as if a final newline arrived. SSE
      // events without a trailing blank line are technically malformed,
      // but we tolerate them on EOF for robustness.
      buffer += decoder.decode();
      if (buffer.length > 0) {
        for (const line of splitLines(buffer)) {
          parseLine(line);
        }
        buffer = '';
        dispatch();
      }
      break;
    }
    buffer += decoder.decode(value, { stream: true });

    let nl: number;
    while ((nl = buffer.indexOf('\n')) >= 0) {
      let line = buffer.slice(0, nl);
      buffer = buffer.slice(nl + 1);
      if (line.endsWith('\r')) line = line.slice(0, -1);
      parseLine(line);
    }
  }

  return lastEventId;

  function parseLine(line: string) {
    if (line === '') { dispatch(); return; }
    if (line.startsWith(':')) return;     // comment

    const colon = line.indexOf(':');
    const field = colon === -1 ? line : line.slice(0, colon);
    let   value = colon === -1 ? '' : line.slice(colon + 1);
    if (value.startsWith(' ')) value = value.slice(1);

    switch (field) {
      case 'data':  dataLines.push(value); break;
      case 'event': pendingEvent = value;  break;
      case 'id':
        // Per spec: ignore ids containing NUL.
        if (!value.includes('\0')) lastEventId = value;
        break;
      case 'retry': /* server-suggested reconnect — we use a fixed backoff */ break;
    }
  }
}

function splitLines(s: string): string[] {
  return s.split(/\r\n|\n|\r/);
}

function sleepWithAbort(ms: number, signal: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    if (signal.aborted) { resolve(); return; }
    const t = setTimeout(() => {
      signal.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    const onAbort = () => { clearTimeout(t); resolve(); };
    signal.addEventListener('abort', onAbort, { once: true });
  });
}
