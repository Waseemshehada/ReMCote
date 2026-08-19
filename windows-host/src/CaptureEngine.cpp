#include "CaptureEngine.h"
#include "Logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace remcote {

int64_t NowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1'000'000 / freq.QuadPart;
}

bool CaptureEngine::Initialize() {
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        device_.GetAddressOf(), &level, context_.GetAddressOf());
    if (FAILED(hr)) {
        Logger::Errorf("Capture: D3D11CreateDevice failed (0x%08lx)", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    device_.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(adapter.GetAddressOf());

    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    char name[256]{};
    wcstombs(name, adapterDesc.Description, sizeof(name) - 1);
    gpuName_ = name;

    // Primary monitor only for MVP (spec §8).
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
        Logger::Error("Capture: no primary display output was found");
        return false;
    }
    ComPtr<IDXGIOutput1> output1;
    output.As(&output1);

    HRESULT dupHr = output1->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    if (FAILED(dupHr)) {
        Logger::Errorf("Capture: DuplicateOutput failed (0x%08lx)", dupHr);
        return false;
    }

    DXGI_OUTDUPL_DESC dd{};
    duplication_->GetDesc(&dd);
    width_ = static_cast<int>(dd.ModeDesc.Width);
    height_ = static_cast<int>(dd.ModeDesc.Height);
    refreshHz_ = dd.ModeDesc.RefreshRate.Denominator
        ? dd.ModeDesc.RefreshRate.Numerator / dd.ModeDesc.RefreshRate.Denominator
        : 60;

    // Single reusable GPU texture — our "queue" of depth 1 (spec §9).
    D3D11_TEXTURE2D_DESC td{};
    td.Width = dd.ModeDesc.Width;
    td.Height = dd.ModeDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, stagingFrame_.GetAddressOf()))) {
        Logger::Error("Capture: GPU frame texture creation failed");
        return false;
    }

    Logger::Infof("Capture initialized: %dx%d @ %d Hz on %s",
                  width_, height_, refreshHz_, gpuName_.c_str());
    return true;
}

void CaptureEngine::Start(FrameCallback onFrame, CursorCallback onCursor) {
    onFrame_ = std::move(onFrame);
    onCursor_ = std::move(onCursor);
    running_ = true;
    thread_ = std::thread([this] { CaptureLoop(); });
}

void CaptureEngine::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool CaptureEngine::Reinitialize() {
    duplication_.Reset();
    stagingFrame_.Reset();
    context_.Reset();
    device_.Reset();
    return Initialize();
}

void CaptureEngine::CaptureLoop() {
    // Capture thread priority just below input (spec §19).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    while (running_) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(16, &info, resource.GetAddressOf());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue; // static desktop — nothing new, nothing stale to send
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // Display mode changed / desktop switched — rebuild duplication.
            Logger::Warning("Capture access lost; reinitializing desktop duplication");
            if (!Reinitialize()) {
                Sleep(1000);
            }
            continue;
        }
        if (FAILED(hr)) {
            Logger::Errorf("Capture: AcquireNextFrame failed (0x%08lx)", hr);
            Sleep(100);
            continue;
        }

        const int64_t captureUs = NowUs();

        // Cursor metadata travels separately from video (spec §20).
        if (info.LastMouseUpdateTime.QuadPart != 0 && onCursor_ && width_ > 0 && height_ > 0) {
            const bool visible = info.PointerPosition.Visible != FALSE;
            const float cx = static_cast<float>(info.PointerPosition.Position.x) / width_;
            const float cy = static_cast<float>(info.PointerPosition.Position.y) / height_;
            onCursor_(cx, cy, visible);
        }

        if (info.LastPresentTime.QuadPart != 0) {
            // GPU-to-GPU copy into our single-slot texture. No CPU roundtrip.
            ComPtr<ID3D11Texture2D> desktopTex;
            resource.As(&desktopTex);
            context_->CopyResource(stagingFrame_.Get(), desktopTex.Get());
            // Synchronous hand-off: the encoder submits this texture to NVENC
            // before we release the duplication frame. If the encoder is busy
            // it drops the frame — never queues it (spec §9).
            if (onFrame_) onFrame_(stagingFrame_.Get(), captureUs);
        }

        duplication_->ReleaseFrame();
    }
}

} // namespace remcote
