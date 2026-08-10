# LiveQX UI

Production-ready React frontend for the LiveQX headless streaming engine.

## Tech stack

| Layer | Library |
|---|---|
| Build | Vite 5 + TypeScript 5 |
| UI | React 18 + Tailwind CSS 3 |
| Routing | react-router-dom v6 |
| Server state | TanStack Query v5 |
| Tables | TanStack Table v8 |
| Client state | Zustand v4 |
| Forms | react-hook-form + zod |
| Icons | lucide-react |
| i18n | i18next + react-i18next |
| Dates | date-fns |
| Mocks | MSW v2 (dev only) |
| Testing | Vitest + Testing Library |

## Quick start (dev)

```bash
cd frontend
npm install
npm run dev
```

Vite dev server starts at **http://localhost:5173**.  
MSW intercepts all `/api/*` calls with mock data — no backend required.

Default login: **admin** / any password.

## Scripts

| Command | Description |
|---|---|
| `npm run dev` | Vite dev server with MSW |
| `npm run build` | Production bundle → `dist/` |
| `npm run preview` | Serve `dist/` locally |
| `npm test` | Vitest smoke tests |
| `npm run lint` | ESLint |
| `npm run gen:api-types` | Regenerate `src/api/types.ts` from `../openapi.yaml` |

## Project structure

```
src/
├── api/
│   ├── client.ts          # fetch wrapper (JWT, 401 refresh, error norm.)
│   ├── types.ts           # TypeScript types (generated from openapi.yaml)
│   └── queries/           # TanStack Query hooks per domain
├── stores/
│   ├── auth.ts            # Zustand: JWT, user, role, channel grants
│   └── ui.ts              # Zustand: sidebar, toasts, LDAP banner, SSE state
├── hooks/
│   ├── useEventStream.ts  # SSE singleton + per-component selector hook
│   ├── useWebRTCPreview.ts# WebRTC preview (offer/answer/cleanup)
│   ├── useChannelAccess.ts# Per-channel RBAC helper
│   └── useToast.ts        # Toast shortcut
├── components/            # Shared primitives (Block, HealthBadge, EmptyState)
├── features/              # Domain pages (auth, channels, settings, …)
├── mocks/                 # MSW browser worker + handlers (DEV only)
├── locales/               # en.json + ru.json
├── schemas/               # Zod form schemas
├── lib/                   # format.ts (fmtTime/fmtDate/fmtBytes/…)
├── styles/                # globals.css + tokens.css
├── routes.tsx             # react-router v6 config + RequireAuth/RequireRole guards
├── App.tsx                # Shell: TopBar + Sidebar + Outlet + ToastRegion
└── main.tsx               # createRoot, providers, MSW init
```

## Connecting to real backend

1. Set `VITE_API_BASE` env var to your LiveQX instance:
   ```
   VITE_API_BASE=http://10.0.0.5:8080 npm run dev
   ```
2. Or build and serve the `dist/` directory from LiveQX:
   ```
   npm run build
   # then pass --ui-dir=dist/ to the liveqx binary
   ```

MSW only runs in dev mode (`import.meta.env.DEV`); production builds make real API calls.

## API type generation

Types in `src/api/types.ts` are derived from `openapi.yaml`:

```bash
npm run gen:api-types
```

This runs `openapi-typescript ../openapi.yaml -o src/api/types.ts`.  
The current `types.ts` is a hand-written equivalent — regenerate to pick up future API changes.

## RBAC

- `<RequireAuth>` — redirects to `/login` if no access token.
- `<RequireRole minRole="admin">` — redirects to `/dashboard` if insufficient role.
- `useChannelAccess(channelId)` — returns `{ canView, canOperate, isAdmin }` for per-channel guards.
- Sidebar items are filtered by role (items not visible to the user are absent from the DOM).

## Real-time (SSE)

`<EventBusProvider>` mounts a single `EventSource` to `/api/events/stream` after login.  
On each event it:
1. Writes to an in-memory ring buffer (256 events).
2. Calls `queryClient.invalidateQueries(...)` for affected domains.
3. Notifies `useEventStream()` subscribers.

On overflow (`event_bus_overflow`): toast warning + full `queryClient.invalidateQueries()`.

## i18n

Default language: English (`en`). Russian (`ru`) available.  
All user-facing strings use `t('namespace.key')`.  
Language can be switched via the Profile page or browser preference detection.

## Open questions / TODOs

See `QUESTIONS.md` for items that need backend clarification before implementation.
