#include "DeviceRegistration.h"

#include <chrono>
#include <cstdio>
#include <fstream>

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
    running_ = false;
    if (ws_) {
        try { ws_->close(); } catch (...) {}
    }
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
}

void DeviceRegistration::Connect() {
    ws_ = std::make_shared<rtc::WebSocket>();

    ws_->onOpen([this] {
        connected_ = true;
        std::printf("[SIGNAL] connected to %s\n", serverUrl_.c_str());
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

    ws_->onMessage([this](auto data) {
        if (std::holds_alternative<rtc::string>(data)) {
            HandleMessage(std::get<rtc::string>(data));
        }
    });

    ws_->onClosed([this] {
        connected_ = false;
        std::fprintf(stderr, "[SIGNAL] connection closed\n");
        if (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (running_) Connect(); // auto-reconnect keeps the device ONLINE
        }
    });

    try {
        ws_->open(serverUrl_);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[SIGNAL] open failed: %s\n", e.what());
    }
}

void DeviceRegistration::HandleMessage(const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (...) {
        return;
    }
    const std::string type = msg.value("type", "");

    if (type == "host-registered") {
        publicDeviceId_ = msg.value("publicDeviceId", "");
        secretToken_ = msg.value("secretToken", secretToken_);
        SaveIdentity(publicDeviceId_, secretToken_);
        std::vector<std::string> iceServers;
        if (msg.contains("iceServers")) {
            for (const auto& s : msg["iceServers"]) {
                if (s.contains("urls")) {
                    for (const auto& u : s["urls"]) iceServers.push_back(u.get<std::string>());
                }
            }
        }
        std::printf("[SIGNAL] registered as %s\n", publicDeviceId_.c_str());
        if (onRegistered_) onRegistered_(publicDeviceId_, iceServers);
    } else if (type == "host-connect-request") {
        if (onConnectRequest_)
            onConnectRequest_(msg.value("sessionId", ""), msg.value("clientDescription", ""));
    } else if (type == "host-peer-signal") {
        if (onPeerSignal_) onPeerSignal_(msg.value("sessionId", ""), msg.value("payload", json{}));
    } else if (type == "host-session-ended") {
        if (onSessionEnded_)
            onSessionEnded_(msg.value("sessionId", ""), msg.value("reason", ""));
    } else if (type == "error") {
        std::fprintf(stderr, "[SIGNAL] server error: %s\n", msg.value("message", "").c_str());
    }
}

void DeviceRegistration::RespondToConnect(const std::string& sessionId, bool accept) {
    Send({{"type", "host-connect-response"}, {"sessionId", sessionId}, {"accept", accept}});
}

void DeviceRegistration::SendSignal(const std::string& sessionId, const json& payload) {
    Send({{"type", "host-signal"}, {"sessionId", sessionId}, {"payload", payload}});
}

void DeviceRegistration::NotifySessionClosed(const std::string& sessionId, const std::string& reason) {
    Send({{"type", "host-session-closed"}, {"sessionId", sessionId}, {"reason", reason}});
}

void DeviceRegistration::Send(const json& msg) {
    if (ws_ && connected_) {
        try { ws_->send(msg.dump()); } catch (...) {}
    }
}

void DeviceRegistration::HeartbeatLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        if (connected_) Send({{"type", "host-heartbeat"}});
    }
}

} // namespace remcote
