# ReMCote — Testing the Real Pipeline

Success is **not** "the frontend loads" or "browser-to-browser WebRTC works."
The milestone is a real Windows desktop streamed to a browser with responsive
input, gated by explicit Host approval.

## Two-machine end-to-end test
**PC A (Host)**
1. Build and run `ReMCoteHost.exe` (see windows-build.md) with `REMCOTE_SIGNALING_URL`
   pointing at your deployed ReMCote.
2. Confirm the window shows a Device ID, `● ONLINE`, the GPU name, and
   `NVENC Ready`.

**PC B (Client)**
3. Open the ReMCote website.
4. Enter PC A's Device ID and click **Connect**.

**PC A**
5. A visible *Incoming ReMCote Connection* prompt appears. Click **ALLOW**.
   (Verify **DECLINE** cleanly refuses and returns to Ready.)

**PC B**
6. The real PC A desktop appears. Now verify:
   - mouse movement + local cursor overlay tracking
   - left / right / middle click
   - typing (including modifiers)
   - scrolling
   - dragging windows
   - fullscreen toggle, fit/fill toggle
   - playing 60 FPS content
   - Premiere timeline scrubbing, Space, J/K/L (if available)

## Performance HUD checks
Open the HUD on the session page. Every value must be **real**:
- Resolution, FPS, codec (H.264), bitrate, RTT, jitter, packet loss,
  frames received/dropped, DIRECT vs RELAY.
- Input RTT (from the `ping`/`pong` probe) updates continuously.

## Safety checks (attended consent)
- Nothing streams before ALLOW.
- **STOP REMOTE SESSION** on the Host immediately kills video + input and
  returns to Ready.
- Closing `ReMCoteHost.exe` terminates access immediately.
- Killing the Host process mid-session: the browser shows a real
  `DISCONNECTED`/`FAILED` state, never a fake connected view.

## Control-plane checks (works today, without the Host)
- `GET /api/healthz` → `{ "status": "ok" }`
- `GET /api/stats` → live device/session counts
- `GET /api/ice-config` → STUN/TURN from environment
- `GET /api/devices/{id}` → 404 for unknown, presence for a registered host

## Connection quality matrix
Test same-LAN (expect DIRECT), across NAT (DIRECT via STUN where possible),
and symmetric NAT with TURN configured (expect RELAY). The HUD badge must match
the actual selected candidate pair.
