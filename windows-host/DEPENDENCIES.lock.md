# ReMCote Windows Host — pinned dependencies

The CI build (`.github/workflows/build-windows-host.yml`) uses **Conan 2**
with prebuilt ConanCenter binaries. vcpkg is no longer used in CI.

| Dependency | Version | Source |
|---|---|---|
| libdatachannel | **0.24.0** | ConanCenter (prebuilt Windows x64 MSVC static, media enabled) |
| nlohmann_json | 3.11.3 | ConanCenter (header-only) |
| nv-codec-headers | tag `n12.2.72.0` | git clone in CI (headers only) |
| Runner | `windows-2022` | pinned — VS2022 / MSVC 194 |
| Conan | `>=2.4,<3` | pip |

Conan profile settings that must match the prebuilt binaries:
- `build_type=Release`, `arch=x86_64`, `compiler=msvc`, `compiler.version=194`
- `compiler.cppstd=17` (dep package ID; our own code compiles as C++20)
- `compiler.runtime=dynamic` (/MD) — the installer ships `vc_redist.x64.exe`

Media support: the ConanCenter libdatachannel package is built with media
enabled (it depends on libsrtp) and propagates `RTC_ENABLE_MEDIA=1` to
consumers via `package_info`. Do NOT define it manually.

Upgrading: change the version in `conanfile.txt`, verify a prebuilt Windows
x64 binary exists on ConanCenter for the profile above, re-audit the
libdatachannel API used in `src/WebRtcTransport.cpp`, and update this file.
