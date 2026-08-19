# ReMCote Signaling & Input Protocol

The wire protocol is defined once in `lib/remcote-protocol/src/index.ts` and
implemented identically by the browser (TypeScript) and the Windows host (C++).
Signaling is JSON over a WebSocket at **`/api/ws`**. Media/input are WebRTC.

## Roles
- **Browser = offerer.** It creates the two data channels and a `recvonly`
  video transceiver, then sends the SDP offer.
- **Host = answerer.** It attaches its NVENC H.264 track (becomes `sendonly`)
  and answers.

## Session lifecycle
```
Host                     Server                    Browser
 │  host-register ─────────►                         │
 │  ◄──── host-registered (publicDeviceId, secret, iceServers)
 │  host-heartbeat ────────► (every 15s)             │
 │                        ◄──── client-connect-request (publicDeviceId)
 │  ◄──── host-connect-request (sessionId)           │
 │                          ────► client-session-state: AWAITING_APPROVAL
 │  [Host clicks ALLOW]                               │
 │  host-connect-response(accept=true) ─►            │
 │                          ────► client-session-state: NEGOTIATING (+iceServers)
 │                        ◄──── client-signal: offer  │
 │  ◄──── host-peer-signal: offer                     │
 │  host-signal: answer ───►                          │
 │                          ────► client-peer-signal: answer
 │  host-signal: candidate ◄──────────────► client-signal: candidate (both ways)
 │            ═══════════ WebRTC P2P established ═══════════
```

## Messages
See `lib/remcote-protocol` for exact TypeScript types. Summary:

**Host → Server:** `host-register`, `host-heartbeat`, `host-connect-response`,
`host-signal`, `host-session-closed`.
**Server → Host:** `host-registered`, `host-connect-request`, `host-peer-signal`,
`host-session-ended`, `error`.
**Browser → Server:** `client-connect-request`, `client-signal`,
`client-session-closed`.
**Server → Browser:** `client-session-state`, `client-peer-signal`, `error`.

`SignalPayload` = `offer | answer | candidate | candidate-end`.

## Device identity
- `publicDeviceId` is a cryptographically random 9-digit number shown as
  `583 491 276`. It is **not** an access secret — a remote user still cannot
  connect without the Host clicking ALLOW.
- `secretToken` is a long-lived secret stored beside the exe
  (`remcote-device.json`) so the machine keeps its Device ID across restarts.
  The server stores only its SHA-256 hash.
- `sessionToken` is short-lived (2-minute TTL for pending sessions), issued to
  the browser, and required on every `client-signal`.

## Data channels
- **`input-pointer`** — `ordered:false`, `maxRetransmits:0`. 9-byte binary:
  `u8 type(=1) | f32 x | f32 y` (little-endian, normalized 0..1).
- **`input-reliable`** — ordered JSON:
  - `{t:"mb", b, d, x, y}` mouse button
  - `{t:"wheel", dx, dy, x, y}`
  - `{t:"kb", code, sc, d}` keyboard (sc = Windows scan code)
  - `{t:"ping", ts}` / `{t:"pong", ts}` input-RTT probe
  - `{t:"cursor", x, y, visible}` host → client cursor metadata
