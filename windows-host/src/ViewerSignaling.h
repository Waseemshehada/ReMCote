#pragma once
// ViewerSignaling — native Windows viewer signaling client for ReMCote.
//
// Implements the client (viewer) side of the ReMCote signaling protocol as
// defined in lib/remcote-protocol/src/index.ts and mirrored by the TypeScript
// SignalingClient / RemoteSession pair in artifacts/remcote/src/lib/remote/.
//
// Wire behaviour:
//   1. Connect to ws/wss URL (libdatachannel WebSocket).
//   2. On open: if a session is active, send client-resume-session first;
//      otherwise send client-connect-request.
//   3. Track sessionId / sessionToken from client-session-state.
//   4. Surface state transitions, host capabilities, ICE servers, and peer
//      signals via callbacks — safe to call from any thread.
//   5. Allow the owner to send client-signal, client-connection-state
//      (client-session-established), and client-session-closed.
//   6. Reconnect with exponential back-off (500 ms → 8 s cap); resume session
//      on reconnect using the stored sessionId + sessionToken.
//   7. Bounded outbound queue (512 messages) that drops ordinary ICE candidates
//      before dropping SDP / session-control messages.
//   8. Clean Stop() with no detached lifetime hazards (generation counter).
//
// All callbacks are invoked from libdatachannel's internal network thread.
// The owner MUST NOT call ViewerSignaling methods from inside a callback
// without dispatching to a different thread (deadlock risk from re-entrant
// mutex acquisition).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "CallbackFence.h"
#include "Common.h"

namespace remcote {

// Session state mirror of the protocol (spec §31).
enum class ViewerSessionState {
    Offline,
    Connecting,
    AwaitingApproval,
    Negotiating,
    ConnectedDirect,
    ConnectedRelay,
    Failed,
    Disconnected,
};

// Converts the wire string to the enum; returns Offline on unknown values.
ViewerSessionState ParseViewerSessionState(const std::string& s) noexcept;
// Returns the wire string representation (for logging / sending).
const char* ViewerSessionStateString(ViewerSessionState s) noexcept;

// Host capabilities advertised by the server in client-session-state (spec §44).
struct ViewerHostCapabilities {
    bool h264 = false;
    bool hevc = false;
    bool av1  = false;
    int  maxWidth    = 0;
    int  maxHeight   = 0;
    int  maxFps      = 0;
    int  desktopWidth  = 0;
    int  desktopHeight = 0;
    int  desktopHz     = 0;
    std::string gpuName;
    std::string encoderName;
};

class ViewerSignaling {
public:
    // ── Callbacks (all optional; set before Start()) ─────────────────────────

    // Fired whenever the session state machine transitions.
    using OnStateChanged = std::function<void(ViewerSessionState state,
                                              const std::string& message)>;

    // Fired when a client-peer-signal arrives (SDP answer / ICE candidate).
    // |payload| is the raw JSON object: {kind, sdp/candidate, ...}
    using OnPeerSignal = std::function<void(const nlohmann::json& payload)>;

    // Fired when the first client-session-state message with capabilities
    // arrives (or when they change on resume).
    using OnCapabilities = std::function<void(const ViewerHostCapabilities& caps)>;

    // Fired when NEGOTIATING state delivers ICE servers to use for WebRTC.
    using OnIceServers = std::function<void(const std::vector<IceServerCfg>& servers)>;

    // Fired on a signaling-level error (type:"error" message from server).
    using OnError = std::function<void(const std::string& code,
                                       const std::string& message)>;

    // ── Construction ─────────────────────────────────────────────────────────

    // |serverUrl|  – ws:// or wss:// URL of the signaling endpoint, e.g.
    //                wss://remcote.replit.app/api/ws
    // |deviceId|   – 9-digit public device ID of the host to connect to
    //                (digits only; spaces stripped automatically).
    explicit ViewerSignaling(std::string serverUrl, std::string deviceId);
    ~ViewerSignaling();

    ViewerSignaling(const ViewerSignaling&) = delete;
    ViewerSignaling& operator=(const ViewerSignaling&) = delete;

    // ── Callback registration (call before Start()) ───────────────────────────

    void SetOnStateChanged(OnStateChanged cb)  { onStateChanged_  = std::move(cb); }
    void SetOnPeerSignal  (OnPeerSignal   cb)  { onPeerSignal_    = std::move(cb); }
    void SetOnCapabilities(OnCapabilities cb)  { onCapabilities_  = std::move(cb); }
    void SetOnIceServers  (OnIceServers   cb)  { onIceServers_    = std::move(cb); }
    void SetOnError       (OnError        cb)  { onError_         = std::move(cb); }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    // Open the WebSocket and begin the connect-request / session dance.
    // Safe to call from any thread; must be called exactly once.
    void Start();

    // Close the session and the WebSocket cleanly.  Sends client-session-closed
    // if a session is active.  Blocks until all internal threads/timers quiesce.
    // After Stop() the object must not be reused.
    void Stop();

    // ── Outbound signaling ───────────────────────────────────────────────────

    // Send a WebRTC SDP or ICE payload to the host via the server.
    // |payload| must be a JSON object matching SignalPayload (kind, sdp/candidate…).
    // No-op if no session is active or the socket is closed.
    void SendSignal(const nlohmann::json& payload);

    // Notify the server that the P2P connection is established (stops the
    // negotiation timeout on the server).  |relay| = true for TURN relay.
    void SendConnectionEstablished(bool relay);

    // Explicitly close the session from the viewer side.
    // |reason| is a short human-readable string forwarded to the host.
    void SendSessionClosed(const std::string& reason = "");

    // ── Accessors (thread-safe, snapshot) ───────────────────────────────────

    ViewerSessionState State()   const;
    std::string        SessionId()    const;
    std::string        SessionToken() const;
    bool               IsConnected()  const; // WS open and registered

private:
    // Internal helpers
    void Connect();
    void HandleMessage(const std::string& text);
    void Send(nlohmann::json msg);
    void FlushQueue();

    // Callback helpers — only invoked inside the mutex-free callback path or
    // under careful lock management to avoid re-entrant issues.
    void FireState(ViewerSessionState s, const std::string& msg);
    void FirePeerSignal(const nlohmann::json& payload);
    void FireCapabilities(const ViewerHostCapabilities& caps);
    void FireIceServers(const std::vector<IceServerCfg>& servers);
    void FireError(const std::string& code, const std::string& message);

    // Parse IceServerConfig array from a JSON array.
    static std::vector<IceServerCfg> ParseIceServers(const nlohmann::json& arr);
    // Parse ViewerHostCapabilities from a JSON object.
    static ViewerHostCapabilities ParseCapabilities(const nlohmann::json& obj);
    // Strip non-digits from a device ID string.
    static std::string NormalizeDeviceId(const std::string& raw);

    // Configuration
    std::string serverUrl_;
    std::string deviceId_;   // normalized (digits only)

    // Session state (protected by sessionMutex_)
    mutable std::mutex sessionMutex_;
    std::string sessionId_;
    std::string sessionToken_;
    ViewerSessionState state_{ViewerSessionState::Offline};

    // WebSocket (protected by socketMutex_)
    std::mutex socketMutex_;
    std::shared_ptr<rtc::WebSocket> ws_;
    std::atomic<bool>     wsOpen_{false};
    std::shared_ptr<CallbackFence> callbackFence_{
        std::make_shared<CallbackFence>()};

    // Lifecycle
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> generation_{0}; // incremented on every (re)connect

    // Outbound queue (protected by queueMutex_)
    // Holds messages that couldn't be sent because the socket was not open.
    std::mutex            queueMutex_;
    std::deque<nlohmann::json> pendingQueue_;
    static constexpr size_t kMaxQueueSize = 512;

    // Reconnect back-off (only written from Connect() / onClosed handler)
    std::mutex reconnectMutex_;
    int reconnectDelayMs_{500};  // 500 ms → doubles to 8 s cap

    // Callbacks (set once before Start(), never changed after)
    OnStateChanged  onStateChanged_;
    OnPeerSignal    onPeerSignal_;
    OnCapabilities  onCapabilities_;
    OnIceServers    onIceServers_;
    OnError         onError_;

    // Reconnect timer thread — sleeps and calls Connect() again on close.
    std::thread reconnectThread_;
    std::mutex  reconnectCvMutex_;
    std::condition_variable reconnectCv_;
    bool reconnectPending_{false};
};

} // namespace remcote
