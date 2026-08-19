#pragma once
// ViewerTransport — native Windows viewer WebRTC offerer.
//
// Role inversion vs. WebRtcTransport (the host answerer):
//   • This class is the OFFERER: it creates the PeerConnection, adds the two
//     input data channels and a recvonly H.264 video transceiver, then
//     explicitly calls setLocalDescription() to generate the SDP offer.
//   • The host (WebRtcTransport) is the answerer.
//
// The class is intentionally self-contained — no browser/WebView2 dependency.
// It implements the viewer signaling and input behaviour directly in C++.
//
// Threading model:
//   Callbacks arrive on libdatachannel's internal network thread.
//   All public send methods (SendPointerMove, etc.) are thread-safe.
//   The owner must NOT call ViewerTransport methods from inside a callback
//   without dispatching to a different thread (re-entrant mutex risk).
//
// Wire formats (spec — lib/remcote-protocol/src/index.ts):
//   input-pointer  binary 9 B: u8 type(=1) | f32 x LE | f32 y LE
//   input-reliable JSON: {t:"mb"…}, {t:"wheel"…}, {t:"kb"…}, {t:"ping"…}
//   H.264 Annex-B frames delivered via OnVideoFrame callback.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "CallbackFence.h"
#include "Common.h"

namespace remcote {

// A reassembled, complete H.264 Annex-B access unit delivered to the viewer.
struct ViewerFrame {
    std::vector<uint8_t> data; // Complete Annex-B NAL unit set (one access unit)
    uint32_t rtpTimestamp;     // 90 kHz RTP timestamp
    bool keyframe;             // true when the AU contains an IDR NAL
};

// Reported connection type once ICE completes.
enum class ViewerConnectionType { Unknown, Direct, Relay };

class ViewerTransport {
public:
    // ── Callbacks (set before Open(); all optional) ─────────────────────────

    // Local SDP offer ready — forward to the host via signaling.
    // |payload| matches SignalPayload: {kind:"offer", sdp:"..."}
    using OnLocalSdp = std::function<void(const nlohmann::json& payload)>;

    // Local ICE candidate ready — forward to the host via signaling.
    // |payload| matches SignalPayload: {kind:"candidate", candidate:"...", sdpMid:"..."}
    using OnLocalCandidate = std::function<void(const nlohmann::json& payload)>;

    // A complete H.264 Annex-B access unit has been depacketized and is ready.
    using OnVideoFrame = std::function<void(const ViewerFrame& frame)>;

    // WebRTC peer-connection state changed.
    // |connected| = true on first Connected; false on Disconnected/Failed/Closed.
    using OnConnected = std::function<void(bool connected, ViewerConnectionType type)>;

    // Cursor metadata received from the host on input-reliable.
    // |x|, |y| are normalized 0..1; |visible| indicates cursor visibility.
    using OnCursorUpdate = std::function<void(float x, float y, bool visible)>;

    // Host pong received for the input-RTT probe (spec §33).
    using OnPong = std::function<void(double ts)>;

    // A non-fatal warning or error message for diagnostic display.
    using OnDiagnostic = std::function<void(const std::string& message)>;

    // ── Construction ─────────────────────────────────────────────────────────

    // |iceServers| — ICE/TURN configuration from the server's host-registered
    // or client-session-state NEGOTIATING message.
    explicit ViewerTransport(std::vector<IceServerCfg> iceServers);
    ~ViewerTransport();

    ViewerTransport(const ViewerTransport&) = delete;
    ViewerTransport& operator=(const ViewerTransport&) = delete;

    // ── Callback registration ─────────────────────────────────────────────────

    void SetOnLocalSdp      (OnLocalSdp       cb) { onLocalSdp_       = std::move(cb); }
    void SetOnLocalCandidate(OnLocalCandidate  cb) { onLocalCandidate_ = std::move(cb); }
    void SetOnVideoFrame    (OnVideoFrame      cb) { onVideoFrame_      = std::move(cb); }
    void SetOnConnected     (OnConnected       cb) { onConnected_       = std::move(cb); }
    void SetOnCursorUpdate  (OnCursorUpdate    cb) { onCursorUpdate_    = std::move(cb); }
    void SetOnPong          (OnPong            cb) { onPong_            = std::move(cb); }
    void SetOnDiagnostic    (OnDiagnostic      cb) { onDiagnostic_      = std::move(cb); }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    // Build the PeerConnection, add tracks/channels, and generate the SDP offer.
    // OnLocalSdp fires synchronously (before Open() returns) or asynchronously
    // depending on libdatachannel's internal scheduling.
    // Must be called exactly once, before any signaling payload arrives.
    void Open();

    // Close the peer connection cleanly.  Safe to call multiple times.
    void Close();

    // ── Inbound signaling (from the host, relayed by server) ─────────────────

    // Accept the host's SDP answer.
    // |payload| must have {kind:"answer", sdp:"..."}.
    void HandleAnswer(const nlohmann::json& payload);

    // Add a remote ICE candidate from the host.
    // |payload| must have {kind:"candidate", candidate:"...", sdpMid:"..."}.
    void HandleRemoteCandidate(const nlohmann::json& payload);

    // ── Input sending ─────────────────────────────────────────────────────────

    // Send a pointer-move event over input-pointer (binary, unreliable).
    // |x|, |y| are normalized 0..1 relative to the remote desktop.
    void SendPointerMove(float x, float y);

    // Send a mouse-button event over input-reliable (JSON).
    // |button| 0=left 1=middle 2=right 3=back 4=forward.
    void SendMouseButton(int button, bool down, float x, float y);

    // Send a wheel event over input-reliable (JSON).
    // |dx|, |dy| are wheel deltas in viewer pixels.
    void SendWheel(float dx, float dy, float x, float y);

    // Send a keyboard event over input-reliable (JSON).
    // |code|     = KeyboardEvent.code string (e.g. "KeyA").
    // |scanCode| = Windows PS/2 scan code (0 if unknown; see remcote-protocol).
    //              Values ≥ 0xE000 indicate extended keys (0xE0 prefix).
    // |down|     = true for key-down, false for key-up.
    void SendKey(const std::string& code, uint32_t scanCode, bool down);

    // Send an RTT probe to the host; host echoes back a pong immediately.
    void SendPing(double ts);

    // Request a keyframe from the host by sending {t:"keyframe"} on reliable.
    // Best-effort — only effective if the host supports the message.
    void SendKeyframeRequest();

    // ── Accessors ─────────────────────────────────────────────────────────────

    bool IsOpen() const; // true while the PeerConnection exists and is not closed
    ViewerConnectionType ConnectionType() const;

private:
    // Internal helpers
    void HandleReliableText(const std::string& text);
    void HandlePointerChannelOpen();

    // Detect relay vs direct from the selected candidate pair description.
    // libdatachannel exposes the selected-pair info via
    // PeerConnection::getSelectedCandidatePair() (libdatachannel ≥ 0.18).
    // If unavailable, returns Unknown.
    static ViewerConnectionType DetectConnectionType(
        const std::shared_ptr<rtc::PeerConnection>& pc);

    // Configuration
    std::vector<IceServerCfg> iceServers_;

    // WebRTC objects
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track>          videoTrack_;
    std::shared_ptr<rtc::DataChannel>    pointerCh_;
    std::shared_ptr<rtc::DataChannel>    reliableCh_;

    // State
    mutable std::mutex            mutex_;
    std::atomic<bool>             open_{false};
    std::atomic<ViewerConnectionType> connectionType_{ViewerConnectionType::Unknown};
    std::shared_ptr<CallbackFence> callbackFence_{
        std::make_shared<CallbackFence>()};

    // Callbacks
    OnLocalSdp       onLocalSdp_;
    OnLocalCandidate onLocalCandidate_;
    OnVideoFrame     onVideoFrame_;
    OnConnected      onConnected_;
    OnCursorUpdate   onCursorUpdate_;
    OnPong           onPong_;
    OnDiagnostic     onDiagnostic_;
};

} // namespace remcote
