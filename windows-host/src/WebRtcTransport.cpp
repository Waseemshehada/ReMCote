#include "WebRtcTransport.h"

#include <cstdio>
#include <cstring>

using nlohmann::json;

namespace remcote {

// H.264 payload type for the answered video track.
static constexpr int kVideoPayloadType = 96;
static constexpr uint32_t kVideoSsrc = 42;

WebRtcTransport::WebRtcTransport(InputEngine& input, std::vector<std::string> iceServers)
    : input_(input), iceServers_(std::move(iceServers)) {}

std::shared_ptr<WebRtcTransport::Session> WebRtcTransport::CreateSession(const std::string& sessionId) {
    rtc::Configuration config;
    for (const auto& s : iceServers_) config.iceServers.emplace_back(s);
    // Prefer direct P2P; TURN (if present in iceServers_) is fallback only.

    auto s = std::make_shared<Session>();
    s->pc = std::make_shared<rtc::PeerConnection>(config);

    s->pc->onLocalDescription([this, sessionId](rtc::Description desc) {
        json payload = {{"kind", desc.typeString()}, {"sdp", std::string(desc)}};
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onLocalCandidate([this, sessionId](rtc::Candidate cand) {
        json payload = {
            {"kind", "candidate"},
            {"candidate", std::string(cand)},
            {"sdpMid", cand.mid()},
        };
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onStateChange([this, sessionId](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Connected) {
            std::printf("[WEBRTC] Peer connected\n");
        }
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            std::printf("[RTC] session %s peer state terminal\n", sessionId.c_str());
            if (sessionEnded_) sessionEnded_(sessionId);
        }
    });

    // The browser creates the data channels — we receive them.
    s->pc->onDataChannel([this, s](std::shared_ptr<rtc::DataChannel> dc) {
        WireDataChannel(s, std::move(dc));
    });

    return s;
}

void WebRtcTransport::WireDataChannel(const std::shared_ptr<Session>& s,
                                      std::shared_ptr<rtc::DataChannel> ch) {
    const std::string label = ch->label();
    std::printf("[WEBRTC] DataChannel opened: %s\n", label.c_str());
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
            // Add the H.264 video track we will feed from NVENC. Because the
            // browser offered a recvonly video m-line, this becomes sendonly.
            rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
            media.addH264Codec(kVideoPayloadType);
            media.addSSRC(kVideoSsrc, "remcote-video");
            s->videoTrack = s->pc->addTrack(media);
            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
                kVideoSsrc, "remcote-video", kVideoPayloadType,
                rtc::H264RtpPacketizer::defaultClockRate);
            auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, rtpConfig);
            s->srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
            packetizer->addToChain(s->srReporter);
            packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
            s->videoTrack->setMediaHandler(packetizer);
            s->videoTrack->onOpen([s] {
                s->trackOpen = true;
                std::printf("[WEBRTC] Video track active\n");
            });
            // A NACK / PLI from the client means "send me a keyframe now."
            s->videoTrack->onMessage([this](rtc::binary) {}, [](rtc::string) {});
            sessions_[sessionId] = s;
        } else {
            s = it->second;
        }
    }

    const std::string kind = payload.value("kind", "");
    if (kind == "offer") {
        std::printf("[WEBRTC] Offer received — creating answer\n");
        s->pc->setRemoteDescription(rtc::Description(payload.value("sdp", ""), "offer"));
        // libdatachannel auto-creates the answer via onLocalDescription.
    } else if (kind == "candidate") {
        std::string cand = payload.value("candidate", "");
        std::string mid = payload.value("sdpMid", "0");
        if (!cand.empty()) s->pc->addRemoteCandidate(rtc::Candidate(cand, mid));
    }
}

void WebRtcTransport::SendFrame(const EncodedFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.empty()) return;
    // Present timestamp in 90 kHz clock units.
    const uint32_t rtpTs = static_cast<uint32_t>(frame.captureUs * 90 / 1000);
    for (auto& [id, s] : sessions_) {
        if (!s->videoTrack || !s->trackOpen || !s->videoTrack->isOpen()) continue;
        if (s->srReporter) s->srReporter->rtpConfig->timestamp = rtpTs;
        try {
            s->videoTrack->send(reinterpret_cast<const std::byte*>(frame.data), frame.size);
            if (!firstFrameSent_.exchange(true)) std::printf("[VIDEO] First frame sent\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[RTC] send failed: %s\n", e.what());
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
