#include "WebRtcTransport.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

using nlohmann::json;

namespace remcote {

static constexpr uint32_t kVideoSsrc = 42;

WebRtcTransport::WebRtcTransport(InputEngine& input, std::vector<IceServerCfg> iceServers)
    : input_(input), iceServers_(std::move(iceServers)) {
    Logger::Infof("WebRTC transport initialized with %zu ICE server(s)", iceServers_.size());
}

std::shared_ptr<WebRtcTransport::Session> WebRtcTransport::CreateSession(const std::string& sessionId) {
    rtc::Configuration config;
    // The browser is the offerer. Wait until its offered video m-line has been
    // received and configured before creating our answer (see onTrack below).
    // Otherwise libdatachannel generates an answer without our H.264 SSRC.
    config.disableAutoNegotiation = true;
    for (const auto& cfg : iceServers_) {
        rtc::IceServer srv(cfg.url);
        if (!cfg.username.empty())   srv.username = cfg.username;
        if (!cfg.credential.empty()) srv.password = cfg.credential;
        config.iceServers.push_back(std::move(srv));
    }
    // Prefer direct P2P; TURN (if present in iceServers_) is used as relay fallback only.

    auto s = std::make_shared<Session>();
    s->pc = std::make_shared<rtc::PeerConnection>(config);

    s->pc->onLocalDescription([this, sessionId](rtc::Description desc) {
        Logger::Debugf("Generated local WebRTC %s description", desc.typeString().c_str());
        json payload = {{"kind", desc.typeString()}, {"sdp", std::string(desc)}};
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onLocalCandidate([this, sessionId](rtc::Candidate cand) {
        Logger::Debug("Generated a local ICE candidate");
        json payload = {
            {"kind", "candidate"},
            {"candidate", std::string(cand)},
            {"sdpMid", cand.mid()},
        };
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onStateChange([this, sessionId](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected) {
            Logger::Info("WebRTC peer connected");
        }
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            Logger::Warning("WebRTC peer entered a terminal state");
            if (sessionEnded_) sessionEnded_(sessionId);
        }
    });

    // The browser creates the data channels — we receive them.
    s->pc->onDataChannel([this, s](std::shared_ptr<rtc::DataChannel> dc) {
        WireDataChannel(s, std::move(dc));
    });

    // libdatachannel creates a reciprocated local Track for every remote media
    // m-line when setRemoteDescription(offer) runs. Its description preserves
    // the browser's codec payload types. Reuse that exact H.264 payload type:
    // a fixed PT is invalid because Chrome commonly assigns PT 96 to VP8.
    s->pc->onTrack([this, s](std::shared_ptr<rtc::Track> offeredTrack) {
        rtc::Description::Media media = offeredTrack->description();
        if (media.type() != "video") return;

        int h264PayloadType = -1;
        for (const int payloadType : media.payloadTypes()) {
            const auto* rtpMap = media.rtpMap(payloadType);
            if (!rtpMap) continue;

            std::string codec = rtpMap->format;
            std::transform(codec.begin(), codec.end(), codec.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (codec == "H264") {
                h264PayloadType = payloadType;
                break;
            }
        }

        if (h264PayloadType < 0) {
            Logger::Error("Remote browser did not offer an H.264 video codec");
            return;
        }

        bool createAnswer = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (s->videoTrack) return; // ignore any renegotiated duplicate

            // `media` is already the reciprocated (sendonly) description for
            // the browser's recvonly offer. Adding an SSRC makes it a valid
            // WebRTC sender in the answer.
            media.addSSRC(kVideoSsrc, "remcote-video");
            s->videoTrack = s->pc->addTrack(media);
            s->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                kVideoSsrc, "remcote-video", h264PayloadType,
                rtc::H264RtpPacketizer::ClockRate);

            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, s->rtpConfig);
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            s->videoTrack->setMediaHandler(packetizer);
            s->videoTrack->onOpen([this, s] {
                std::lock_guard<std::mutex> lock(mutex_);
                s->trackOpen = true;
                Logger::Info("WebRTC video track is active");
                // The first IDR can be produced before SRTP opens. Force a
                // fresh one now so the browser can decode immediately.
                if (keyframeRequest_) keyframeRequest_();
            });

            if (!s->answerStarted) {
                s->answerStarted = true;
                createAnswer = true;
            }
        }

        Logger::Infof("Negotiated H.264 video track (payload type %d)", h264PayloadType);
        if (createAnswer) {
            try {
                s->pc->setLocalDescription();
            } catch (const std::exception& e) {
                Logger::Errorf("WebRTC answer generation failed: %s", e.what());
            }
        }
    });

    return s;
}

void WebRtcTransport::WireDataChannel(const std::shared_ptr<Session>& s,
                                      std::shared_ptr<rtc::DataChannel> ch) {
    const std::string label = ch->label();
    Logger::Infof("WebRTC data channel opened: %s", label.c_str());
    if (label == kPointerChannel) {
        s->pointerCh = ch;
        ch->onMessage(
            [this](rtc::binary data) { HandlePointerBinary(data); },
            [](rtc::string) {});
    } else if (label == kReliableChannel) {
        s->reliableCh = ch;
        std::weak_ptr<Session> weak = s;
        ch->onMessage(
            [](rtc::binary) {},
            [this, weak](rtc::string text) {
                if (auto locked = weak.lock()) HandleReliableMessage(locked, text);
            });
    }
}

void WebRtcTransport::HandlePointerBinary(const rtc::binary& data) {
    // 9-byte format: [type u8][x f32 LE][y f32 LE] (see remcote-protocol).
    if (data.size() < 9) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
    if (bytes[0] != kPointerMoveType) return;
    float x, y;
    std::memcpy(&x, bytes + 1, 4);
    std::memcpy(&y, bytes + 5, 4);
    InputEvent ev;
    ev.kind = InputEvent::Kind::PointerMove;
    ev.x = x;
    ev.y = y;
    input_.Enqueue(ev);
}

void WebRtcTransport::HandleReliableMessage(const std::shared_ptr<Session>& s,
                                            const std::string& text) {
    json msg;
    try {
        msg = json::parse(text);
    } catch (...) {
        return;
    }
    const std::string t = msg.value("t", "");
    if (t == "ping") {
        // Immediate pong for the input-RTT diagnostic (spec §33).
        if (s->reliableCh && s->reliableCh->isOpen()) {
            json pong = {{"t", "pong"}, {"ts", msg.value("ts", 0.0)}};
            s->reliableCh->send(pong.dump());
        }
    } else if (t == "mb") {
        InputEvent ev;
        ev.kind = InputEvent::Kind::MouseButton;
        ev.button = msg.value("b", 0);
        ev.down = msg.value("d", false);
        ev.x = msg.value("x", 0.0f);
        ev.y = msg.value("y", 0.0f);
        input_.Enqueue(ev);
    } else if (t == "wheel") {
        InputEvent ev;
        ev.kind = InputEvent::Kind::Wheel;
        ev.dx = msg.value("dx", 0.0f);
        ev.dy = msg.value("dy", 0.0f);
        input_.Enqueue(ev);
    } else if (t == "kb") {
        InputEvent ev;
        ev.kind = InputEvent::Kind::Key;
        ev.scanCode = msg.value("sc", 0u);
        ev.code = msg.value("code", "");
        ev.down = msg.value("d", false);
        input_.Enqueue(ev);
    } else if (t == "bitrate") {
        if (bitrateRequest_) bitrateRequest_(msg.value("kbps", 0));
    } else if (t == "keyframe") {
        if (keyframeRequest_) keyframeRequest_();
    }
}

void WebRtcTransport::HandlePeerSignal(const std::string& sessionId, const json& payload) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) {
            s = CreateSession(sessionId);
            sessions_[sessionId] = s;
        } else {
            s = it->second;
        }
    }

    const std::string kind = payload.value("kind", "");
    if (kind == "offer") {
        Logger::Info("WebRTC offer received; creating answer");
        s->pc->setRemoteDescription(rtc::Description(payload.value("sdp", ""), "offer"));
        // onTrack selects the browser's H.264 payload type, attaches our SSRC,
        // then explicitly generates the answer.
    } else if (kind == "candidate") {
        std::string cand = payload.value("candidate", "");
        std::string mid = payload.value("sdpMid", "0");
        if (!cand.empty()) {
            s->pc->addRemoteCandidate(rtc::Candidate(cand, mid));
            Logger::Debug("Accepted a remote ICE candidate");
        }
    }
}

void WebRtcTransport::SendFrame(const EncodedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.empty()) return;
    // Present timestamp in 90 kHz clock units.
    const uint32_t rtpTs = static_cast<uint32_t>(frame.captureUs * 90 / 1000);
    for (auto& [id, s] : sessions_) {
        if (!s->videoTrack || !s->trackOpen || !s->videoTrack->isOpen()) continue;
        if (s->rtpConfig) s->rtpConfig->timestamp = rtpTs;
        try {
            s->videoTrack->send(reinterpret_cast<const std::byte*>(frame.data), frame.size);
            if (!firstFrameSent_.exchange(true)) Logger::Info("First video frame sent over WebRTC");
        } catch (const std::exception& e) {
            Logger::Errorf("WebRTC video-frame send failed: %s", e.what());
        }
    }
}

void WebRtcTransport::SendCursor(float x, float y, bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, s] : sessions_) {
        if (s->reliableCh && s->reliableCh->isOpen()) {
            json msg = {{"t", "cursor"}, {"x", x}, {"y", y}, {"visible", visible}};
            s->reliableCh->send(msg.dump());
        }
    }
}

bool WebRtcTransport::HasActiveSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, s] : sessions_) {
        if (s->pc && s->pc->state() == rtc::PeerConnection::State::Connected) return true;
    }
    return false;
}

void WebRtcTransport::CloseSession(const std::string& sessionId) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return;
        s = it->second;
        sessions_.erase(it);
    }
    if (s && s->pc) s->pc->close();
}

void WebRtcTransport::CloseAll() {
    std::map<std::string, std::shared_ptr<Session>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy.swap(sessions_);
    }
    for (auto& [id, s] : copy) {
        if (s->pc) s->pc->close();
    }
}

} // namespace remcote
