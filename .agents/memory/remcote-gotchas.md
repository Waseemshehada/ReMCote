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

## GitHub connector push gotchas (Aug 2026)
- Replit GitHub OAuth token scopes: repo, read:org/user/project, user:email — **no `workflow` scope**, and reauth offers none. Any Git Data tree containing `.github/workflows/*` fails with a misleading **404** on POST /git/trees. Push everything else; the user must add workflow files via the GitHub web UI.
- Connector proxyFetch takes paths only (`/repos/...`), rate-limited ~10 RPS per repl (429 with Retry-After).
- Git Data API returns 409 "Git Repository is empty" on empty repos — seed with one Contents-API PUT first, then force-update the ref with an orphan commit.
- `git ls-files -s` gives index blob shas/modes — blobs already uploaded can be reused without re-upload since shas are content-derived.
