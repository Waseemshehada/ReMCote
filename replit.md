# ReMCote — "Remote. Reimagined."

An attended, ultra-low-latency remote desktop platform. A browser controls a
real Windows desktop over WebRTC P2P; the Windows host captures with DXGI,
encodes with NVENC H.264, and injects input via SendInput. Every session
requires the physical host user to click ALLOW — no unattended access.

## Run & Operate

- `pnpm --filter @workspace/api-server run dev` — API + signaling server
- `pnpm --filter @workspace/remcote run dev` — web viewer (React/Vite)
- `pnpm run typecheck` — full typecheck across all packages
- `pnpm --filter @workspace/api-spec run codegen` — regenerate API hooks/Zod from OpenAPI
- `pnpm --filter @workspace/db run push` — push DB schema (dev only)
- Windows host build (on Windows, not Replit): `windows-host/build-windows.ps1`
- Required env: `DATABASE_URL`. Optional signaling env: `STUN_URL` (defaults to
  Google STUN), `TURN_URL`, `TURN_USERNAME`, `TURN_PASSWORD`.

## Stack

- pnpm workspaces, Node.js 24, TypeScript 5.9
- API: Express 5; signaling: `ws` WebSocket server at `/api/ws`
- Web: React + Vite + wouter + TanStack Query (generated Orval hooks)
- DB: PostgreSQL + Drizzle ORM; Validation: Zod (`zod/v4`)
- Windows host: C++20, CMake, libdatachannel, NVENC, DXGI (see `windows-host/`)

## Where things live

- Shared signaling/input protocol (source of truth): `lib/remcote-protocol/src/index.ts`
- Signaling server (control plane, no media): `artifacts/api-server/src/lib/signaling.ts`
- REST routes (`/healthz`, `/devices/:id`, `/ice-config`, `/stats`): `artifacts/api-server/src/routes/remcote.ts`
- DB schema (`devices`, `sessions`): `lib/db/src/schema/`
- API contract: `lib/api-spec/openapi.yaml` → generates `api-client-react` + `api-zod`
- Web WebRTC core (correctness-critical, hand-written): `artifacts/remcote/src/lib/remote/` (signaling, session, input, stats)
- Web pages: `artifacts/remcote/src/pages/` (home, download, session, not-found)
- Windows host source + docs: `windows-host/src/`, `windows-host/docs/`

## Architecture decisions

- **Browser is the WebRTC offerer**; it creates both data channels + a recvonly
  video transceiver, and the host answers. Simplifies the browser path.
- **Media never touches the server.** Signaling only relays SDP/ICE + approval.
- **Newest-frame-wins everywhere.** Host encoder has a depth-1 queue; pointer
  motion uses an unordered/`maxRetransmits:0` data channel.
- **Device IDs are not secrets.** Random 9-digit IDs; connecting still requires
  the host to click ALLOW. Host keeps its ID via a hashed long-lived secret.
- **Native Windows host cannot be compiled on Replit** — real source + an
  automated `build-windows.ps1` is the deliverable; build on Windows.

## Product

Web viewer: enter a 9-digit Device ID → connect → approve on host → fullscreen
low-latency desktop with client-side cursor overlay, real WebRTC-stats HUD,
input-RTT probe, fit/fill + fullscreen controls. Download page documents the
Windows host build. All performance numbers are measured, never faked.

## User preferences

- Dark, premium, professional aesthetic; no emojis in UI; no gaming/RGB/neon.
- Do not fake streaming, stats, or connected states.

## Gotchas

- Zod v3 in the codegen path lacks `z.int()`: OpenAPI `type: integer` breaks
  codegen — use `type: number` in `lib/api-spec/openapi.yaml`.
- The WS path `/api/ws` must be listed in the api-server artifact's
  `services.paths` or the shared proxy won't forward the upgrade.
- Generated query hooks require an explicit `query.queryKey` (use the generated
  `getXxxQueryKey()`), even when passing only `refetchInterval`/`enabled`.
