#include "DeviceRegistration.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <variant>

using nlohmann::json;

namespace remcote {

// The device secret persists next to the exe so the machine keeps its Device
// ID across restarts. It is NOT an access credential — a remote user still
// cannot connect without the Host clicking ALLOW (spec §28).
static const char* kSecretFile = "remcote-device.json";

static void LoadIdentity(std::string& publicId, std::string& secret) {
    std::ifstream in(kSecretFile);
    if (!in) return;
    try {
        json j;
        in >> j;
        publicId = j.value("publicDeviceId", "");
        secret = j.value("secretToken", "");
    } catch (...) {}
}

static void SaveIdentity(const std::string& publicId, const std::string& secret) {
    std::ofstream out(kSecretFile);
    if (!out) return;
    json j = {{"publicDeviceId", publicId}, {"secretToken", secret}};
    out << j.dump(2);
}

DeviceRegistration::DeviceRegistration(std::string serverUrl, HostCapabilities caps)
    : serverUrl_(std::move(serverUrl)), caps_(std::move(caps)) {
    LoadIdentity(publicDeviceId_, secretToken_);
}

DeviceRegistration::~DeviceRegistration() { Stop(); }

void DeviceRegistration::Start() {
    running_ = true;
    Connect();
    heartbeatThread_ = std::thread([this] { HeartbeatLoop(); });
}

void DeviceRegistration::Stop() {
    if (!running_.exchange(false)) {
        callbackFence_->StopAccepting();
        callbackFence_->WaitForIdle();
        return;
    }
    registered_ = false;
    connected_ = false;
    connectionGeneration_.fetch_add(1);
    callbackFence_->StopAccepting();
    heartbeatCv_.notify_all();
    if (heartbeatThread_.joinable()) heartbeatThread_.join();

    // A stale onClosed callback may have been between its reconnect checks.
    // Wait for it before taking the final socket snapshot.
    callbackFence_->WaitForIdle();

    std::shared_ptr<rtc::WebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket = std::move(ws_);
    }
    if (socket) {
        try { socket->close(); } catch (...) {}
    }
}

void DeviceRegistration::Connect() {
    const uint64_t generation = connectionGeneration_.fetch_add(1) + 1;
    auto socket = std::make_shared<rtc::WebSocket>();
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        ws_ = socket;
    }
    auto callbackFence = callbackFence_;

    socket->onOpen([this, generation, callbackFence] {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        if (connectionGeneration_.load() != generation) return;
        connected_ = true;
        registered_ = false;
        Logger::Infof("Signaling socket connected: %s", Logger::RedactUrl(serverUrl_).c_str());
        json reg = {
            {"type", "host-register"},
            {"name", "ReMCote Host"},
            {"capabilities",
             {{"h264", caps_.h264},
              {"hevc", caps_.hevc},
              {"av1", caps_.av1},
              {"gpuName", caps_.gpuName},
              {"encoderName", caps_.encoderName},
              {"desktopWidth", caps_.desktopWidth},
              {"desktopHeight", caps_.desktopHeight},
              {"desktopHz", caps_.desktopHz}}},
        };
        if (!publicDeviceId_.empty() && !secretToken_.empty()) {
            reg["publicDeviceId"] = publicDeviceId_;
            reg["secretToken"] = secretToken_;
        }
        Send(reg);
    });

    socket->onMessage(
        [](rtc::binary) {},  // ignore binary messages on the signaling socket
        [this, generation, callbackFence](rtc::string text) {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            if (connectionGeneration_.load() == generation) HandleMessage(text);
        }
    );

    socket->onClosed([this, generation, callbackFence] {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        if (connectionGeneration_.load() != generation) return;
        connected_ = false;
        registered_ = false;
        Logger::Warning("Signaling socket closed; reconnecting in 2 seconds");
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (running_ && connectionGeneration_.load() == generation)
                Connect(); // auto-reconnect keeps the device ONLINE
        }
    });

    try {
        socket->open(serverUrl_);
    } catch (const std::exception& e) {
        Logger::Errorf("Signaling socket failed to open: %s", e.what());
    }
}

void DeviceRegistration::HandleMessage(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (...) {
        Logger::Warning("Ignored malformed signaling message");
        return;
    }
    const std::string type = msg.value("type", "");

    if (type == "host-registered") {
        publicDeviceId_ = msg.value("publicDeviceId", "");
        secretToken_ = msg.value("secretToken", secretToken_);
        SaveIdentity(publicDeviceId_, secretToken_);
        std::vector<IceServerCfg> iceServers;
        if (msg.contains("iceServers")) {
            for (const auto& s : msg["iceServers"]) {
                if (!s.contains("urls")) continue;
                const std::string username   = s.value("username",   "");
                const std::string credential = s.value("credential", "");
                for (const auto& u : s["urls"]) {
                    IceServerCfg cfg;
                    cfg.url        = u.get<std::string>();
                    cfg.username   = username;
                    cfg.credential = credential;
                    iceServers.push_back(std::move(cfg));
                }
            }
        }
        const bool hasTurn = std::any_of(iceServers.begin(), iceServers.end(),
            [](const IceServerCfg& c) {
                return c.url.rfind("turn:", 0) == 0 || c.url.rfind("turns:", 0) == 0;
            });
        Logger::Infof("Device registration complete: %zu ICE server(s), TURN relay %s",
                      iceServers.size(), hasTurn ? "AVAILABLE" : "NOT configured");
        registered_ = true;
        FlushPendingMessages();
        if (onRegistered_) onRegistered_(publicDeviceId_, iceServers);
    } else if (type == "host-connect-request") {
        Logger::Info("Signaling server forwarded a connection request");
        if (onConnectRequest_)
            onConnectRequest_(msg.value("sessionId", ""), msg.value("clientDescription", ""));
    } else if (type == "host-peer-signal") {
        Logger::Debug("Received a WebRTC signaling message from the remote viewer");
        if (onPeerSignal_) onPeerSignal_(msg.value("sessionId", ""), msg.value("payload", json{}));
    } else if (type == "host-session-ended") {
        if (onSessionEnded_)
            onSessionEnded_(msg.value("sessionId", ""), msg.value("reason", ""));
    } else if (type == "error") {
        Logger::Error("Signaling server returned an error response");
    } else {
        Logger::Debug("Ignored an unhandled signaling message type");
    }
}

void DeviceRegistration::RespondToConnect(const std::string& sessionId, bool accept) {
    Send({{"type", "host-connect-response"}, {"sessionId", sessionId}, {"accept", accept}});
    Logger::Info(accept
        ? "Host approval submitted to the signaling server"
        : "Host rejection submitted to the signaling server");
}

void DeviceRegistration::SendSignal(const std::string& sessionId, const json& payload) {
    Send({{"type", "host-signal"}, {"sessionId", sessionId}, {"payload", payload}});
}

void DeviceRegistration::NotifySessionClosed(const std::string& sessionId, const std::string& reason) {
    Send({{"type", "host-session-closed"}, {"sessionId", sessionId}, {"reason", reason}});
}

void DeviceRegistration::Send(const json& msg) {
    if (!running_) return;
    const std::string type = msg.value("type", "");
    const bool isRegistration = type == "host-register";
    const bool isHeartbeat = type == "host-heartbeat";
    std::shared_ptr<rtc::WebSocket> socket;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        socket = ws_;
    }
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (socket && connected_ && (registered_ || isRegistration)) {
        try {
            socket->send(msg.dump());
            return;
        } catch (const std::exception& e) {
            Logger::Errorf("Could not send signaling message: %s", e.what());
        }
    }
    // Heartbeats are freshness probes and should never accumulate. Session
    // control and SDP/ICE messages must survive a brief signaling reconnect.
    if (!isHeartbeat && !isRegistration) {
        const auto candidateIt = std::find_if(
            pendingMessages_.begin(), pendingMessages_.end(),
            [](const json& queued) {
                return queued.value("type", "") == "host-signal" &&
                    queued.contains("payload") &&
                    queued["payload"].value("kind", "") == "candidate";
            });
        const bool incomingCandidate = type == "host-signal" &&
            msg.contains("payload") &&
            msg["payload"].value("kind", "") == "candidate";
        if (pendingMessages_.size() >= 512 && candidateIt != pendingMessages_.end()) {
            pendingMessages_.erase(candidateIt);
        } else if (pendingMessages_.size() >= 512 && incomingCandidate) {
            Logger::Warning("Dropped queued ICE candidate without dropping SDP/control");
            return;
        }
        pendingMessages_.push_back(msg);
        Logger::Warningf("Signaling unavailable; queued outbound %s message", type.c_str());
    }
}

void DeviceRegistration::FlushPendingMessages() {
    std::deque<json> pending;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        pending.swap(pendingMessages_);
    }
    if (pending.empty()) return;
    Logger::Infof("Flushing %zu queued signaling message(s)", pending.size());
    for (const auto& msg : pending) Send(msg);
}

void DeviceRegistration::HeartbeatLoop() {
    std::unique_lock<std::mutex> lock(heartbeatMutex_);
    while (running_) {
        if (heartbeatCv_.wait_for(
                lock,
                std::chrono::seconds(15),
                [this] { return !running_.load(); })) {
            break;
        }
        lock.unlock();
        if (connected_) Send({{"type", "host-heartbeat"}});
        lock.lock();
    }
}

} // namespace remcote
