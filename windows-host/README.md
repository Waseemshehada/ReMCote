# ReMCote Host (Windows)

Native Windows C++ host for ReMCote — an **attended** ultra-low-latency remote
desktop. Captures the desktop with DXGI Desktop Duplication, encodes with NVENC
H.264, and streams to a browser over WebRTC P2P. Remote mouse/keyboard input is
injected with SendInput. **Every session requires the Host user to click ALLOW.**

## Requirements

- Windows 10 or 11, 64-bit
- NVIDIA GPU with a current driver (NVENC)
- Visual Studio 2022 (any edition) with the **Desktop development with C++** workload
- git

## Build

```powershell
cd windows-host
.\build-windows.ps1
```

The script checks prerequisites (and tells you exactly what is missing),
bootstraps vcpkg, fetches NVENC headers, configures with CMake, and builds
Release x64. The result is:

```
windows-host\dist\ReMCoteHost.exe
```

## Configure the signaling server

The Host has **no built-in server URL**. Point it at your ReMCote server using
either of the following (checked in this order):

1. Environment variable:
   ```powershell
   $env:REMCOTE_SIGNALING_URL = "wss://<your-remcote-server>/api/ws"
   ```
   (`REMCOTE_SERVER` is also accepted.)
2. A file named `remcote-server.txt` next to `ReMCoteHost.exe` containing the
   URL on a single line.

## Run

```powershell
cd windows-host\dist
.\ReMCoteHost.exe
```

- Launched from a terminal, logs print to that terminal.
- Double-clicked, logs are written to `remcote-host.log` next to the exe.

Startup runs a real preflight (Windows version, NVENC, D3D11, DXGI capture,
signaling URL) with PASS/FAIL lines, then registers and shows a 9-digit
Device ID in the Host window. Enter that ID on the ReMCote website from
another computer; the Host window shows an ALLOW / DECLINE prompt.

## First-frame diagnostics

State transitions are logged exactly once per session — use these to locate a
failure point: `[CAPTURE] DXGI initialized`, `[NVENC] initialized`,
`[SIGNALING] Connecting to ...`, `[SIGNAL] registered as ...`,
`[SESSION] Request received`, `[SESSION] Host approved`,
`[WEBRTC] Offer received — creating answer`, `[WEBRTC] Peer connected`,
`[WEBRTC] Video track active`, `[WEBRTC] DataChannel opened: ...`,
`[VIDEO] First frame captured / encoded / sent`.

## Docs

- Build details: [`docs/windows-build.md`](docs/windows-build.md)
- Architecture: [`docs/architecture.md`](docs/architecture.md)
- Wire protocol: [`docs/protocol.md`](docs/protocol.md)
- Performance: [`docs/performance.md`](docs/performance.md)
- Testing: [`docs/testing.md`](docs/testing.md)
