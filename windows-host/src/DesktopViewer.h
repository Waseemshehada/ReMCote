#pragma once

#include <memory>
#include <string>
#include <windows.h>

#include <WebView2.h>
#include <wrl/client.h>     // Microsoft::WRL::ComPtr
#include <wrl/implements.h> // Microsoft::WRL::Callback

namespace remcote {

// A native desktop shell for the existing ReMCote Viewer.  It keeps the
// established browser WebRTC implementation inside the installed Windows app,
// while the host continues to run natively in the same process.
class DesktopViewer {
public:
    bool Open(HINSTANCE hInstance, const std::wstring& startUrl);
    void Close();

private:
    struct ViewerInstance;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void CreateWebView(const std::shared_ptr<ViewerInstance>& instance);
    void ResizeWebView();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    std::shared_ptr<ViewerInstance> instance_;
};

} // namespace remcote