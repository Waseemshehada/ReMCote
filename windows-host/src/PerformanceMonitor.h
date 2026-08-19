#pragma once
// Real host-side telemetry (spec §35). All numbers are measured, never faked.
#include <atomic>
#include <cstdint>
#include <string>

namespace remcote {

class PerformanceMonitor {
public:
    void OnCaptureFrame(int64_t captureDurationUs);
    void OnEncodeFrame(int64_t encodeDurationUs);
    void OnFrameDropped();

    struct Snapshot {
        double captureMs = 0;
        double encodeMs = 0;
        int captureFps = 0;
        int encodeFps = 0;
        uint64_t framesDropped = 0;
    };

    // Computes per-second rates; call roughly once per second from the UI.
    Snapshot Sample();

private:
    std::atomic<uint64_t> capturedFrames_{0};
    std::atomic<uint64_t> encodedFrames_{0};
    std::atomic<uint64_t> droppedFrames_{0};
    std::atomic<int64_t> lastCaptureUs_{0};
    std::atomic<int64_t> lastEncodeUs_{0};
    uint64_t lastCaptured_ = 0;
    uint64_t lastEncoded_ = 0;
    int64_t lastSampleUs_ = 0;
};

} // namespace remcote
