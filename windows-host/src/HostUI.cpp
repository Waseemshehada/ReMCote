#include "HostUI.h"

#include <cstdio>

namespace remcote {

enum ControlId { ID_ALLOW = 1001, ID_DECLINE = 1002, ID_STOP = 1003 };

static std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

bool HostUI::Create(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HostUI::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(11, 11, 14)); // nearly-black
    wc.lpszClassName = L"ReMCoteHostWindow";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0, L"ReMCoteHostWindow", L"ReMCote Host",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 560,
        nullptr, nullptr, hInstance, this);
    if (!hwnd_) return false;

    fontBig_ = CreateFontW(30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fontId_ = CreateFontW(46, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    fontBody_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    allowBtn_ = CreateWindowW(L"BUTTON", L"ALLOW",
        WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_, (HMENU)ID_ALLOW, hInstance, nullptr);
    declineBtn_ = CreateWindowW(L"BUTTON", L"DECLINE",
        WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_, (HMENU)ID_DECLINE, hInstance, nullptr);
    stopBtn_ = CreateWindowW(L"BUTTON", L"STOP REMOTE SESSION",
        WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_, (HMENU)ID_STOP, hInstance, nullptr);
    SendMessageW(allowBtn_, WM_SETFONT, (WPARAM)fontBody_, TRUE);
    SendMessageW(declineBtn_, WM_SETFONT, (WPARAM)fontBody_, TRUE);
    SendMessageW(stopBtn_, WM_SETFONT, (WPARAM)fontBody_, TRUE);

    Layout();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void HostUI::Layout() {
    // Approval buttons appear only while a request is pending.
    ShowWindow(allowBtn_, requestPending_ ? SW_SHOW : SW_HIDE);
    ShowWindow(declineBtn_, requestPending_ ? SW_SHOW : SW_HIDE);
    ShowWindow(stopBtn_, sessionActive_ ? SW_SHOW : SW_HIDE);
    MoveWindow(declineBtn_, 30, 360, 170, 54, TRUE);
    MoveWindow(allowBtn_, 224, 360, 170, 54, TRUE);
    MoveWindow(stopBtn_, 30, 440, 364, 48, TRUE);
}

int HostUI::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK HostUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HostUI* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<HostUI*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HostUI*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        self->OnPaint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)GetStockObject(NULL_BRUSH);
    case WM_TIMER:
        if (self->telemetryProvider_) self->UpdateTelemetry(self->telemetryProvider_());
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        std::string sid;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            sid = self->pendingSessionId_;
        }
        if (id == ID_ALLOW) {
            self->requestPending_ = false;
            self->Layout();
            if (self->onApproval_) self->onApproval_(sid, true);
        } else if (id == ID_DECLINE) {
            self->requestPending_ = false;
            self->Layout();
            if (self->onApproval_) self->onApproval_(sid, false);
        } else if (id == ID_STOP) {
            if (self->onStopSession_) self->onStopSession_();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        // Closing the app immediately terminates all access (spec §37).
        if (self->onExit_) self->onExit_();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void DrawText(HDC hdc, int x, int y, const std::wstring& s, COLORREF color, HFONT font) {
    SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc, x, y, s.c_str(), (int)s.size());
}

void HostUI::OnPaint(HDC hdc) {
    std::lock_guard<std::mutex> lock(mutex_);
    const COLORREF white = RGB(238, 238, 242);
    const COLORREF grey = RGB(150, 150, 160);
    const COLORREF blue = RGB(80, 140, 255);
    const COLORREF green = RGB(70, 200, 120);
    const COLORREF red = RGB(240, 90, 90);

    DrawText(hdc, 30, 24, L"ReMCote", blue, fontBig_);
    DrawText(hdc, 30, 78, L"THIS DEVICE", grey, fontBody_);
    DrawText(hdc, 30, 104, Widen(deviceId_), white, fontId_);

    DrawText(hdc, 30, 166, online_ ? L"\u25CF ONLINE" : L"\u25CF OFFLINE",
             online_ ? green : red, fontBody_);
    DrawText(hdc, 30, 192, Widen(status_), grey, fontBody_);

    DrawText(hdc, 30, 236, L"GPU", grey, fontBody_);
    DrawText(hdc, 90, 236, Widen(gpu_), white, fontBody_);
    DrawText(hdc, 30, 262, L"Encoder", grey, fontBody_);
    DrawText(hdc, 120, 262, Widen(encoder_), white, fontBody_);

    if (requestPending_) {
        DrawText(hdc, 30, 300, L"Incoming ReMCote Connection", white, fontBody_);
        DrawText(hdc, 30, 324, L"Another computer is requesting access.", grey, fontBody_);
    } else if (sessionActive_) {
        wchar_t buf[256];
        swprintf(buf, 256, L"Cap %.1fms  Enc %.1fms  %d/%d fps  drop %llu",
                 telemetry_.captureMs, telemetry_.encodeMs,
                 telemetry_.captureFps, telemetry_.encodeFps,
                 (unsigned long long)telemetry_.framesDropped);
        DrawText(hdc, 30, 300, L"REMOTE SESSION ACTIVE", green, fontBody_);
        DrawText(hdc, 30, 324, buf, grey, fontBody_);
    }
}

// --- thread-safe setters ---------------------------------------------------

void HostUI::SetDeviceId(const std::string& id) {
    { std::lock_guard<std::mutex> l(mutex_); deviceId_ = id; }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void HostUI::SetOnline(bool online) {
    { std::lock_guard<std::mutex> l(mutex_); online_ = online; }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void HostUI::SetGpuInfo(const std::string& gpu, const std::string& encoder) {
    { std::lock_guard<std::mutex> l(mutex_); gpu_ = gpu; encoder_ = encoder; }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void HostUI::SetStatusLine(const std::string& text) {
    { std::lock_guard<std::mutex> l(mutex_); status_ = text; }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void HostUI::ShowConnectionRequest(const std::string& sessionId, const std::string& description) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        pendingSessionId_ = sessionId;
        requestDescription_ = description;
        requestPending_ = true;
    }
    if (hwnd_) {
        SetForegroundWindow(hwnd_);
        FlashWindow(hwnd_, TRUE);
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
void HostUI::OnSessionActive(bool active) {
    {
        std::lock_guard<std::mutex> l(mutex_);
        sessionActive_ = active;
        if (active) requestPending_ = false;
    }
    if (hwnd_) {
        if (active) SetTimer(hwnd_, 1, 1000, nullptr);
        else KillTimer(hwnd_, 1);
        Layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
void HostUI::UpdateTelemetry(const PerformanceMonitor::Snapshot& snap) {
    { std::lock_guard<std::mutex> l(mutex_); telemetry_ = snap; }
}

} // namespace remcote
