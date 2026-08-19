# ReMCote Windows Host — Pinned Dependency Versions

All build-time dependencies are pinned to specific commits/tags so that the
same source commit always produces the same dependency tree.

## vcpkg (dependency manager)

| Property | Value |
|----------|-------|
| Tag      | `2025.04.09` |
| Commit   | `ce613c41372b23b1f51333815feb3edd87ef8a8b` |
| vcpkg.json `builtin-baseline` | `ce613c41372b23b1f51333815feb3edd87ef8a8b` |

The `builtin-baseline` in `vcpkg.json` locks the registry snapshot used for
dependency resolution to the same commit as the pinned vcpkg clone.

## nv-codec-headers (NVIDIA Video Codec SDK interface headers)

| Property | Value |
|----------|-------|
| Tag      | `n12.2.72.0` |
| Commit   | `c69278340ab1d5559c7d7bf0edf615dc33ddbba7` |
| Source   | https://github.com/FFmpeg/nv-codec-headers |

The NVIDIA driver ships `nvEncodeAPI64.dll`. Only the headers are needed at
compile time; no `.lib` or SDK installer is required.

## libdatachannel (WebRTC implementation)

| Property | Value |
|----------|-------|
| Version  | `0.22.6` |
| Resolved by | vcpkg baseline `ce613c41` |
| vcpkg triplet | `x64-windows-static-md` |

RTP/media support (`RTC_ENABLE_MEDIA`) is ON by default in libdatachannel
0.22.6 and is always built. No vcpkg feature flag is required.

## nlohmann-json (JSON parsing)

Resolved by the same vcpkg baseline. Version tracks whatever 0.22.x-era
vcpkg recommends. No API used that has been unstable across recent versions.

## To upgrade a dependency

1. Update the tag constant in `build-windows.ps1`.
2. If upgrading vcpkg: update `builtin-baseline` in `vcpkg.json` to the new
   vcpkg commit hash.
3. Update this file.
4. Run a local build to confirm no API breakage before pushing.
