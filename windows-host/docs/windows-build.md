# Building ReMCote Host on Windows

The Host is native Windows C++ and **must be built on Windows** — it cannot be
compiled in the Replit Linux environment. The source and an automated build
script are provided; run the script on a Windows machine.

## Requirements
- Windows 10 or 11, x64
- Visual Studio 2022 (or Build Tools) with **Desktop development with C++**
  (MSVC v143 + Windows 10/11 SDK)
- CMake 3.24+
- Git
- **NVIDIA GPU with a current driver** (NVENC). `nvEncodeAPI64.dll` ships with
  the driver and is loaded at runtime — no separate SDK install is required for
  the encoder library itself; only the interface headers are fetched.

## One-command build
```powershell
cd windows-host
.\build-windows.ps1            # Release  -> dist\ReMCoteHost.exe
.\build-windows.ps1 -Debug     # Debug
```
The script:
1. Bootstraps **vcpkg** into `third_party/vcpkg` and installs
   `libdatachannel` and `nlohmann-json` (manifest mode via `vcpkg.json`).
2. Fetches **nv-codec-headers** (NVENC interface) into `third_party/`.
3. Configures with the VS 2022 generator + vcpkg toolchain.
4. Builds and copies the exe to `dist\ReMCoteHost.exe`.

## Running
```powershell
$env:REMCOTE_SIGNALING_URL = "wss://<your-app>.replit.app/api/ws"
.\dist\ReMCoteHost.exe
```
There is no default server URL. If neither `REMCOTE_SIGNALING_URL` (or the
legacy `REMCOTE_SERVER`) nor a `remcote-server.txt` file next to the exe
provides one, the preflight fails with instructions. Launched from a terminal,
logs print there; double-clicked, they go to `remcote-host.log` next to the exe.

The window shows the Device ID (e.g. `583 491 276`), `● ONLINE`, GPU, and
`NVENC Ready`. Enter that Device ID on the ReMCote website, then click **ALLOW**
on the Host when the request appears.

## Dependencies summary
| Dependency | Source | Purpose |
|------------|--------|---------|
| libdatachannel | vcpkg | WebRTC peer, DTLS/SRTP, H264 RTP packetization |
| nlohmann-json | vcpkg | Signaling / input JSON |
| nv-codec-headers | git (FFmpeg) | NVENC API headers |
| Direct3D 11 / DXGI | Windows SDK | Desktop Duplication capture |
| user32 (SendInput) | Windows SDK | Input injection |

## Troubleshooting
- **`nvEncodeAPI64.dll not found`** — update the NVIDIA driver; the machine needs
  an NVIDIA GPU.
- **`DuplicateOutput failed`** — another Desktop Duplication client may hold the
  output, or you are on a headless/RDP session. Run on the physical console.
- **vcpkg build slow the first time** — dependencies compile once and cache.
