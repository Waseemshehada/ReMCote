---
name: ReMCote / pnpm-monorepo gotchas
description: Non-obvious traps hit building the ReMCote WebRTC remote-desktop app in this monorepo template.
---

## OpenAPI `type: integer` breaks Orval+Zod codegen
The api-spec codegen path uses a Zod version without `z.int()`. An OpenAPI
schema with `type: integer` makes codegen emit `z.int()` and fail.
**How to apply:** use `type: number` in `lib/api-spec/openapi.yaml` for integer
fields; re-run `pnpm --filter @workspace/api-spec run codegen`.

## WebSocket paths must be allow-listed in the artifact toml
The shared preview proxy only forwards HTTP/WS paths listed under a service's
`paths` in `.replit-artifact/artifact.toml`. A WS endpoint at `/api/ws` returns
connection-refused until `/api/ws` is added to the api-server service `paths`.
**Why:** the proxy is path-routed; unlisted paths never reach the server.
**How to apply:** edit a copy of artifact.toml, then `verifyAndReplaceArtifactToml`
(never hand-edit `.replit`). Attach the `ws` upgrade handler on the same
`http.Server` Express listens on.

## Generated TanStack Query hooks need an explicit queryKey
Orval-generated `useX(..., { query: {...} })` hooks type `query` as requiring
`queryKey`. Passing only `refetchInterval`/`enabled` fails typecheck.
**How to apply:** always pass `queryKey: getXQueryKey(args)` from the generated
helper alongside other query options.

## curl the dev domain over https
`$REPLIT_DEV_DOMAIN` has no scheme; `curl $REPLIT_DEV_DOMAIN/...` fails (exit 7).
Use `https://$REPLIT_DEV_DOMAIN/...`, and `wss://` for websockets. The `ws`
package isn't resolvable from the repo root — run node test scripts from a
package dir that depends on it (e.g. `artifacts/api-server`).
