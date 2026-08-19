#include "PerformanceMonitor.h"

#include "Common.h"

namespace remcote {

void PerformanceMonitor::OnCaptureFrame(int64_t captureDurationUs) {
    capturedFrames_.fetch_add(1, std::memory_order_relaxed);
    lastCaptureUs_.store(captureDurationUs, std::memory_order_relaxed);
}

void PerformanceMonitor::OnEncodeFrame(int64_t encodeDurationUs) {
    encodedFrames_.fetch_add(1, std::memory_order_relaxed);
    lastEncodeUs_.store(encodeDurationUs, std::memory_order_relaxed);
}

void PerformanceMonitor::OnFrameDropped() {
    droppedFrames_.fetch_add(1, std::memory_order_relaxed);
}

PerformanceMonitor::Snapshot PerformanceMonitor::Sample() {
    const int64_t now = NowUs();
    const uint64_t captured = capturedFrames_.load(std::memory_order_relaxed);
    const uint64_t encoded = encodedFrames_.load(std::memory_order_relaxed);

    Snapshot snap;
    snap.captureMs = lastCaptureUs_.load(std::memory_order_relaxed) / 1000.0;
    snap.encodeMs = lastEncodeUs_.load(std::memory_order_relaxed) / 1000.0;
    snap.framesDropped = droppedFrames_.load(std::memory_order_relaxed);

    if (lastSampleUs_ > 0) {
        const double dtSec = (now - lastSampleUs_) / 1'000'000.0;
        if (dtSec > 0) {
            snap.captureFps = static_cast<int>((captured - lastCaptured_) / dtSec + 0.5);
            snap.encodeFps = static_cast<int>((encoded - lastEncoded_) / dtSec + 0.5);
        }
    }
    lastCaptured_ = captured;
    lastEncoded_ = encoded;
    lastSampleUs_ = now;
    return snap;
}

} // namespace remcote
