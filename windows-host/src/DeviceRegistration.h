#pragma once
// Signaling client: connects to the ReMCote server over WebSocket, registers
// this device, keeps presence alive, and relays SDP/ICE. Control plane only —
// never carries media (spec §7).

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "Common.h"

namespace remcote {

class DeviceRegistration {
public:
    using ConnectRequest = std::function<void(const std::string& sessionId, const std::string& desc)>;
    using PeerSignal = std::function<void(const std::string& sessionId, const nlohmann::json& payload)>;
    using SessionEnded = std::function<void(const std::string& sessionId, const std::string& reason)>;
    using Registered = std::function<void(const std::string& publicDeviceId,
                                          const std::vector<IceServerCfg>& iceServers)>;

    DeviceRegistration(std::string serverUrl, HostCapabilities caps);
    ~DeviceRegistration();

    void SetOnConnectRequest(ConnectRequest cb) { onConnectRequest_ = std::move(cb); }
    void SetOnPeerSignal(PeerSignal cb) { onPeerSignal_ = std::move(cb); }
    void SetOnSessionEnded(SessionEnded cb) { onSessionEnded_ = std::move(cb); }
    void SetOnRegistered(Registered cb) { onRegistered_ = std::move(cb); }

    void Start();
    void Stop();

    // Host-side approval decision for a pending connection request (spec §4).
    void RespondToConnect(const std::string& sessionId, bool accept);
    // Relay our SDP answer / ICE candidate to the client via the server.
    void SendSignal(const std::string& sessionId, const nlohmann::json& payload);
    void NotifySessionClosed(const std::string& sessionId, const std::string& reason);

    std::string PublicDeviceId() const { return publicDeviceId_; }

private:
    void Connect();
    void HandleMessage(const std::string& text);
    void Send(const nlohmann::json& msg);
    void HeartbeatLoop();

    std::string serverUrl_;
    HostCapabilities caps_;
    std::string publicDeviceId_;
    std::string secretToken_;   // persisted so this device keeps its ID across restarts
    std::shared_ptr<rtc::WebSocket> ws_;
    std::thread heartbeatThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    ConnectRequest onConnectRequest_;
    PeerSignal onPeerSignal_;
    SessionEnded onSessionEnded_;
    Registered onRegistered_;
};

} // namespace remcote
