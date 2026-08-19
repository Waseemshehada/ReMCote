#include "ViewerSignaling.h"
#include "Logger.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <sstream>
#include <stdexcept>

using nlohmann::json;
using namespace std::chrono_literals;

namespace remcote {

// ---------------------------------------------------------------------------
// Helpers — protocol enum ↔ string
// ---------------------------------------------------------------------------

ViewerSessionState ParseViewerSessionState(const std::string& s) noexcept {
    if (s == "CONNECTING")        return ViewerSessionState::Connecting;
    if (s == "AWAITING_APPROVAL") return ViewerSessionState::AwaitingApproval;
    if (s == "NEGOTIATING")       return ViewerSessionState::Negotiating;
    if (s == "CONNECTED_DIRECT")  return ViewerSessionState::ConnectedDirect;
    if (s == "CONNECTED_RELAY")   return ViewerSessionState::ConnectedRelay;
    if (s == "FAILED")            return ViewerSessionState::Failed;
    if (s == "DISCONNECTED")      return ViewerSessionState::Disconnected;
    return ViewerSessionState::Offline;
}

const char* ViewerSessionStateString(ViewerSessionState s) noexcept {
    switch (s) {
        case ViewerSessionState::Offline:          return "OFFLINE";
        case ViewerSessionState::Connecting:       return "CONNECTING";
        case ViewerSessionState::AwaitingApproval: return "AWAITING_APPROVAL";
        case ViewerSessionState::Negotiating:      return "NEGOTIATING";
        case ViewerSessionState::ConnectedDirect:  return "CONNECTED_DIRECT";
        case ViewerSessionState::ConnectedRelay:   return "CONNECTED_RELAY";
        case ViewerSessionState::Failed:           return "FAILED";
        case ViewerSessionState::Disconnected:     return "DISCONNECTED";
    }
    return "OFFLINE";
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ViewerSignaling::ViewerSignaling(std::string serverUrl, std::string deviceId)
    : serverUrl_(std::move(serverUrl))
    , deviceId_(NormalizeDeviceId(deviceId))
{}

ViewerSignaling::~ViewerSignaling() {
    Stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ViewerSignaling::Start() {
    if (running_.exchange(true)) return; // idempotent

    // Start the reconnect-timer thread.  It wakes up whenever reconnectCv_
    // is notified with reconnectPending_ == true, sleeps the back-off delay,
    // then calls Connect() again.  It exits when running_ is false.
    reconnectThread_ = std::thread([this] {
        while (running_) {
            int delayMs = 0;
            {
                std::unique_lock<std::mutex> lk(reconnectCvMutex_);
                reconnectCv_.wait(lk, [this] {
                    return reconnectPending_ || !running_;
                });
                if (!running_) break;
                reconnectPending_ = false;
                std::lock_guard<std::mutex> rlk(reconnectMutex_);
                delayMs = reconnectDelayMs_;
                // Advance back-off (cap at 8 s).
                reconnectDelayMs_ = std::min(reconnectDelayMs_ * 2, 8000);
            }
            Logger::Infof("[ViewerSignaling] reconnecting in %d ms", delayMs);
            // Interruptible sleep: check running_ every 250 ms.
            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(delayMs);
            while (running_ && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(50ms);
            }
            if (running_) Connect();
        }
    });

    Connect();
}

void ViewerSignaling::Stop() {
    if (!running_.exchange(false)) {
        callbackFence_->StopAccepting();
        callbackFence_->WaitForIdle();
        return;
    }

    // Prevent new library callbacks from touching this object and invalidate
    // callbacks that have not yet passed their generation check.
    callbackFence_->StopAccepting();
    generation_.fetch_add(1, std::memory_order_seq_cst);

    // Wake the reconnect thread so it exits.
    {
        std::lock_guard<std::mutex> lk(reconnectCvMutex_);
        reconnectPending_ = false;
    }
    reconnectCv_.notify_all();
    if (reconnectThread_.joinable()) reconnectThread_.join();

    // A callback that entered before StopAccepting may still be finishing.
    // Wait before clearing session/callback-owned state.
    callbackFence_->WaitForIdle();

    // Send client-session-closed before tearing down, if a session is live.
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        if (!sessionId_.empty() && !sessionToken_.empty()) {
            json closed = {
                {"type",         "client-session-closed"},
                {"sessionId",    sessionId_},
                {"sessionToken", sessionToken_},
                {"reason",       "Viewer closed"}
            };
            std::shared_ptr<rtc::WebSocket> sock;
            {
                std::lock_guard<std::mutex> slk(socketMutex_);
                sock = ws_;
            }
            if (sock && wsOpen_) {
                try { sock->send(closed.dump()); } catch (...) {}
            }
        }
        sessionId_.clear();
        sessionToken_.clear();
        state_ = ViewerSessionState::Disconnected;
    }

    std::shared_ptr<rtc::WebSocket> sock;
    {
        std::lock_guard<std::mutex> lk(socketMutex_);
        sock = std::move(ws_);
    }
    wsOpen_ = false;
    if (sock) {
        try { sock->close(); } catch (...) {}
    }

    // Drain the queue.
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        pendingQueue_.clear();
    }
}

// ---------------------------------------------------------------------------
// WebSocket connection
// ---------------------------------------------------------------------------

void ViewerSignaling::Connect() {
    // Assign a fresh generation token so stale callbacks self-discard.
    const uint64_t gen = generation_.fetch_add(1, std::memory_order_seq_cst) + 1;

    auto socket = std::make_shared<rtc::WebSocket>();
    {
        std::lock_guard<std::mutex> lk(socketMutex_);
        ws_ = socket;
    }
    wsOpen_ = false;
    auto callbackFence = callbackFence_;

    // ── onOpen ──────────────────────────────────────────────────────────────
    socket->onOpen([this, gen, callbackFence] {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        if (generation_.load(std::memory_order_seq_cst) != gen) return;

        wsOpen_ = true;
        // Reset back-off on successful connect.
        {
            std::lock_guard<std::mutex> lk(reconnectMutex_);
            reconnectDelayMs_ = 500;
        }
        Logger::Info("[ViewerSignaling] WebSocket connected");

        // If we already have a live session, attempt to resume it first so
        // the server rebinds this socket before we flush any queued SDP/ICE.
        {
            std::lock_guard<std::mutex> lk(sessionMutex_);
            if (!sessionId_.empty() && !sessionToken_.empty()) {
                json resume = {
                    {"type",         "client-resume-session"},
                    {"sessionId",    sessionId_},
                    {"sessionToken", sessionToken_}
                };
                try {
                    std::shared_ptr<rtc::WebSocket> sock;
                    {
                        std::lock_guard<std::mutex> slk(socketMutex_);
                        sock = ws_;
                    }
                    if (sock) sock->send(resume.dump());
                    Logger::Infof("[ViewerSignaling] session resume sent (%s)", sessionId_.c_str());
                } catch (const std::exception& e) {
                    Logger::Errorf("[ViewerSignaling] resume send failed: %s", e.what());
                }
                // Fall through to FlushQueue() — queued SDP/ICE goes after resume.
            } else {
                // No live session: issue a fresh connect-request.
                // We must NOT hold sessionMutex_ while calling Send() because
                // Send() acquires queueMutex_ and may re-enter socketMutex_.
                // Release here, send below.
            }
        }

        // Send queued connect-request or buffered SDP/ICE messages.
        FlushQueue();
    });

    // ── onMessage ────────────────────────────────────────────────────────────
    socket->onMessage(
        [](rtc::binary) {}, // ignore binary frames on the signaling socket
        [this, gen, callbackFence](rtc::string text) {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            if (generation_.load(std::memory_order_seq_cst) != gen) return;
            HandleMessage(text);
        }
    );

    // ── onClosed ─────────────────────────────────────────────────────────────
    socket->onClosed([this, gen, callbackFence] {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        if (generation_.load(std::memory_order_seq_cst) != gen) return;
        wsOpen_ = false;
        Logger::Warning("[ViewerSignaling] WebSocket closed");

        // Notify state-machine: if we were mid-session, show reconnecting.
        {
            ViewerSessionState cur;
            {
                std::lock_guard<std::mutex> lk(sessionMutex_);
                cur = state_;
            }
            if (cur == ViewerSessionState::AwaitingApproval ||
                cur == ViewerSessionState::Negotiating) {
                FireState(cur, "Reconnecting to signaling service");
            }
        }

        if (running_) {
            // Schedule a reconnect via the reconnect-timer thread.
            {
                std::lock_guard<std::mutex> lk(reconnectCvMutex_);
                reconnectPending_ = true;
            }
            reconnectCv_.notify_one();
        }
    });

    // ── onError ──────────────────────────────────────────────────────────────
    socket->onError([this, gen, callbackFence](const std::string& err) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        if (generation_.load(std::memory_order_seq_cst) != gen) return;
        Logger::Errorf("[ViewerSignaling] WebSocket error: %s", err.c_str());
        // onClosed will fire next; reconnect happens there.
    });

    // Enqueue the initial connect-request.  If the socket opens before
    // FlushQueue() runs this message stays in the queue; FlushQueue() drains it.
    // If we already have a session the queue may have queued SDP, which is fine.
    bool hasSession = false;
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        hasSession = !sessionId_.empty() && !sessionToken_.empty();
    }
    if (!hasSession) {
        json req = {
            {"type",           "client-connect-request"},
            {"publicDeviceId", deviceId_}
        };
        std::lock_guard<std::mutex> lk(queueMutex_);
        // Clear any stale connect-request from previous attempt.
        pendingQueue_.erase(
            std::remove_if(pendingQueue_.begin(), pendingQueue_.end(),
                [](const json& m) {
                    return m.value("type", "") == "client-connect-request";
                }),
            pendingQueue_.end());
        pendingQueue_.push_front(std::move(req));
    }

    try {
        socket->open(serverUrl_);
    } catch (const std::exception& e) {
        Logger::Errorf("[ViewerSignaling] socket open failed: %s", e.what());
    }
}

// ---------------------------------------------------------------------------
// Inbound message handling
// ---------------------------------------------------------------------------

void ViewerSignaling::HandleMessage(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (...) {
        Logger::Warning("[ViewerSignaling] ignored malformed message");
        return;
    }
    if (!msg.is_object()) return;

    const std::string type = msg.value("type", "");

    // ── error ─────────────────────────────────────────────────────────────
    if (type == "error") {
        const std::string code    = msg.value("code",    "UNKNOWN");
        const std::string message = msg.value("message", "");
        Logger::Errorf("[ViewerSignaling] server error %s: %s", code.c_str(), message.c_str());
        FireError(code, message);

        // A SESSION_RESUME_FAILED error means we must start fresh.
        if (code == "SESSION_RESUME_FAILED") {
            {
                std::lock_guard<std::mutex> lk(sessionMutex_);
                sessionId_.clear();
                sessionToken_.clear();
                state_ = ViewerSessionState::Failed;
            }
            FireState(ViewerSessionState::Failed, message);
        }
        return;
    }

    // ── client-session-state ──────────────────────────────────────────────
    if (type == "client-session-state") {
        const std::string incomingId = msg.value("sessionId", "");

        std::string capturedSessionId;
        {
            std::lock_guard<std::mutex> lk(sessionMutex_);

            // Guard against stale state messages for a previous session.
            if (!sessionId_.empty() && incomingId != sessionId_) {
                Logger::Warningf("[ViewerSignaling] ignored stale client-session-state "
                                 "(expected %s, got %s)",
                                 sessionId_.c_str(), incomingId.c_str());
                return;
            }

            sessionId_ = incomingId;
            if (msg.contains("sessionToken") && !msg["sessionToken"].is_null()) {
                sessionToken_ = msg["sessionToken"].get<std::string>();
            }

            const std::string stateStr = msg.value("state", "OFFLINE");
            state_ = ParseViewerSessionState(stateStr);
            capturedSessionId = sessionId_;
        }

        const std::string message  = msg.value("message",  "");
        const ViewerSessionState st = [&] {
            std::lock_guard<std::mutex> lk(sessionMutex_);
            return state_;
        }();

        Logger::Infof("[ViewerSignaling] session %s → %s: %s",
                      capturedSessionId.c_str(),
                      ViewerSessionStateString(st),
                      message.c_str());

        // Host capabilities (advertised by the server).
        if (msg.contains("hostCapabilities") && msg["hostCapabilities"].is_object()) {
            FireCapabilities(ParseCapabilities(msg["hostCapabilities"]));
        }

        // ICE servers are only delivered in the NEGOTIATING state.
        if (st == ViewerSessionState::Negotiating &&
            msg.contains("iceServers") && msg["iceServers"].is_array()) {
            FireIceServers(ParseIceServers(msg["iceServers"]));
        }

        FireState(st, message);
        return;
    }

    // ── client-peer-signal ────────────────────────────────────────────────
    if (type == "client-peer-signal") {
        const std::string incomingId = msg.value("sessionId", "");
        {
            std::lock_guard<std::mutex> lk(sessionMutex_);
            if (incomingId != sessionId_) return; // wrong session
        }
        if (!msg.contains("payload") || !msg["payload"].is_object()) return;
        FirePeerSignal(msg["payload"]);
        return;
    }

    Logger::Debugf("[ViewerSignaling] ignored unhandled message type: %s", type.c_str());
}

// ---------------------------------------------------------------------------
// Outbound signaling
// ---------------------------------------------------------------------------

void ViewerSignaling::SendSignal(const nlohmann::json& payload) {
    std::string sid, tok;
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        sid = sessionId_;
        tok = sessionToken_;
    }
    if (sid.empty() || tok.empty()) {
        Logger::Warning("[ViewerSignaling] SendSignal: no active session");
        return;
    }
    Send({
        {"type",         "client-signal"},
        {"sessionId",    sid},
        {"sessionToken", tok},
        {"payload",      payload}
    });
}

void ViewerSignaling::SendConnectionEstablished(bool relay) {
    std::string sid, tok;
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        sid = sessionId_;
        tok = sessionToken_;
    }
    if (sid.empty() || tok.empty()) return;
    Send({
        {"type",           "client-session-established"},
        {"sessionId",      sid},
        {"sessionToken",   tok},
        {"connectionType", relay ? "relay" : "direct"}
    });
    Logger::Infof("[ViewerSignaling] connection established (%s)",
                  relay ? "relay" : "direct");
}

void ViewerSignaling::SendSessionClosed(const std::string& reason) {
    std::string sid, tok;
    {
        std::lock_guard<std::mutex> lk(sessionMutex_);
        sid = sessionId_;
        tok = sessionToken_;
        // Clear immediately — we are done with this session.
        sessionId_.clear();
        sessionToken_.clear();
        state_ = ViewerSessionState::Disconnected;
    }
    if (sid.empty() || tok.empty()) return;
    Send({
        {"type",         "client-session-closed"},
        {"sessionId",    sid},
        {"sessionToken", tok},
        {"reason",       reason}
    });
    Logger::Infof("[ViewerSignaling] session closed: %s", reason.c_str());
}

// ---------------------------------------------------------------------------
// Send — queued, bounded, SDP-safe
// ---------------------------------------------------------------------------

void ViewerSignaling::Send(nlohmann::json msg) {
    if (!running_) return;

    const std::string type = msg.value("type", "");

    // Try immediate delivery.
    {
        std::shared_ptr<rtc::WebSocket> sock;
        {
            std::lock_guard<std::mutex> lk(socketMutex_);
            sock = ws_;
        }
        if (sock && wsOpen_) {
            try {
                sock->send(msg.dump());
                return;
            } catch (const std::exception& e) {
                Logger::Errorf("[ViewerSignaling] send failed: %s", e.what());
                // Fall through to queue.
            }
        }
    }

    // Socket not ready — queue the message.
    // Heartbeat-style messages (client-session-established after reconnect)
    // are not themselves heartbeats, but client-session-closed is disposable
    // if the socket is already gone.  However, the protocol does not define
    // a "heartbeat" for the client side, so we queue everything except
    // duplicate connect-requests.
    const bool isConnectReq = (type == "client-connect-request");

    // client-session-closed sent during Stop() should still be attempted once
    // but not accumulated (it has no meaningful retry value once the object is
    // being torn down).  We allow it through the queue; Stop() will drain.
    std::lock_guard<std::mutex> lk(queueMutex_);

    // De-duplicate connect-requests: only keep the latest.
    if (isConnectReq) {
        pendingQueue_.erase(
            std::remove_if(pendingQueue_.begin(), pendingQueue_.end(),
                [](const json& m) {
                    return m.value("type", "") == "client-connect-request";
                }),
            pendingQueue_.end());
    }

    // Bounded queue: under pressure, prefer to drop plain ICE candidates.
    if (pendingQueue_.size() >= kMaxQueueSize) {
        const bool incomingIsCandidate =
            type == "client-signal" &&
            msg.contains("payload") &&
            msg["payload"].value("kind", "") == "candidate";

        // Find an ordinary ICE candidate already in the queue to evict.
        auto it = std::find_if(pendingQueue_.begin(), pendingQueue_.end(),
            [](const json& queued) {
                return queued.value("type", "") == "client-signal" &&
                       queued.contains("payload") &&
                       queued["payload"].value("kind", "") == "candidate";
            });

        if (it != pendingQueue_.end()) {
            pendingQueue_.erase(it);
            Logger::Warning("[ViewerSignaling] queue full — evicted a queued ICE candidate");
        } else if (incomingIsCandidate) {
            // No queued candidate to evict; incoming is a plain candidate — drop it.
            Logger::Warning("[ViewerSignaling] queue full — dropped incoming ICE candidate "
                            "(SDP/control preserved)");
            return;
        } else {
            // Queue is full and the incoming is control-plane (SDP/answer/session
            // messages).  Evict the oldest entry regardless.
            Logger::Warning("[ViewerSignaling] control-signal queue exceeded ICE budget — "
                            "evicting oldest entry");
            pendingQueue_.pop_front();
        }
    }

    pendingQueue_.push_back(std::move(msg));
    Logger::Warningf("[ViewerSignaling] queued outbound %s (queue size: %zu)",
                     type.c_str(), pendingQueue_.size());
}

// ---------------------------------------------------------------------------
// FlushQueue — drains pendingQueue_ over the open socket
// ---------------------------------------------------------------------------

void ViewerSignaling::FlushQueue() {
    std::deque<json> batch;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        batch.swap(pendingQueue_);
    }
    if (batch.empty()) return;

    Logger::Infof("[ViewerSignaling] flushing %zu queued message(s)", batch.size());
    for (auto& m : batch) {
        Send(std::move(m));
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

ViewerSessionState ViewerSignaling::State() const {
    std::lock_guard<std::mutex> lk(sessionMutex_);
    return state_;
}

std::string ViewerSignaling::SessionId() const {
    std::lock_guard<std::mutex> lk(sessionMutex_);
    return sessionId_;
}

std::string ViewerSignaling::SessionToken() const {
    std::lock_guard<std::mutex> lk(sessionMutex_);
    return sessionToken_;
}

bool ViewerSignaling::IsConnected() const {
    return wsOpen_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Callback dispatchers
// ---------------------------------------------------------------------------

void ViewerSignaling::FireState(ViewerSessionState s, const std::string& msg) {
    if (onStateChanged_) onStateChanged_(s, msg);
}

void ViewerSignaling::FirePeerSignal(const nlohmann::json& payload) {
    if (onPeerSignal_) onPeerSignal_(payload);
}

void ViewerSignaling::FireCapabilities(const ViewerHostCapabilities& caps) {
    if (onCapabilities_) onCapabilities_(caps);
}

void ViewerSignaling::FireIceServers(const std::vector<IceServerCfg>& servers) {
    if (onIceServers_) onIceServers_(servers);
}

void ViewerSignaling::FireError(const std::string& code, const std::string& message) {
    if (onError_) onError_(code, message);
}

// ---------------------------------------------------------------------------
// Protocol helpers
// ---------------------------------------------------------------------------

std::vector<IceServerCfg> ViewerSignaling::ParseIceServers(const nlohmann::json& arr) {
    std::vector<IceServerCfg> result;
    if (!arr.is_array()) return result;
    for (const auto& s : arr) {
        if (!s.is_object() || !s.contains("urls") || !s["urls"].is_array()) continue;
        const std::string username   = s.value("username",   "");
        const std::string credential = s.value("credential", "");
        for (const auto& u : s["urls"]) {
            if (!u.is_string()) continue;
            IceServerCfg cfg;
            cfg.url        = u.get<std::string>();
            cfg.username   = username;
            cfg.credential = credential;
            result.push_back(std::move(cfg));
        }
    }
    return result;
}

ViewerHostCapabilities ViewerSignaling::ParseCapabilities(const nlohmann::json& obj) {
    ViewerHostCapabilities caps;
    if (!obj.is_object()) return caps;
    caps.h264          = obj.value("h264",          false);
    caps.hevc          = obj.value("hevc",          false);
    caps.av1           = obj.value("av1",           false);
    caps.maxWidth      = obj.value("maxWidth",      0);
    caps.maxHeight     = obj.value("maxHeight",     0);
    caps.maxFps        = obj.value("maxFps",        0);
    caps.desktopWidth  = obj.value("desktopWidth",  0);
    caps.desktopHeight = obj.value("desktopHeight", 0);
    caps.desktopHz     = obj.value("desktopHz",     0);
    caps.gpuName       = obj.value("gpuName",       std::string{});
    caps.encoderName   = obj.value("encoderName",   std::string{});
    return caps;
}

std::string ViewerSignaling::NormalizeDeviceId(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c >= '0' && c <= '9') out += c;
    }
    return out;
}

} // namespace remcote
