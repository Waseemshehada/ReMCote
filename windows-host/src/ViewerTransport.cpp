// ViewerTransport — native Windows viewer WebRTC offerer.
// See ViewerTransport.h for the design overview.
//
// Key role difference from WebRtcTransport (host answerer):
//   • We are the OFFERER.  We create the PeerConnection, add the two data
//     channels and a recvonly H.264 video transceiver, then call
//     setLocalDescription() to generate the SDP offer.  The host answers.
//   • On the received Track we attach an H264RtpDepacketizer so that
//     onMessage delivers complete Annex-B access units to OnVideoFrame.
//
// Input wire format (spec — lib/remcote-protocol/src/index.ts):
//   input-pointer  binary 9 B:  u8 type(=1) | f32 x LE | f32 y LE
//   input-reliable JSON objects: {t:"mb"/"wheel"/"kb"/"ping"/"keyframe"/…}

#include "ViewerTransport.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

using nlohmann::json;

namespace remcote {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ViewerTransport::ViewerTransport(std::vector<IceServerCfg> iceServers)
    : iceServers_(std::move(iceServers))
{
    Logger::Infof("[ViewerTransport] created with %zu ICE server(s)",
                  iceServers_.size());
}

ViewerTransport::~ViewerTransport() {
    Close();
}

// ---------------------------------------------------------------------------
// Lifecycle — Open / Close
// ---------------------------------------------------------------------------

void ViewerTransport::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pc_) {
        Logger::Warning("[ViewerTransport] Open() called more than once — ignored");
        return;
    }

    // ── PeerConnection configuration ────────────────────────────────────────
    rtc::Configuration config;
    // We are the offerer; disable automatic renegotiation so we can control
    // exactly when the offer is generated via setLocalDescription().
    config.disableAutoNegotiation = true;
    // Keep the native H.264 recvonly m-line backed by an SRTP media
    // transport. Without this, SCTP data channels can connect while the
    // media track remains permanently closed.
    config.forceMediaTransport = true;
    for (const auto& ice : iceServers_) {
        rtc::IceServer srv(ice.url);
        if (!ice.username.empty())   srv.username = ice.username;
        if (!ice.credential.empty()) srv.password = ice.credential;
        config.iceServers.push_back(std::move(srv));
    }

    pc_ = std::make_shared<rtc::PeerConnection>(config);
    open_ = true;
    auto callbackFence = callbackFence_;

    // ── onLocalDescription ──────────────────────────────────────────────────
    // Fires when setLocalDescription() has produced the SDP offer (or when
    // ICE trickle restarts generate a new description).
    pc_->onLocalDescription([this, callbackFence](rtc::Description desc) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        const std::string typeStr = desc.typeString(); // "offer" | "answer" | …
        Logger::Debugf("[ViewerTransport] local description ready: %s", typeStr.c_str());
        json payload = {{"kind", typeStr}, {"sdp", std::string(desc)}};
        if (onLocalSdp_) onLocalSdp_(payload);
    });

    // ── onLocalCandidate ────────────────────────────────────────────────────
    pc_->onLocalCandidate([this, callbackFence](rtc::Candidate cand) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        Logger::Debug("[ViewerTransport] local ICE candidate");
        json payload = {
            {"kind",      "candidate"},
            {"candidate", std::string(cand)},
            {"sdpMid",    cand.mid()},
        };
        if (onLocalCandidate_) onLocalCandidate_(payload);
    });

    // ── onStateChange ───────────────────────────────────────────────────────
    pc_->onStateChange([this, callbackFence](rtc::PeerConnection::State state) {
        auto lease = callbackFence->TryEnter();
        if (!lease) return;
        using S = rtc::PeerConnection::State;
        if (state == S::Connected) {
            const ViewerConnectionType ct = DetectConnectionType(pc_);
            connectionType_.store(ct, std::memory_order_relaxed);
            const char* typeStr =
                ct == ViewerConnectionType::Relay  ? "relay" :
                ct == ViewerConnectionType::Direct ? "direct" : "unknown";
            Logger::Infof("[ViewerTransport] WebRTC connected (%s)", typeStr);
            if (onConnected_) onConnected_(true, ct);
        } else if (state == S::Disconnected ||
                   state == S::Failed       ||
                   state == S::Closed) {
            Logger::Warningf("[ViewerTransport] WebRTC state: %s",
                state == S::Disconnected ? "Disconnected" :
                state == S::Failed       ? "Failed"       : "Closed");
            open_ = false;
            if (onConnected_) onConnected_(false, connectionType_.load());
        }
    });

    // ── onGatheringStateChange ───────────────────────────────────────────────
    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            Logger::Debug("[ViewerTransport] ICE gathering complete");
        }
    });

    // ── Data channels: create them ourselves (we are the offerer) ───────────
    //
    // input-pointer  — unreliable, unordered, high-frequency pointer motion.
    // input-reliable — reliable, ordered, buttons / keys / wheel / control.
    {
        rtc::DataChannelInit pointerInit;
        pointerInit.reliability.unordered = true;
        pointerInit.reliability.maxRetransmits = 0;

        pointerCh_ = pc_->createDataChannel(kPointerChannel, pointerInit);
        pointerCh_->onOpen([this, callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Info("[ViewerTransport] input-pointer channel open");
            HandlePointerChannelOpen();
        });
        pointerCh_->onClosed([callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Warning("[ViewerTransport] input-pointer channel closed");
        });
        // We only send on this channel; incoming binary is ignored.
        pointerCh_->onMessage([](rtc::binary) {}, [](rtc::string) {});
    }
    {
        rtc::DataChannelInit reliableInit;
        // Default init is reliable + ordered — exactly what we need.
        reliableInit.negotiated = false;

        reliableCh_ = pc_->createDataChannel(kReliableChannel, reliableInit);
        reliableCh_->onOpen([callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Info("[ViewerTransport] input-reliable channel open");
        });
        reliableCh_->onClosed([callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Warning("[ViewerTransport] input-reliable channel closed");
        });
        reliableCh_->onMessage(
            [](rtc::binary) {}, // no binary on reliable channel
            [this, callbackFence](rtc::string text) {
                auto lease = callbackFence->TryEnter();
                if (!lease) return;
                HandleReliableText(text);
            });
    }

    // ── Video transceiver: recvonly H.264 ───────────────────────────────────
    //
    // Build an H.264 recvonly media description for the offer.
    // We pick payload type 96 — the most common Chrome assignment for H.264.
    // libdatachannel will honour whatever PT the host answers with when it
    // sets the remote description, so the exact value here only affects the
    // offer SDP; the host's selected PT takes effect after setRemoteDescription.
    {
        // Construct a video media description.  "recvonly" signals to the host
        // that this side only wants to receive, causing the host to answer
        // "sendonly" and attach its NVENC track.
        rtc::Description::Video videoDesc("video", rtc::Description::Direction::RecvOnly);
        // Advertise the same constrained-baseline Level 4.2 profile emitted
        // by the NVENC host. addH264Codec expects a complete fmtp string.
        videoDesc.addH264Codec(
            96,
            "profile-level-id=42e02a;packetization-mode=1;"
            "level-asymmetry-allowed=1");

        videoTrack_ = pc_->addTrack(videoDesc);

        // Attach the H.264 RTP depacketizer so onMessage receives complete
        // Annex-B access units rather than raw RTP payloads.
        auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        videoTrack_->setMediaHandler(depacketizer);

        videoTrack_->onOpen([callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Info("[ViewerTransport] video track open — H.264 stream active");
        });
        videoTrack_->onClosed([callbackFence] {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            Logger::Warning("[ViewerTransport] video track closed");
        });

        // onMessage delivers reassembled Annex-B frames after depacketization.
        videoTrack_->onMessage([this, callbackFence](rtc::binary data) {
            auto lease = callbackFence->TryEnter();
            if (!lease) return;
            if (!onVideoFrame_) return;

            // data is a complete Annex-B access unit at this point.
            // Scan for IDR NAL to set the keyframe flag.
            const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
            const size_t size = data.size();

            bool isKeyframe = false;
            // Walk the Annex-B start codes to detect IDR_SLICE (nal_unit_type == 5).
            size_t i = 0;
            while (i + 4 < size) {
                // Find start code: 0x00 0x00 0x00 0x01 or 0x00 0x00 0x01
                bool found4 = (bytes[i]   == 0x00 && bytes[i+1] == 0x00 &&
                               bytes[i+2] == 0x00 && bytes[i+3] == 0x01);
                bool found3 = (bytes[i]   == 0x00 && bytes[i+1] == 0x00 &&
                               bytes[i+2] == 0x01);
                if (found4) {
                    if (i + 4 < size) {
                        const uint8_t nalType = bytes[i + 4] & 0x1F;
                        if (nalType == 5) { isKeyframe = true; break; }
                    }
                    i += 4;
                } else if (found3) {
                    if (i + 3 < size) {
                        const uint8_t nalType = bytes[i + 3] & 0x1F;
                        if (nalType == 5) { isKeyframe = true; break; }
                    }
                    i += 3;
                } else {
                    ++i;
                }
            }

            ViewerFrame frame;
            frame.data.assign(bytes, bytes + size);
            frame.rtpTimestamp = 0; // libdatachannel depacketizer doesn't
                                    // surface the RTP timestamp directly here;
                                    // use 0 and let the caller track timing.
            frame.keyframe = isKeyframe;
            onVideoFrame_(frame);
        },
        [](rtc::string) {}); // no text on video track
    }

    // ── Generate the SDP offer ───────────────────────────────────────────────
    // setLocalDescription() with no argument generates an offer when the PC
    // has no remote description yet.  The onLocalDescription callback fires.
    try {
        pc_->setLocalDescription();
        Logger::Info("[ViewerTransport] SDP offer generated");
    } catch (const std::exception& e) {
        Logger::Errorf("[ViewerTransport] offer generation failed: %s", e.what());
        if (onDiagnostic_) onDiagnostic_(std::string("Offer generation failed: ") + e.what());
    }
}

void ViewerTransport::Close() {
    callbackFence_->StopAccepting();
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pc_) {
            callbackFence_->WaitForIdle();
            return;
        }
        pc = std::move(pc_);
        videoTrack_.reset();
        pointerCh_.reset();
        reliableCh_.reset();
        open_ = false;
    }
    try {
        pc->close();
    } catch (...) {}
    callbackFence_->WaitForIdle();
    Logger::Info("[ViewerTransport] peer connection closed");
}

// ---------------------------------------------------------------------------
// Inbound signaling
// ---------------------------------------------------------------------------

void ViewerTransport::HandleAnswer(const nlohmann::json& payload) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) {
        Logger::Warning("[ViewerTransport] HandleAnswer: no active connection");
        return;
    }
    const std::string sdp = payload.value("sdp", "");
    if (sdp.empty()) {
        Logger::Warning("[ViewerTransport] HandleAnswer: empty SDP");
        return;
    }
    try {
        pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        Logger::Info("[ViewerTransport] remote answer applied");
    } catch (const std::exception& e) {
        Logger::Errorf("[ViewerTransport] setRemoteDescription(answer) failed: %s", e.what());
        if (onDiagnostic_) onDiagnostic_(std::string("Answer rejected: ") + e.what());
    }
}

void ViewerTransport::HandleRemoteCandidate(const nlohmann::json& payload) {
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pc = pc_;
    }
    if (!pc) return;
    const std::string cand = payload.value("candidate", "");
    const std::string mid  = payload.value("sdpMid", "0");
    if (cand.empty()) return;
    try {
        pc->addRemoteCandidate(rtc::Candidate(cand, mid));
        Logger::Debug("[ViewerTransport] remote ICE candidate accepted");
    } catch (const std::exception& e) {
        Logger::Warningf("[ViewerTransport] addRemoteCandidate failed: %s", e.what());
    }
}

// ---------------------------------------------------------------------------
// Input sending
// ---------------------------------------------------------------------------

void ViewerTransport::SendPointerMove(float x, float y) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = pointerCh_;
    }
    if (!ch || !ch->isOpen()) return;

    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    // 9-byte binary: u8 type(=1) | f32 x LE | f32 y LE  (spec §15)
    uint8_t buf[9];
    buf[0] = kPointerMoveType;
    std::memcpy(buf + 1, &x, 4);
    std::memcpy(buf + 5, &y, 4);
    try {
        ch->send(reinterpret_cast<const std::byte*>(buf), sizeof(buf));
    } catch (...) {}
}

void ViewerTransport::SendMouseButton(int button, bool down, float x, float y) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = reliableCh_;
    }
    if (!ch || !ch->isOpen()) return;
    json msg = {
        {"t", "mb"},
        {"b", button},
        {"d", down},
        {"x", std::clamp(x, 0.0f, 1.0f)},
        {"y", std::clamp(y, 0.0f, 1.0f)},
    };
    try {
        ch->send(msg.dump());
    } catch (...) {}
}

void ViewerTransport::SendWheel(float dx, float dy, float x, float y) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = reliableCh_;
    }
    if (!ch || !ch->isOpen()) return;
    json msg = {
        {"t", "wheel"},
        {"dx", dx},
        {"dy", dy},
        {"x", std::clamp(x, 0.0f, 1.0f)},
        {"y", std::clamp(y, 0.0f, 1.0f)},
    };
    try {
        ch->send(msg.dump());
    } catch (...) {}
}

void ViewerTransport::SendKey(const std::string& code, uint32_t scanCode, bool down) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = reliableCh_;
    }
    if (!ch || !ch->isOpen()) return;
    json msg = {{"t", "kb"}, {"code", code}, {"sc", scanCode}, {"d", down}};
    try {
        ch->send(msg.dump());
    } catch (...) {}
}

void ViewerTransport::SendPing(double ts) {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = reliableCh_;
    }
    if (!ch || !ch->isOpen()) return;
    json msg = {{"t", "ping"}, {"ts", ts}};
    try {
        ch->send(msg.dump());
    } catch (...) {}
}

void ViewerTransport::SendKeyframeRequest() {
    std::shared_ptr<rtc::DataChannel> ch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ch = reliableCh_;
    }
    if (!ch || !ch->isOpen()) return;
    json msg = {{"t", "keyframe"}};
    try {
        ch->send(msg.dump());
    } catch (...) {}
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool ViewerTransport::IsOpen() const {
    return open_.load(std::memory_order_relaxed);
}

ViewerConnectionType ViewerTransport::ConnectionType() const {
    return connectionType_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void ViewerTransport::HandlePointerChannelOpen() {
    // Currently no action needed on open; we begin sending on demand.
    // A future enhancement could send a "ready" control message here.
}

void ViewerTransport::HandleReliableText(const std::string& text) {
    // The host sends cursor metadata on input-reliable.
    json msg;
    try {
        msg = json::parse(text);
    } catch (...) {
        return;
    }
    if (!msg.is_object()) return;

    const std::string t = msg.value("t", "");

    if (t == "cursor") {
        // {t:"cursor", x:f, y:f, visible:bool}  — host → viewer cursor overlay
        const float x       = msg.value("x", 0.0f);
        const float y       = msg.value("y", 0.0f);
        const bool visible  = msg.value("visible", true);
        if (onCursorUpdate_) onCursorUpdate_(x, y, visible);

    } else if (t == "pong") {
        // {t:"pong", ts:number} — host echo of our ping
        const double ts = msg.value("ts", 0.0);
        if (onPong_) onPong_(ts);

    } else {
        Logger::Debugf("[ViewerTransport] ignored reliable message: t=%s", t.c_str());
    }
}

// Attempt to detect relay vs direct from the selected ICE candidate pair.
ViewerConnectionType ViewerTransport::DetectConnectionType(
    const std::shared_ptr<rtc::PeerConnection>& pc)
{
    if (!pc) return ViewerConnectionType::Unknown;

    try {
        rtc::Candidate local;
        rtc::Candidate remote;
        if (pc->getSelectedCandidatePair(&local, &remote)) {
            if (local.type() == rtc::Candidate::Type::Relayed ||
                remote.type() == rtc::Candidate::Type::Relayed) {
                return ViewerConnectionType::Relay;
            }
            return ViewerConnectionType::Direct;
        }
    } catch (...) {
        // getSelectedCandidatePair() may throw if ICE hasn't completed yet.
    }
    return ViewerConnectionType::Unknown;
}

} // namespace remcote
