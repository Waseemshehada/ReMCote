#include "DesktopViewer.h"
#include "Logger.h"

// Diagnostic stub: WebView2 / WRL code temporarily excluded to isolate the
// MSVC compile error.  If the build passes with this stub, the root cause is
// in the WebView2/WRL implementation.  Restore the full implementation once
// the underlying error is identified and fixed.

namespace remcote {

bool DesktopViewer::Open(HINSTANCE /*hInstance*/, const std::wstring& /*startUrl*/) {
    Logger::Warning("DesktopViewer: viewer temporarily disabled in this diagnostic build");
    return false;
}

void DesktopViewer::Close() {
    // no-op in diagnostic stub
}

} // namespace remcote
