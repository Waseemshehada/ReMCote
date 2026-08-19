#pragma once
// DesktopViewer — WebView2 embedded viewer (diagnostic stub).
// WebView2 / WRL code temporarily excluded from this build to isolate
// the MSVC compile error.  Re-enable by restoring the full implementation.

#include <string>
#include <windows.h>

namespace remcote {

class DesktopViewer {
public:
    bool Open(HINSTANCE hInstance, const std::wstring& startUrl);
    void Close();

private:
    HWND hwnd_ = nullptr;
};

} // namespace remcote
