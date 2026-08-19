# ReMCote Desktop (Windows)

Native Windows C++ remote desktop for both computers. The same installation can
share this PC or connect to another Device ID. Video and input travel peer to
peer over WebRTC; the viewer decodes H.264 with Windows Media Foundation and
presents it with D3D11. **Every session requires the Host user to click ALLOW.**

## Requirements

- Windows 10 or 11, 64-bit
- NVIDIA GPU with a current driver is required only on a PC that shares its screen
- A viewer-only PC needs Windows 10/11 with Media Foundation and D3D11
- Visual Studio 2022 (any edition) with the **Desktop development with C++** workload
- Git and Python 3.11+ (build only)

## Build

```powershell
cd windows-host
.\build-windows.ps1
```

The script checks prerequisites, resolves the pinned Conan 2 dependencies,
fetches NVENC headers, configures CMake, and builds Release x64. The result is:

```
windows-host\dist\ReMCoteHost.exe
```

## Configure the signaling server

The app uses the production ReMCote signaling server by default. Developers can
override it using either of the following (checked in this order):

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

Install and run the same executable on both computers. On the viewer PC click
**CONNECT TO ANOTHER DEVICE**, enter the 9-digit Device ID shown on the host,
then click **ALLOW** on the host. No browser is used.

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
