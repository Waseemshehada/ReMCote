# ReMCote — Future Performance Work

The MVP proves the transport at 1080p60 / 1440p60 with NVENC H.264 over WebRTC.
None of the items below are required for the MVP, and the architecture is built
so they can be added without a rewrite.

## Resolution & refresh
- 4K60 / 4K120: NVENC handles it; the limiters are bitrate, network, and the
  browser decoder. Add multi-slice encoding to parallelize decode.
- High-refresh pacing: pace encode submission to the source display's refresh
  and to client `framesDecoded` feedback to avoid over-producing.

## Codecs (encoder abstraction already in place)
- **HEVC** — big bitrate savings at 4K; gate on browser support detection.
- **AV1** — RTX 40-series NVENC AV1; best efficiency, narrower browser support.
- Negotiate codec from `HostCapabilities` + browser `RTCRtpReceiver.getCapabilities`.

## Latency
- Per-frame trace: stamp capture→encode→send→decode→present and surface a real
  glass-to-glass estimate (current HUD input-RTT is only a component).
- Trigger IDR precisely on client PLI/NACK instead of periodic keyframes
  (already on-demand; wire the client feedback path fully).
- Consider `playoutDelayHint = 0` on the browser receiver.

## Adaptive bitrate
- Closed loop from WebRTC `availableOutgoingBitrate`, RTT, and loss into
  `EncoderEngine::SetBitrate`, with hysteresis to prevent oscillation.
- TURN-aware ceiling: cap bitrate when the selected pair is `relay`.

## Capture
- Multi-monitor selection and hot-switching outputs.
- Dirty-rect / move-rect awareness from DXGI to skip static regions.
- WGC (Windows.Graphics.Capture) as an alternative on systems where DXGI
  duplication is restricted.

## Audio
- WASAPI loopback capture behind the existing (currently disabled)
  `AudioCaptureEngine` interface, sent as an Opus WebRTC track.

## Robustness
- Encoder recreation on device-lost without dropping the peer connection.
- SVC / temporal layers for graceful degradation under loss.
