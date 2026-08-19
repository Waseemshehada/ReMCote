# ReMCote Host — Architecture

ReMCote is an **attended** remote desktop system. A session only becomes
interactive after the person physically at the Host computer clicks **ALLOW**.
Media and input travel peer-to-peer over WebRTC; the ReMCote server is a
control plane only and never sees a single video frame.

```
                      ReMCote Server (Node/Express)
                         Signaling — /api/ws
                                 │
                    ┌────────────┴────────────┐
             host-register / SDP / ICE   client-connect / SDP / ICE
                    │                          │
             Windows Host  ◄──── WebRTC P2P ────►  Browser (React)
              (this tree)      video + input          viewer
```

## Data flow

**Video (Host → Browser)**
```
DXGI Desktop Duplication (GPU texture, BGRA)
  → CopyResource into a single-slot GPU texture (depth-1 "queue")
  → NVENC H.264 (ultra-low-latency, CBR, no B-frames, infinite GOP)
  → libdatachannel H264 RTP packetizer
  → WebRTC media track → browser <video>
```
No frame is ever copied to system RAM, and at most one frame is in flight —
if the encoder is busy the capture callback drops the frame (newest wins).

**Input (Browser → Host)**
```
Browser pointer move  → input-pointer  (unordered, maxRetransmits 0, 9-byte binary)
Browser click/key/wheel → input-reliable (ordered JSON)
  → WebRtcTransport decodes → InputEngine queue → SendInput
```
The input injection thread runs at `THREAD_PRIORITY_TIME_CRITICAL` and never
blocks on a video encode (spec §19).

## Module map (`windows-host/src/`)

| Module | Responsibility |
|--------|----------------|
| `main.cpp` | Wiring, session gating behind approval, lifecycle |
| `HostUI` | Visible Win32 window: Device ID, presence, GPU/encoder, ALLOW/DECLINE, STOP |
| `DeviceRegistration` | Signaling WebSocket client: register, heartbeat, SDP/ICE relay |
| `CaptureEngine` | DXGI Desktop Duplication, GPU textures, cursor metadata |
| `EncoderEngine` | NVENC H.264 low-latency encode, depth-1 queue, live bitrate reconfigure |
| `WebRtcTransport` | libdatachannel peer (answerer), video track, input data channels |
| `InputEngine` | SendInput injection on a dedicated time-critical thread |
| `PerformanceMonitor` | Real capture/encode timing and FPS counters |

## Threads (spec §17)
- **Capture thread** — `AcquireNextFrame` loop, submits to encoder synchronously.
- **Encoder** — runs inside the capture callback but with a depth-1 in-flight guard.
- **WebRTC/network** — libdatachannel internal threads.
- **Input** — dedicated, time-critical, independent of video.
- **UI** — Win32 message loop, telemetry once per second.

## Consent boundary
Capture, encode, and input injection do **not** start until `ALLOW` is clicked
(`HostApp::StartPipeline`). `STOP REMOTE SESSION`, closing the window, or a peer
disconnect all call `StopPipeline`, which stops capture, disables input, tears
down the peer connection, and returns the Host to *Ready*.
