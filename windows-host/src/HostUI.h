#pragma once
// Visible Win32 host window (spec §4, §36). Shows the Device ID, presence, GPU
// / encoder status, a live performance readout, and — crucially — a large,
// explicit ALLOW / DECLINE prompt for every incoming connection. Remote
// control never begins until the physical Host user clicks ALLOW.

#include <atomic>
#include <functional>
#include <deque>
#include <mutex>
#include <string>
#include <windows.h>

#include "PerformanceMonitor.h"

namespace remcote {

class HostUI {
public:
    using ApprovalDecision = std::function<void(const std::string& sessionId, bool accept)>;
    using StopSession = std::function<void()>;
    using ExitRequest = std::function<void()>;
    using ViewerRequest = std::function<void()>;
    using PeerDisconnected = std::function<void()>;
    using TelemetryProvider = std::function<PerformanceMonitor::Snapshot()>;

    bool Create(HINSTANCE hInstance);
    int RunMessageLoop();

    // Thread-safe updates from signaling / RTC / telemetry threads.
    void SetDeviceId(const std::string& id);
    void SetOnline(bool online);
    void SetGpuInfo(const std::string& gpu, const std::string& encoder);
    void SetStatusLine(const std::string& text);
    void AppendLog(const std::string& line);
    void RefreshLogView();
    void ShowConnectionRequest(const std::string& sessionId, const std::string& description);
    void OnSessionActive(bool active);
    void UpdateTelemetry(const PerformanceMonitor::Snapshot& snap);
    void NotifyPeerDisconnected();

    void SetApprovalDecision(ApprovalDecision cb) { onApproval_ = std::move(cb); }
    void SetStopSession(StopSession cb) { onStopSession_ = std::move(cb); }
    void SetExitRequest(ExitRequest cb) { onExit_ = std::move(cb); }
    void SetViewerRequest(ViewerRequest cb) { onViewer_ = std::move(cb); }
    void SetPeerDisconnected(PeerDisconnected cb) {
        onPeerDisconnected_ = std::move(cb);
    }
    void SetTelemetryProvider(TelemetryProvider cb) { telemetryProvider_ = std::move(cb); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HDC hdc);
    void Layout();
    void CopyLogsToClipboard();
    void QueueUiRefresh(bool bringToFront = false);

    HWND hwnd_ = nullptr;
    HWND allowBtn_ = nullptr;
    HWND declineBtn_ = nullptr;
    HWND stopBtn_ = nullptr;
    HWND viewerBtn_ = nullptr;
    HWND copyLogBtn_ = nullptr;
    HWND logView_ = nullptr;
    HFONT fontBig_ = nullptr;
    HFONT fontId_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontLog_ = nullptr;

    std::mutex mutex_;
    std::string deviceId_ = "---------";
    std::string gpu_ = "Detecting...";
    std::string encoder_ = "Detecting...";
    std::string status_ = "Starting...";
    std::string pendingSessionId_;
    std::string requestDescription_;
    bool online_ = false;
    bool requestPending_ = false;
    bool sessionActive_ = false;
    PerformanceMonitor::Snapshot telemetry_{};
    std::deque<std::string> logLines_;
    std::atomic<bool> logRefreshPending_{false};
    std::atomic<bool> uiRefreshPending_{false};
    std::atomic<bool> bringToFrontPending_{false};

    ApprovalDecision onApproval_;
    StopSession onStopSession_;
    ExitRequest onExit_;
    ViewerRequest onViewer_;
    PeerDisconnected onPeerDisconnected_;
    TelemetryProvider telemetryProvider_;
};

} // namespace remcote
