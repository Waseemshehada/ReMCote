#pragma once
// ReMCote Host — shared types and constants.
// Protocol must stay in lockstep with lib/remcote-protocol (see docs/protocol.md).

#include <cstdint>
#include <functional>
#include <string>

namespace remcote {

constexpr const char* kAppName = "ReMCote Host";
constexpr const char* kPointerChannel = "input-pointer";
constexpr const char* kReliableChannel = "input-reliable";
constexpr uint8_t kPointerMoveType = 1;

// There is NO default signaling endpoint. The URL must be provided via the
// REMCOTE_SIGNALING_URL (or REMCOTE_SERVER) env var, or a remcote-server.txt
// file next to the exe, e.g. wss://your-app.replit.app/api/ws
// The host refuses to start with a fabricated/placeholder URL.

struct HostCapabilities {
    bool h264 = true;
    bool hevc = false;
    bool av1 = false;
    int maxWidth = 0;
    int maxHeight = 0;
    int maxFps = 0;
    std::string gpuName;
    std::string encoderName;
    int desktopWidth = 0;
    int desktopHeight = 0;
    int desktopHz = 0;
};

// Full ICE server configuration including optional TURN credentials.
struct IceServerCfg {
    std::string url;
    std::string username;   // empty for STUN-only entries
    std::string credential; // empty for STUN-only entries
};

// One encoded H.264 access unit (Annex-B), ready for RTP packetization.
struct EncodedFrame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;
    // Capture timestamp, QueryPerformanceCounter-derived microseconds.
    int64_t captureUs = 0;
    int64_t encodeDurationUs = 0;
};

int64_t NowUs();

} // namespace remcote
