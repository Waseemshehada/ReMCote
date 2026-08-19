#include "CaptureEngine.h"
#include "Logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

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
    constexpr UINT kNvidiaVendorId = 0x10DE;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        Logger::Errorf("Capture: CreateDXGIFactory1 failed (0x%08lx)", hr);
        return false;
    }

    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enumResult == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enumResult)) continue;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        // Put NVIDIA first. NVENC requires that the D3D11 texture belongs to
        // the NVIDIA device, which matters on Optimus/hybrid GPU laptops.
        if (desc.VendorId == kNvidiaVendorId) adapters.insert(adapters.begin(), adapter);
        else adapters.push_back(adapter);
    }

    for (const auto& adapter : adapters) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name[256]{};
        wcstombs_s(nullptr, name, sizeof(name), desc.Description, sizeof(name) - 1);

        ComPtr<ID3D11Device> candidateDevice;
        ComPtr<ID3D11DeviceContext> candidateContext;
        D3D_FEATURE_LEVEL level{};
        const HRESULT deviceResult = D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION,
            candidateDevice.GetAddressOf(), &level, candidateContext.GetAddressOf());
        if (FAILED(deviceResult)) {
            Logger::Warningf("Capture: D3D11 device failed on %s (0x%08lx)",
                             name, deviceResult);
            continue;
        }

        // Primary monitor only for MVP. A hybrid laptop may expose it only on
        // one adapter, so keep trying adapters instead of accepting the default.
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
            Logger::Infof("Capture: no display output on %s", name);
            continue;
        }
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) continue;

        ComPtr<IDXGIOutputDuplication> candidateDuplication;
        const HRESULT duplicationResult =
            output1->DuplicateOutput(candidateDevice.Get(), candidateDuplication.GetAddressOf());
        if (FAILED(duplicationResult)) {
            Logger::Warningf("Capture: DuplicateOutput failed on %s (0x%08lx)",
                             name, duplicationResult);
            continue;
        }

        DXGI_OUTDUPL_DESC dd{};
        candidateDuplication->GetDesc(&dd);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> candidateFrame;
        if (FAILED(candidateDevice->CreateTexture2D(
                &td, nullptr, candidateFrame.GetAddressOf()))) {
            Logger::Warningf("Capture: GPU frame texture failed on %s", name);
            continue;
        }

        device_ = candidateDevice;
        context_ = candidateContext;
        duplication_ = candidateDuplication;
        stagingFrame_ = candidateFrame;
        gpuName_ = name;
        adapterVendorId_ = desc.VendorId;
        width_ = static_cast<int>(dd.ModeDesc.Width);
        height_ = static_cast<int>(dd.ModeDesc.Height);
        refreshHz_ = dd.ModeDesc.RefreshRate.Denominator
            ? dd.ModeDesc.RefreshRate.Numerator / dd.ModeDesc.RefreshRate.Denominator
            : 60;
        Logger::Infof("Capture selected adapter: %s (vendor 0x%04X)",
                      gpuName_.c_str(), adapterVendorId_);
        Logger::Infof("Capture initialized: %dx%d @ %d Hz on %s",
                      width_, height_, refreshHz_, gpuName_.c_str());
        return true;
    }

    Logger::Error("Capture: no display adapter could initialize desktop duplication");
    return false;
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
    adapterVendorId_ = 0;
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
