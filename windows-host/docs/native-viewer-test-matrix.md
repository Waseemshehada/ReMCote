# Native viewer test matrix

Run these checks with the same `ReMCoteSetup.exe` installed on two physical
Windows 10/11 computers. The host needs an NVIDIA GPU; the viewer does not.

## Required release checks

1. **Direct connection** — Put both PCs on the same LAN, connect by Device ID,
   approve on the host, verify the status is direct, video appears, and mouse,
   click, wheel, letters, modifiers, arrows, and function keys work.
2. **TURN fallback** — Put the PCs behind different restrictive networks,
   verify the session connects through relay, then repeat the video and input
   checks.
3. **Decline** — Decline the request on the host and confirm the viewer leaves
   the waiting screen with a clear failure message and can retry.
4. **Signaling reconnect** — Interrupt signaling after approval while keeping
   the peer connection alive, restore it within the resume window, and confirm
   the session token resumes without a second approval.
5. **Peer reconnect/failure** — Interrupt the viewer network, confirm both
   programs leave the active state cleanly, then reconnect with a new session.
6. **Video startup** — Confirm the first frame appears after approval without
   waiting for a periodic keyframe; resize and toggle full screen while moving
   the pointer near all four remote-screen corners.
7. **Viewer-only computer** — Run on a PC where DXGI capture is unavailable or
   no NVIDIA GPU is present and confirm Connect to another device still works.
8. **Teardown** — Test viewer Disconnect, host Stop, closing either window, and
   closing the whole app. Input injection and capture must stop immediately.

Record Windows versions, GPU/driver, direct or relay status, resolution, refresh
rate, and logs from both PCs for every failed case.