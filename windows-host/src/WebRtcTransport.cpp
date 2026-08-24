#include "WebRtcTransport.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <variant>

using nlohmann::json;

namespace remcote {

static constexpr uint32_t kVideoSsrc = 42;

WebRtcTransport::WebRtcTransport(InputEngine& input, std::vector<IceServerCfg> iceServers)
    : input_(input), iceServers_(std::move(iceServers)) {
    Logger::Infof("WebRTC transport initialized with %zu ICE server(s)", iceServers_.size());
}

WebRtcTransport::~WebRtcTransport() { CloseAll(); }

std::shared_ptr<WebRtcTransport::Session> WebRtcTransport::CreateSession(const std::string& sessionId) {
    rtc::Configuration config;
    // The native viewer is the offerer. We configure its H.264 video m-line
    // after applying the offer, then explicitly generate the host answer.
    config.disableAutoNegotiation = true;
    // The native viewer's recvonly video m-line means the host adds its H.264
    // sender after applying the offer. Without this, libdatachannel may build
    // only the SCTP data transport, leaving control connected but never opening
    // the SRTP video track.
    config.forceMediaTransport = true;
    for (const auto& cfg : iceServers_) {
        rtc::IceServer srv(cfg.url);
        if (!cfg.username.empty())   srv.username = cfg.username;
        if (!cfg.credential.empty()) srv.password = cfg.credential;
        config.iceServers.push_back(std::move(srv));
    }
    // Prefer direct P2P; TURN (if present in iceServers_) is used as relay fallback only.

    auto s = std::make_shared<Session>();
    s->pc = std::make_shared<rtc::PeerConnection>(config);
    auto callbackFence = s->callbackFence;
    std::weak_ptr<Session> weakSession = s;

    s->pc->onLocalDescription([this, sessionId, callbackFence](rtc::Description desc) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        Logger::Debugf("Generated local WebRTC %s description", desc.typeString().c_str());
        json payload = {{"kind", desc.typeString()}, {"sdp", std::string(desc)}};
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onLocalCandidate([this, sessionId, callbackFence](rtc::Candidate cand) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        Logger::Debug("Generated a local ICE candidate");
        json payload = {
            {"kind", "candidate"},
            {"candidate", std::string(cand)},
            {"sdpMid", cand.mid()},
        };
        if (signalOut_) signalOut_(sessionId, payload);
    });
    s->pc->onStateChange([this, weakSession, sessionId, callbackFence](rtc::PeerConnection::State state) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        auto s = weakSession.lock();
        if (!s) return;
        if (state == rtc::PeerConnection::State::Connected) {
            Logger::Info("WebRTC peer connected");
            // Track::onOpen is diagnostic only for a locally-added sendonly
            // media track. The peer state is the reliable point to request a
            // new IDR after DTLS-SRTP is ready, including TURN relay sessions.
            KeyframeRequest keyframe;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                keyframe = keyframeRequest_;
            }
            if (keyframe) keyframe();
        }
        if (state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Closed) {
            Logger::Warning("WebRTC peer entered a terminal state");
            if (!s->terminalNotified.exchange(true) && sessionEnded_) {
                sessionEnded_(sessionId);
            }
        }
    });

    // The native viewer creates the data channels — we receive them.
    s->pc->onDataChannel([this, weakSession, callbackFence](std::shared_ptr<rtc::DataChannel> dc) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        auto s = weakSession.lock();
        if (!s) return;
        WireDataChannel(s, std::move(dc));
    });

    // libdatachannel creates a reciprocated local Track for the viewer's
    // recvonly video m-line while applying the offer. Configure the host
    // sender from that exact track description so the answer reuses the
    // negotiated mid and codec mapping.
    s->pc->onTrack([this, weakSession, callbackFence](std::shared_ptr<rtc::Track> offeredTrack) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        auto s = weakSession.lock();
        if (!s) return;
        Logger::Debugf(
            "Received remote %s track; configuring the negotiated sender",
            offeredTrack->description().type().c_str());
        if (offeredTrack->description().type() == "video" &&
            !ConfigureVideoSender(s, offeredTrack->description().description())) {
            Logger::Error("Failed to configure the negotiated video sender");
        }
    });

    return s;
}

bool WebRtcTransport::ConfigureVideoSender(
    const std::shared_ptr<Session>& s,
    const std::string& offeredVideoSdp) {
    rtc::Description::Media offeredVideo(offeredVideoSdp);
    if (offeredVideo.type() != "video") {
        Logger::Error("Remote viewer track was not a video m-line");
        return false;
    }

    int h264PayloadType = -1;
    for (const int payloadType : offeredVideo.payloadTypes()) {
        const auto* rtpMap = offeredVideo.rtpMap(payloadType);
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
        Logger::Error("Remote viewer did not offer an H.264 video codec");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (s->videoTrack || s->answerStarted) return true;

        // Preserve the remote offer's m-line and payload mapping while turning
        // the host's side into a valid H.264 sender.
        rtc::Description::Media answerVideo = offeredVideo;
        answerVideo.setDirection(rtc::Description::Direction::SendOnly);
        answerVideo.addSSRC(kVideoSsrc, "remcote-video");
        s->videoTrack = s->pc->addTrack(answerVideo);
        s->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "remcote-video", h264PayloadType,
            rtc::H264RtpPacketizer::ClockRate);

        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, s->rtpConfig);
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        s->videoTrack->setMediaHandler(packetizer);

        std::weak_ptr<Session> weakSession = s;
        auto callbackFence = s->callbackFence;
        s->videoTrack->onOpen([this, weakSession, callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            auto session = weakSession.lock();
            if (!session) return;
            KeyframeRequest keyframe;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                keyframe = keyframeRequest_;
            }
            Logger::Info("WebRTC video track is active");
            // The first IDR can be produced before SRTP opens. Force a fresh
            // one now so the viewer can decode immediately.
            if (keyframe) keyframe();
        });
    }

    Logger::Infof("Negotiated H.264 video track (payload type %d)", h264PayloadType);
    return true;
}

void WebRtcTransport::WireDataChannel(const std::shared_ptr<Session>& s,
                                      std::shared_ptr<rtc::DataChannel> ch) {
    const std::string label = ch->label();
    Logger::Infof("WebRTC data channel opened: %s", label.c_str());
    auto callbackFence = s->callbackFence;
    if (label == kPointerChannel) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s->pointerCh = ch;
        }
        ch->onMessage(
            [this, callbackFence](rtc::binary data) {
                auto lease = callbackFence->TryEnter();
                if (!lease) return;
                HandlePointerBinary(data);
            },
            [](rtc::string) {});
    } else if (label == kReliableChannel) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s->reliableCh = ch;
        }
        std::weak_ptr<Session> weak = s;
        ch->onMessage(
            [](rtc::binary) {},
            [this, weak, callbackFence](rtc::string text) {
                auto lease = callbackFence->TryEnter();
                if (!lease) return;
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
        try {
            rtc::Description offer(payload.value("sdp", ""), "offer");
            s->pc->setRemoteDescription(offer);
            const rtc::Description::Media* offeredVideo = nullptr;
            for (int index = 0; index < offer.mediaCount(); ++index) {
                const auto entry = offer.media(index);
                const auto* media = std::get_if<const rtc::Description::Media*>(&entry);
                if (media && *media && (*media)->type() == "video") {
                    offeredVideo = *media;
                    break;
                }
            }
            if (!offeredVideo) {
                Logger::Error("Remote viewer offer did not contain a video m-line");
                return;
            }
            if (!ConfigureVideoSender(s, offeredVideo->description())) return;
            s->pc->setLocalDescription();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                s->answerStarted = true;
            }
            Logger::Info("Generated local WebRTC answer");
        } catch (const std::exception& e) {
            Logger::Errorf("WebRTC answer generation failed: %s", e.what());
        }
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
    struct ActiveVideoTrack {
        std::shared_ptr<rtc::Track> track;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
    };
    std::vector<ActiveVideoTrack> tracks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, s] : sessions_) {
            // Track::onOpen is not guaranteed to fire for a locally-created
            // sendonly track. isOpen() is the authoritative readiness check.
            if (s->videoTrack && s->videoTrack->isOpen()) {
                tracks.push_back({s->videoTrack, s->rtpConfig});
            }
        }
    }
    if (tracks.empty()) return;
    // Present timestamp in 90 kHz clock units.
    const uint32_t rtpTs = static_cast<uint32_t>(frame.captureUs * 90 / 1000);
    for (const auto& active : tracks) {
        if (active.rtpConfig) active.rtpConfig->timestamp = rtpTs;
        try {
            active.track->send(reinterpret_cast<const std::byte*>(frame.data), frame.size);
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
    if (!s) return;
    s->callbackFence->StopAccepting();
    if (s->pc) s->pc->close();
    s->callbackFence->WaitForIdle();
}

void WebRtcTransport::CloseAll() {
    std::map<std::string, std::shared_ptr<Session>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy.swap(sessions_);
    }
    for (auto& [id, s] : copy) {
        s->callbackFence->StopAccepting();
    }
    for (auto& [id, s] : copy) {
        if (s->pc) s->pc->close();
    }
    for (auto& [id, s] : copy) {
        s->callbackFence->WaitForIdle();
    }
}

} // namespace remcote
