#pragma once
// D3D11Renderer — presents decoded BGRA frames to a Win32 HWND via a D3D11
// device and a DXGI flip/discard swap chain with a D3D11 Video Processor for
// hardware colour-space conversion and scaling.  A software GDI fallback
// (BitBlt) is used if the video processor is unavailable.
//
// PresentBgra() is safe to call from any thread; Resize()/Shutdown() must be
// called from the UI thread.  Internal double-buffer upload avoids blocking
// the caller while the GPU reads the previous frame.

#include <atomic>
#include <mutex>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace remcote {

class D3D11Renderer {
public:
    // Create the D3D11 device and swap chain for hwnd.
    bool Initialize(HWND hwnd);

    // Respond to WM_SIZE: recreate the swap chain buffers.
    void Resize();

    // Upload a decoded BGRA frame and present it, letterboxing to the client area.
    // Thread-safe.  data must remain valid for the duration of the call.
    void PresentBgra(const uint8_t* data, int width, int height, int stride);

    // Return the destination rectangle (letterbox area) in client coordinates.
    RECT VideoRect() const;

    // Update the remote cursor overlay position (normalised 0–1 client coords).
    // Thread-safe.
    void SetCursor(float x, float y, bool visible);

    // Clear the swap chain to black and present.
    void Clear();

    // Repaint the cached software frame after WM_PAINT. No-op on the D3D path.
    void PaintFallback();

    // Release all D3D resources.
    void Shutdown();

private:
    // (Re)create swap-chain-sized render-target view.
    bool CreateRenderTarget();

    // (Re)create the video processor for the given source dimensions.
    bool CreateVideoProcessor(int width, int height);

    // Upload BGRA pixels into the staging texture then copy to srcTex_.
    // Returns false on failure.
    bool UploadBgra(const uint8_t* data, int width, int height, int stride);

    // Present via D3D11VideoProcessor (hardware path).
    void PresentViaVideoProcessor(int width, int height);

    // Present via GDI StretchBlt into a DXGI-compatible surface (SW fallback).
    void PresentViaGdi(const uint8_t* data, int width, int height, int stride);

    // Compute letterbox RECT inside clientW×clientH for srcW×srcH content.
    static RECT LetterboxRect(int clientW, int clientH, int srcW, int srcH);

    // ── Device & swap chain ───────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>        swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;

    // ── Video processor ───────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>         videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>         videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor>       vp_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView>  vpInputView_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> vpOutputView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>            srcTex_;      // BGRA source
    Microsoft::WRL::ComPtr<ID3D11Texture2D>            stagingTex_;  // CPU-writable upload buffer
    int vpSrcWidth_  = 0;
    int vpSrcHeight_ = 0;

    // ── Window / layout ───────────────────────────────────────────────────────
    HWND hwnd_ = nullptr;
    int  clientW_ = 0;
    int  clientH_ = 0;

    // ── Cursor overlay ────────────────────────────────────────────────────────
    std::atomic<float> cursorX_{0.f};
    std::atomic<float> cursorY_{0.f};
    std::atomic<bool>  cursorVisible_{false};

    // ── Video rect cache ──────────────────────────────────────────────────────
    mutable std::mutex rectMutex_;
    RECT videoRect_{};

    // ── Serialise GPU work from multiple threads ──────────────────────────────
    std::mutex presentMutex_;

    bool useVideoProcessor_ = false; // false → GDI fallback
    bool initialized_       = false;

    std::vector<uint8_t> fallbackFrame_;
    int fallbackWidth_ = 0;
    int fallbackHeight_ = 0;
    int fallbackStride_ = 0;
};

} // namespace remcote
