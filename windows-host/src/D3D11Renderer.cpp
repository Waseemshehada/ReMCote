// D3D11Renderer — D3D11/DXGI presentation with hardware video processor scaling
// and a GDI software fallback.  See D3D11Renderer.h for the public contract.

#include "D3D11Renderer.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi.h>
#include <wrl/client.h>

// Link: d3d11.lib dxgi.lib user32.lib gdi32.lib

using Microsoft::WRL::ComPtr;

namespace remcote {

// ─── helpers ─────────────────────────────────────────────────────────────────

namespace {

// Draw a simple crosshair cursor via GDI into the window HDC.
void DrawGdiCursor(HWND hwnd, int cx, int cy) {
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    constexpr int kArm = 10;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 0));
    HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, cx - kArm, cy, nullptr); LineTo(hdc, cx + kArm, cy);
    MoveToEx(hdc, cx, cy - kArm, nullptr); LineTo(hdc, cx, cy + kArm);
    SelectObject(hdc, old);
    DeleteObject(pen);
    ReleaseDC(hwnd, hdc);
}

} // namespace

// ─── Initialize ──────────────────────────────────────────────────────────────

bool D3D11Renderer::Initialize(HWND hwnd) {
    std::lock_guard<std::mutex> lk(presentMutex_);

    if (initialized_ && hwnd_ == hwnd) {
        return true;
    }

    hwnd_  = hwnd;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    clientW_ = rc.right  - rc.left;
    clientH_ = rc.bottom - rc.top;
    if (clientW_ <= 0) clientW_ = 1;
    if (clientH_ <= 0) clientH_ = 1;

    // ── Create D3D11 device ───────────────────────────────────────────────────
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1,
                                    D3D_FEATURE_LEVEL_11_0,
                                    D3D_FEATURE_LEVEL_10_1,
                                    D3D_FEATURE_LEVEL_10_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr,                // default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &device_, &featureLevel, &context_);

    if (FAILED(hr)) {
        // Retry without video-support flag (some adapters reject it).
        Logger::Warningf("D3D11Renderer: device with VIDEO_SUPPORT failed (0x%08X), retrying", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                               flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION,
                               &device_, &featureLevel, &context_);
    }
    if (FAILED(hr)) {
        Logger::Errorf("D3D11Renderer: D3D11CreateDevice failed: 0x%08X", hr);
        return false;
    }

    // ── Create DXGI swap chain ────────────────────────────────────────────────
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) { Logger::Errorf("D3D11Renderer: QueryInterface IDXGIDevice1 failed"); return false; }

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { Logger::Errorf("D3D11Renderer: GetParent IDXGIFactory2 failed"); return false; }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width  = static_cast<UINT>(clientW_);
    scd.Height = static_cast<UINT>(clientH_);
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count   = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Windows 10+
    scd.Flags       = 0;
    scd.Scaling     = DXGI_SCALING_STRETCH;
    scd.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd,
                                          &scd, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) {
        // Fall back to DXGI_SWAP_EFFECT_DISCARD (older drivers / WARP).
        Logger::Warningf("D3D11Renderer: FLIP_DISCARD swap chain failed (0x%08X), trying DISCARD", hr);
        scd.SwapEffect  = DXGI_SWAP_EFFECT_DISCARD;
        scd.BufferCount = 1;
        hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd,
                                              &scd, nullptr, nullptr, &swapChain_);
    }
    if (FAILED(hr)) {
        Logger::Errorf("D3D11Renderer: CreateSwapChainForHwnd failed: 0x%08X", hr);
        return false;
    }

    // Disable alt-enter full-screen toggle (we handle our own resize).
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if (!CreateRenderTarget()) return false;

    // ── Try to acquire video processor interfaces ─────────────────────────────
    if (SUCCEEDED(device_.As(&videoDevice_)) &&
        SUCCEEDED(context_.As(&videoContext_))) {
        useVideoProcessor_ = true;
        Logger::Info("D3D11Renderer: D3D11 video processor available");
    } else {
        useVideoProcessor_ = false;
        Logger::Warning("D3D11Renderer: D3D11 video processor unavailable; using GDI fallback");
    }

    initialized_ = true;
    Logger::Infof("D3D11Renderer: initialized %dx%d", clientW_, clientH_);
    return true;
}

// ─── CreateRenderTarget ───────────────────────────────────────────────────────

bool D3D11Renderer::CreateRenderTarget() {
    rtv_.Reset();
    ComPtr<ID3D11Texture2D> backBuf;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    if (FAILED(hr)) {
        Logger::Errorf("D3D11Renderer: GetBuffer failed: 0x%08X", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(backBuf.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
        Logger::Errorf("D3D11Renderer: CreateRenderTargetView failed: 0x%08X", hr);
        return false;
    }
    return true;
}

// ─── Resize ──────────────────────────────────────────────────────────────────

void D3D11Renderer::Resize() {
    std::lock_guard<std::mutex> lk(presentMutex_);
    if (!initialized_) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int newW = rc.right  - rc.left;
    int newH = rc.bottom - rc.top;
    if (newW <= 0) newW = 1;
    if (newH <= 0) newH = 1;
    if (newW == clientW_ && newH == clientH_) return;

    clientW_ = newW;
    clientH_ = newH;

    // Must release all references to swap-chain buffers before ResizeBuffers.
    rtv_.Reset();
    vpOutputView_.Reset();
    context_->ClearState();

    HRESULT hr = swapChain_->ResizeBuffers(0,
                                            static_cast<UINT>(clientW_),
                                            static_cast<UINT>(clientH_),
                                            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        Logger::Errorf("D3D11Renderer: ResizeBuffers failed: 0x%08X", hr);
        return;
    }
    CreateRenderTarget();

    // Re-create video processor output view for new back-buffer dimensions.
    if (useVideoProcessor_ && vpSrcWidth_ > 0) {
        CreateVideoProcessor(vpSrcWidth_, vpSrcHeight_);
    } else if (!fallbackFrame_.empty()) {
        PresentViaGdi(
            fallbackFrame_.data(),
            fallbackWidth_,
            fallbackHeight_,
            fallbackStride_);
    }

    Logger::Infof("D3D11Renderer: resized to %dx%d", clientW_, clientH_);
}

// ─── CreateVideoProcessor ─────────────────────────────────────────────────────

bool D3D11Renderer::CreateVideoProcessor(int width, int height) {
    // Called with presentMutex_ held.
    vpEnum_.Reset();
    vp_.Reset();
    vpInputView_.Reset();
    vpOutputView_.Reset();
    srcTex_.Reset();
    stagingTex_.Reset();

    // Content description: BGRA source, same-size output (scaling done by VP).
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc{};
    vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vpDesc.InputWidth  = static_cast<UINT>(width);
    vpDesc.InputHeight = static_cast<UINT>(height);
    vpDesc.OutputWidth  = static_cast<UINT>(clientW_);
    vpDesc.OutputHeight = static_cast<UINT>(clientH_);
    vpDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&vpDesc, &vpEnum_);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateVideoProcessorEnumerator failed: 0x%08X", hr);
        return false;
    }

    // Verify BGRA is supported as input.
    UINT formatFlags = 0;
    hr = vpEnum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &formatFlags);
    if (FAILED(hr) || !(formatFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        Logger::Warning("D3D11Renderer: VP does not support BGRA input; using GDI fallback");
        useVideoProcessor_ = false;
        return false;
    }

    hr = videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateVideoProcessor failed: 0x%08X", hr);
        return false;
    }

    // Source texture: DEFAULT usage, BGRA, shader resource + video.
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width  = static_cast<UINT>(width);
    texDesc.Height = static_cast<UINT>(height);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage     = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_VIDEO_ENCODER |
                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = device_->CreateTexture2D(&texDesc, nullptr, &srcTex_);
    if (FAILED(hr)) {
        // Try without decoder/encoder bind flags.
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&texDesc, nullptr, &srcTex_);
    }
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateTexture2D (src) failed: 0x%08X", hr);
        return false;
    }

    // Staging texture: CPU-writable upload buffer.
    texDesc.Usage          = D3D11_USAGE_STAGING;
    texDesc.BindFlags      = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateTexture2D(&texDesc, nullptr, &stagingTex_);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateTexture2D (staging) failed: 0x%08X", hr);
        return false;
    }

    // VP input view.
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.FourCC = 0;
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorInputView(srcTex_.Get(), vpEnum_.Get(), &ivDesc, &vpInputView_);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateVideoProcessorInputView failed: 0x%08X", hr);
        return false;
    }

    // VP output view on the swap chain back buffer.
    ComPtr<ID3D11Texture2D> backBuf;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovDesc.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(backBuf.Get(), vpEnum_.Get(), &ovDesc, &vpOutputView_);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: CreateVideoProcessorOutputView failed: 0x%08X", hr);
        return false;
    }

    vpSrcWidth_  = width;
    vpSrcHeight_ = height;
    return true;
}

// ─── UploadBgra ──────────────────────────────────────────────────────────────

bool D3D11Renderer::UploadBgra(const uint8_t* data, int width, int height, int stride) {
    // Map staging texture.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context_->Map(stagingTex_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: Map staging failed: 0x%08X", hr);
        return false;
    }
    // Copy rows, handling stride differences.
    const int rowBytes = width * 4;
    for (int row = 0; row < height; ++row) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                    data + row * stride,
                    rowBytes);
    }
    context_->Unmap(stagingTex_.Get(), 0);

    // Copy staging → default (GPU-resident) texture.
    context_->CopyResource(srcTex_.Get(), stagingTex_.Get());
    return true;
}

// ─── PresentViaVideoProcessor ─────────────────────────────────────────────────

void D3D11Renderer::PresentViaVideoProcessor(int width, int height) {
    // Compute letterbox destination rect.
    RECT dst = LetterboxRect(clientW_, clientH_, width, height);
    {
        std::lock_guard<std::mutex> lk(rectMutex_);
        videoRect_ = dst;
    }

    // Configure VP stream: source rect = full texture, dest rect = letterbox.
    RECT src{ 0, 0, width, height };
    RECT out{ 0, 0, clientW_, clientH_ };
    videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    videoContext_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &out);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable          = TRUE;
    stream.pInputSurface   = vpInputView_.Get();

    // Clear back buffer to black before blit (letterbox bars).
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    context_->ClearRenderTargetView(rtv_.Get(), black);

    HRESULT hr = videoContext_->VideoProcessorBlt(vp_.Get(), vpOutputView_.Get(),
                                                    0, 1, &stream);
    if (FAILED(hr)) {
        Logger::Warningf("D3D11Renderer: VideoProcessorBlt failed: 0x%08X", hr);
    }
}

// ─── PresentViaGdi ───────────────────────────────────────────────────────────

void D3D11Renderer::PresentViaGdi(const uint8_t* data, int width, int height, int stride) {
    // Last-resort software path. Draw directly to the child HWND because
    // flip-model swap chains cannot expose a GDI DC.
    HDC surfaceDC = GetDC(hwnd_);
    if (!surfaceDC) return;

    // Fill with black.
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT all{ 0, 0, clientW_, clientH_ };
    FillRect(surfaceDC, &all, black);

    // Create a DIB section from the BGRA data.
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibBits = nullptr;
    HDC memDC = CreateCompatibleDC(surfaceDC);
    HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    if (hbm && dibBits) {
        // Copy pixels; DIB stride is DWORD-aligned = width*4 for 32-bit.
        for (int row = 0; row < height; ++row) {
            std::memcpy(static_cast<uint8_t*>(dibBits) + row * width * 4,
                        data + row * stride,
                        width * 4);
        }
        HBITMAP old = static_cast<HBITMAP>(SelectObject(memDC, hbm));

        // Letterbox.
        RECT dst = LetterboxRect(clientW_, clientH_, width, height);
        {
            std::lock_guard<std::mutex> lk(rectMutex_);
            videoRect_ = dst;
        }
        StretchBlt(surfaceDC,
                   dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                   memDC, 0, 0, width, height, SRCCOPY);

        SelectObject(memDC, old);
        DeleteObject(hbm);
    }
    DeleteDC(memDC);
    ReleaseDC(hwnd_, surfaceDC);
}

// ─── PresentBgra ─────────────────────────────────────────────────────────────

void D3D11Renderer::PresentBgra(const uint8_t* data, int width, int height, int stride) {
    if (!data || width <= 0 || height <= 0 || stride <= 0) return;

    std::lock_guard<std::mutex> lk(presentMutex_);
    if (!initialized_) return;

    if (useVideoProcessor_) {
        // (Re)create VP resources when source dimensions change.
        if (width != vpSrcWidth_ || height != vpSrcHeight_) {
            if (!CreateVideoProcessor(width, height)) {
                // VP setup failed; fall back to GDI.
                useVideoProcessor_ = false;
            }
        }
    }

    if (useVideoProcessor_) {
        if (!UploadBgra(data, width, height, stride)) {
            useVideoProcessor_ = false; // next frame will use GDI
        } else {
            PresentViaVideoProcessor(width, height);
        }
    }

    const bool presentedWithVideoProcessor = useVideoProcessor_;
    if (!presentedWithVideoProcessor) {
        fallbackWidth_ = width;
        fallbackHeight_ = height;
        fallbackStride_ = width * 4;
        fallbackFrame_.resize(
            static_cast<size_t>(fallbackStride_) *
            static_cast<size_t>(fallbackHeight_));
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                fallbackFrame_.data() + row * fallbackStride_,
                data + row * stride,
                static_cast<size_t>(fallbackStride_));
        }
        PresentViaGdi(data, width, height, stride);
    } else {
        fallbackFrame_.clear();
        fallbackWidth_ = fallbackHeight_ = fallbackStride_ = 0;
    }

    if (presentedWithVideoProcessor) {
        swapChain_->Present(0, 0);
    }

    // ── Cursor overlay ────────────────────────────────────────────────────────
    if (cursorVisible_.load(std::memory_order_relaxed)) {
        RECT vr{};
        {
            std::lock_guard<std::mutex> lk2(rectMutex_);
            vr = videoRect_;
        }
        float nx = cursorX_.load(std::memory_order_relaxed);
        float ny = cursorY_.load(std::memory_order_relaxed);
        int cx = vr.left + static_cast<int>(nx * (vr.right  - vr.left));
        int cy = vr.top  + static_cast<int>(ny * (vr.bottom - vr.top));
        DrawGdiCursor(hwnd_, cx, cy);
    }

}

// ─── VideoRect ───────────────────────────────────────────────────────────────

RECT D3D11Renderer::VideoRect() const {
    std::lock_guard<std::mutex> lk(rectMutex_);
    return videoRect_;
}

// ─── SetCursor ────────────────────────────────────────────────────────────────

void D3D11Renderer::SetCursor(float x, float y, bool visible) {
    cursorX_.store(x, std::memory_order_relaxed);
    cursorY_.store(y, std::memory_order_relaxed);
    cursorVisible_.store(visible, std::memory_order_relaxed);
}

void D3D11Renderer::PaintFallback() {
    std::lock_guard<std::mutex> lk(presentMutex_);
    if (!initialized_ || useVideoProcessor_ || fallbackFrame_.empty()) return;
    PresentViaGdi(
        fallbackFrame_.data(),
        fallbackWidth_,
        fallbackHeight_,
        fallbackStride_);
}

// ─── Clear ───────────────────────────────────────────────────────────────────

void D3D11Renderer::Clear() {
    std::lock_guard<std::mutex> lk(presentMutex_);
    if (!initialized_ || !rtv_) return;
    fallbackFrame_.clear();
    fallbackWidth_ = fallbackHeight_ = fallbackStride_ = 0;
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    context_->ClearRenderTargetView(rtv_.Get(), black);
    swapChain_->Present(0, 0);
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

void D3D11Renderer::Shutdown() {
    std::lock_guard<std::mutex> lk(presentMutex_);
    if (!initialized_) return;

    vpInputView_.Reset();
    vpOutputView_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    srcTex_.Reset();
    stagingTex_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    rtv_.Reset();
    if (swapChain_) swapChain_->SetFullscreenState(FALSE, nullptr);
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();

    initialized_ = false;
    fallbackFrame_.clear();
    fallbackWidth_ = fallbackHeight_ = fallbackStride_ = 0;
    hwnd_ = nullptr;
    Logger::Info("D3D11Renderer: shut down");
}

// ─── LetterboxRect ───────────────────────────────────────────────────────────

/*static*/ RECT D3D11Renderer::LetterboxRect(int clientW, int clientH,
                                               int srcW,    int srcH)
{
    if (srcW <= 0 || srcH <= 0) return RECT{0, 0, clientW, clientH};

    // Scale to fit while preserving aspect ratio.
    float scaleX = static_cast<float>(clientW) / static_cast<float>(srcW);
    float scaleY = static_cast<float>(clientH) / static_cast<float>(srcH);
    float scale  = std::min(scaleX, scaleY);

    int dstW = static_cast<int>(srcW * scale);
    int dstH = static_cast<int>(srcH * scale);
    if (dstW < 1) dstW = 1;
    if (dstH < 1) dstH = 1;

    int x = (clientW - dstW) / 2;
    int y = (clientH - dstH) / 2;
    return RECT{x, y, x + dstW, y + dstH};
}

} // namespace remcote
