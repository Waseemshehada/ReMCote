#pragma once
// NVENC H.264 hardware encoder, tuned for interactive ultra-low latency
// (spec §10): ultra-low-latency tuning, CBR, no B-frames, infinite GOP with
// on-demand IDR, and an encode queue depth of ONE — a busy encoder drops
// incoming frames instead of buffering them.

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

#include "Common.h"

// NV_ENCODE_API_FUNCTION_LIST is a typedef of an anonymous struct in nvEncodeAPI.h
// and cannot be forward-declared. Keep it as void* here; cast in the .cpp.

namespace remcote {

struct EncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    int bitrateKbps = 12000; // adaptive; ceiling raised for high motion
    int maxBitrateKbps = 24000;
};

class EncoderEngine {
public:
    using OutputCallback = std::function<void(const EncodedFrame&)>;

    // device: the SAME D3D11 device the capture engine uses, so desktop
    // textures go straight into NVENC without any copies through system RAM.
    bool Initialize(ID3D11Device* device, const EncoderConfig& config);
    void Shutdown();

    // Called from the capture thread. Returns false if the frame was dropped
    // because an encode is already in flight (newest-frame-wins).
    bool SubmitFrame(ID3D11Texture2D* frame, int64_t captureUs);

    void RequestKeyframe() { forceIdr_ = true; }
    void SetBitrate(int kbps); // live reconfigure, no session restart
    void SetOutputCallback(OutputCallback cb) { onOutput_ = std::move(cb); }

    std::string Name() const { return "NVENC H.264"; }

private:
    void* encoder_ = nullptr;                     // NVENC session handle
    void* api_ = nullptr;                         // NV_ENCODE_API_FUNCTION_LIST*, cast in .cpp
    void* inputResource_ = nullptr;               // registered D3D11 texture
    void* bitstreamBuffer_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTexture_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    EncoderConfig config_{};
    OutputCallback onOutput_;
    std::atomic<bool> busy_{false};   // the 1-frame "queue"
    std::atomic<bool> forceIdr_{true};
    std::mutex reconfigureMutex_;
    void* hModule_ = nullptr; // nvEncodeAPI64.dll
};

} // namespace remcote
