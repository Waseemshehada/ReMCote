# Building ReMCote Desktop on Windows

ReMCote Desktop is native Windows C++ and **must be built on Windows** — it cannot be
compiled in the Replit Linux environment. The source and an automated build
script are provided; run the script on a Windows machine.

## Requirements
- Windows 10 or 11, x64
- Visual Studio 2022 (or Build Tools) with **Desktop development with C++**
  (MSVC v143 + Windows 10/11 SDK)
- CMake 3.24+
- Git
- Python 3.11+ (for Conan 2)
- **NVIDIA GPU with a current driver** is required only to host (NVENC).
  `nvEncodeAPI64.dll` ships with
  the driver and is loaded at runtime — no separate SDK install is required for
  the encoder library itself; only the interface headers are fetched.

## One-command build
```powershell
cd windows-host
.\build-windows.ps1            # Release  -> dist\ReMCoteHost.exe
.\build-windows.ps1 -Debug     # Debug
```
The script:
1. Installs **Conan 2** and resolves `libdatachannel` and `nlohmann-json`
   from `conanfile.txt`.
2. Fetches **nv-codec-headers** (NVENC interface) into `third_party/`.
3. Configures with the VS 2022 generator + Conan toolchain.
4. Builds and copies the exe to `dist\ReMCoteHost.exe`.

## Running
```powershell
$env:REMCOTE_SIGNALING_URL = "wss://<your-app>.replit.app/api/ws"
.\dist\ReMCoteHost.exe
```
The production server URL is built in. `REMCOTE_SIGNALING_URL`, the legacy
`REMCOTE_SERVER`, or `remcote-server.txt` can override it for developers.

The window shows the Device ID and a **CONNECT TO ANOTHER DEVICE** button.
Install the same executable on both PCs, enter the host's Device ID on the
viewer PC, then click **ALLOW** on the host.

## Dependencies summary
| Dependency | Source | Purpose |
|------------|--------|---------|
| libdatachannel | ConanCenter | WebRTC peer, DTLS/SRTP, H264 RTP packetization |
| nlohmann-json | ConanCenter | Signaling / input JSON |
| nv-codec-headers | git (FFmpeg) | NVENC API headers |
| Direct3D 11 / DXGI | Windows SDK | Desktop Duplication capture |
| Media Foundation | Windows SDK/runtime | Native H.264 decoding |
| user32 (SendInput) | Windows SDK | Input injection |

## Troubleshooting
- **`nvEncodeAPI64.dll not found`** — update the NVIDIA driver; the machine needs
  an NVIDIA GPU.
- **`DuplicateOutput failed`** — another Desktop Duplication client may hold the
  output, or you are on a headless/RDP session. Run on the physical console.
- **Conan has no compatible binary** — confirm MSVC 2022 x64 Release and the
  versions in `DEPENDENCIES.lock.md`; CI intentionally uses prebuilt packages.
