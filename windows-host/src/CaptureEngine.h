#pragma once
// DXGI Desktop Duplication capture. GPU textures only — frames are never
// copied to CPU memory (spec §8). Newest frame wins: the engine holds at most
// one pending frame (spec §9).

#include <atomic>
#include <functional>
#include <thread>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "Common.h"

namespace remcote {

class CaptureEngine {
public:
    // Called on the capture thread with the latest desktop texture (BGRA,
    // default-usage, GPU-resident). The callee must submit it to the encoder
    // synchronously (the encoder copies into its own registered resource).
    using FrameCallback = std::function<void(ID3D11Texture2D* frame, int64_t captureUs)>;
    // Cursor metadata, sent separately from video when available (spec §20).
    using CursorCallback = std::function<void(float x, float y, bool visible)>;

    // Creates a D3D11 device + IDXGIOutputDuplication for the primary output.
    // NVIDIA adapters are preferred so the captured texture can be passed
    // directly to NVENC on dual-GPU Windows laptops.
    bool Initialize();
    void Start(FrameCallback onFrame, CursorCallback onCursor);
    void Stop();

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    int Width() const { return width_; }
    int Height() const { return height_; }
    int RefreshHz() const { return refreshHz_; }
    std::string GpuName() const { return gpuName_; }
    bool IsNvidiaAdapter() const { return adapterVendorId_ == 0x10DE; }

private:
    void CaptureLoop();
    bool Reinitialize(); // display mode change / access lost recovery

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingFrame_; // single-slot latest frame
    FrameCallback onFrame_;
    CursorCallback onCursor_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int width_ = 0;
    int height_ = 0;
    int refreshHz_ = 0;
    std::string gpuName_;
    unsigned int adapterVendorId_ = 0;
};

} // namespace remcote
