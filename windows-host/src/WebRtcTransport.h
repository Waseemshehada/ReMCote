#pragma once
// WebRTC transport via libdatachannel. The host is the ANSWERER: the native
// viewer creates the offer, the two input data channels, and a recvonly video
// transceiver. We attach our NVENC H.264 track and answer.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>

#include "Common.h"
#include "CallbackFence.h"
#include "InputEngine.h"

namespace remcote {

class WebRtcTransport {
public:
    using SignalOut = std::function<void(const std::string& sessionId, const nlohmann::json& payload)>;
    using BitrateRequest = std::function<void(int kbps)>;
    using KeyframeRequest = std::function<void()>;
    using SessionEnded = std::function<void(const std::string& sessionId)>;

    WebRtcTransport(InputEngine& input, std::vector<IceServerCfg> iceServers);
    ~WebRtcTransport();

    void SetSignalOut(SignalOut cb) { signalOut_ = std::move(cb); }
    void SetBitrateRequest(BitrateRequest cb) { bitrateRequest_ = std::move(cb); }
    void SetKeyframeRequest(KeyframeRequest cb) { keyframeRequest_ = std::move(cb); }
    void SetSessionEnded(SessionEnded cb) { sessionEnded_ = std::move(cb); }

    // Signaling payload from the client (offer / candidate) relayed by server.
    void HandlePeerSignal(const std::string& sessionId, const nlohmann::json& payload);
    void CloseSession(const std::string& sessionId);
    void CloseAll();

    // Called from the encoder output path with a complete H.264 access unit.
    void SendFrame(const EncodedFrame& frame);
    void SendCursor(float x, float y, bool visible);

    bool HasActiveSession();

private:
    struct Session {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::Track> videoTrack;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig; // for timestamp updates
        std::shared_ptr<rtc::DataChannel> pointerCh;
        std::shared_ptr<rtc::DataChannel> reliableCh;
        bool answerStarted = false;
        std::atomic<bool> terminalNotified{false};
        std::shared_ptr<CallbackFence> callbackFence{
            std::make_shared<CallbackFence>()};
    };

    std::shared_ptr<Session> CreateSession(const std::string& sessionId);
    bool ConfigureVideoSender(
        const std::shared_ptr<Session>& session,
        const rtc::Description& offer);
    void WireDataChannel(const std::shared_ptr<Session>& s,
                         std::shared_ptr<rtc::DataChannel> ch);
    void HandleReliableMessage(const std::shared_ptr<Session>& s, const std::string& text);
    void HandlePointerBinary(const rtc::binary& data);

    InputEngine& input_;
    std::vector<IceServerCfg> iceServers_;
    SignalOut signalOut_;
    BitrateRequest bitrateRequest_;
    KeyframeRequest keyframeRequest_;
    SessionEnded sessionEnded_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::atomic<bool> firstFrameSent_{false}; // first-frame diagnostic, logs once
};

} // namespace remcote
