# ReMCote Host — Performance Design

The whole pipeline is built around one rule: **the newest frame wins**. This is
an interactive desktop, not offline video — a fresh frame late by 0 ms always
beats a perfect frame late by 40 ms.

## Latency budget targets (1080p60, direct P2P, same LAN)
| Stage | Target |
|-------|--------|
| Capture (`AcquireNextFrame` + CopyResource) | < 2 ms |
| NVENC encode | 2–5 ms |
| Packetize + network (LAN) | 1–5 ms |
| Native receive + Media Foundation decode | 5–15 ms |
| Input inject (SendInput) | < 1 ms |

## Techniques
- **GPU-only frame path.** Desktop textures are copied GPU→GPU into the NVENC
  input texture. No `Map`/CPU readback.
- **Depth-1 queue.** `EncoderEngine::SubmitFrame` uses an atomic `busy_` flag;
  if an encode is in flight the incoming frame is dropped and counted as a
  dropped capture frame. No buffer ever grows.
- **NVENC low-latency config.** Preset P1 + `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`,
  CBR, `frameIntervalP = 1` (no B-frames), infinite GOP with on-demand IDR,
  VBV sized to ~1 frame, `repeatSPSPPS` for fast resync.
- **Independent input thread.** `THREAD_PRIORITY_TIME_CRITICAL`; pointer moves
  collapse to the newest value, discrete events are never dropped.
- **Adaptive bitrate.** `EncoderEngine::SetBitrate` reconfigures NVENC live
  (no session restart) when the client requests a change via `input-reliable`.

## Measured telemetry (never faked — spec §35)
`PerformanceMonitor` reports capture ms, encode ms, capture FPS, encode FPS, and
dropped frames from real counters. The native viewer derives its connection
type from the selected ICE candidate pair and reports input RTT from the
reliable data-channel ping/pong probe.

## Suggested starting bitrates
| Mode | Range |
|------|-------|
| 1080p60 | 10–50 Mbps |
| 1440p60 | 20–80 Mbps |

Treat these as tuning starting points. Adapt gradually and avoid oscillation.

## Next steps (see future-performance.md)
Multi-slice NVENC, HEVC/AV1, frame pacing to display refresh, per-frame
capture→display trace, and TURN-aware bitrate caps.
