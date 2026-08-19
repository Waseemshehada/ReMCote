#include "DesktopViewer.h"

#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iterator>
#include <windowsx.h>

namespace remcote {

namespace {

constexpr int kIdDeviceEdit = 2101;
constexpr int kIdConnect = 2102;
constexpr int kIdDisconnect = 2103;
constexpr int kIdFullscreen = 2104;

constexpr UINT kMessageState = WM_APP + 70;
constexpr UINT kMessageIceServers = WM_APP + 71;
constexpr UINT kMessagePeerSignal = WM_APP + 72;
constexpr UINT kMessageTransportState = WM_APP + 73;
constexpr UINT kMessageRequestKeyframe = WM_APP + 74;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring StateText(ViewerSessionState state, const std::string& message) {
    if (!message.empty()) return Widen(message);
    switch (state) {
    case ViewerSessionState::Connecting: return L"Contacting ReMCote...";
    case ViewerSessionState::AwaitingApproval: return L"Waiting for approval on the remote PC...";
    case ViewerSessionState::Negotiating: return L"Creating the secure peer-to-peer connection...";
    case ViewerSessionState::ConnectedDirect: return L"Connected directly";
    case ViewerSessionState::ConnectedRelay: return L"Connected through TURN relay";
    case ViewerSessionState::Failed: return L"Connection failed";
    case ViewerSessionState::Disconnected: return L"Disconnected";
    default: return L"Enter the other computer's Device ID";
    }
}

} // namespace

DesktopViewer::~DesktopViewer() {
    Close();
}

bool DesktopViewer::Open(HINSTANCE hInstance, const std::string& signalingUrl) {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return true;
    }

    hInstance_ = hInstance;
    signalingUrl_ = signalingUrl;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DesktopViewer::WindowProc;
    windowClass.hInstance = hInstance_;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = CreateSolidBrush(RGB(15, 16, 21));
    windowClass.lpszClassName = L"ReMCoteNativeViewerWindow";
    RegisterClassExW(&windowClass);

    WNDCLASSEXW videoClass{};
    videoClass.cbSize = sizeof(videoClass);
    videoClass.lpfnWndProc = &DesktopViewer::VideoProc;
    videoClass.hInstance = hInstance_;
    videoClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = L"ReMCoteNativeVideoSurface";
    RegisterClassExW(&videoClass);

    hwnd_ = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        L"ReMCote — Connect to another PC",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        800,
        nullptr,
        nullptr,
        hInstance_,
        this);
    if (!hwnd_) {
        Logger::Error("Could not create the native viewer window");
        return false;
    }

    CreateControls();
    Layout();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    return true;
}

void DesktopViewer::CreateControls() {
    titleFont_ = CreateFontW(
        22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    bodyFont_ = CreateFontW(
        17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");

    deviceLabel_ = CreateWindowW(
        L"STATIC", L"Remote Device ID", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);
    deviceEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdDeviceEdit), hInstance_, nullptr);
    SendMessageW(deviceEdit_, EM_SETLIMITTEXT, 11, 0);
    connectButton_ = CreateWindowW(
        L"BUTTON", L"CONNECT", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdConnect), hInstance_, nullptr);
    disconnectButton_ = CreateWindowW(
        L"BUTTON", L"DISCONNECT", WS_CHILD | WS_TABSTOP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdDisconnect), hInstance_, nullptr);
    fullscreenButton_ = CreateWindowW(
        L"BUTTON", L"FULL SCREEN", WS_CHILD | WS_TABSTOP,
        0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(kIdFullscreen), hInstance_, nullptr);
    statusLabel_ = CreateWindowW(
        L"STATIC", L"Enter the other computer's Device ID",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, nullptr);

    videoSurface_ = CreateWindowExW(
        0, L"ReMCoteNativeVideoSurface", L"",
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 0, 0, hwnd_, nullptr, hInstance_, this);

    for (HWND control : {
             deviceLabel_, deviceEdit_, connectButton_, disconnectButton_,
             fullscreenButton_, statusLabel_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    }
    SendMessageW(deviceLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
}

void DesktopViewer::Layout() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (videoVisible_) {
        MoveWindow(videoSurface_, 0, 58, width, std::max(1, height - 58), TRUE);
        MoveWindow(statusLabel_, 16, 18, std::max(100, width - 290), 28, TRUE);
        MoveWindow(fullscreenButton_, std::max(0, width - 262), 10, 116, 36, TRUE);
        MoveWindow(disconnectButton_, std::max(0, width - 138), 10, 122, 36, TRUE);
        renderer_.Resize();
    } else {
        const int panelWidth = std::min(560, std::max(340, width - 80));
        const int left = (width - panelWidth) / 2;
        const int top = std::max(70, height / 2 - 150);
        MoveWindow(deviceLabel_, left, top, panelWidth, 34, TRUE);
        MoveWindow(deviceEdit_, left, top + 50, panelWidth, 48, TRUE);
        MoveWindow(connectButton_, left, top + 116, panelWidth, 48, TRUE);
        MoveWindow(statusLabel_, left, top + 184, panelWidth, 54, TRUE);
    }
}

void DesktopViewer::SetStatus(const std::wstring& status, bool error) {
    if (!statusLabel_) return;
    SetWindowTextW(statusLabel_, status.c_str());
    if (error) MessageBeep(MB_ICONWARNING);
}

void DesktopViewer::ShowVideo(bool visible) {
    videoVisible_ = visible;
    ShowWindow(videoSurface_, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(disconnectButton_, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(fullscreenButton_, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(deviceLabel_, visible ? SW_HIDE : SW_SHOW);
    ShowWindow(deviceEdit_, visible ? SW_HIDE : SW_SHOW);
    ShowWindow(connectButton_, visible ? SW_HIDE : SW_SHOW);
    Layout();
    if (visible) SetFocus(videoSurface_);
}

void DesktopViewer::StartSession() {
    wchar_t buffer[32]{};
    GetWindowTextW(deviceEdit_, buffer, static_cast<int>(std::size(buffer)));
    std::wstring normalized;
    for (const wchar_t character : std::wstring(buffer)) {
        if (std::iswdigit(character)) normalized.push_back(character);
    }
    if (normalized.size() != 9) {
        SetStatus(L"Device ID must contain exactly 9 digits", true);
        SetFocus(deviceEdit_);
        return;
    }

    StopSession("", false);
    EnableWindow(connectButton_, FALSE);
    SetStatus(L"Contacting ReMCote...");

    signaling_ = std::make_unique<ViewerSignaling>(
        signalingUrl_, Narrow(normalized));
    signaling_->SetOnStateChanged(
        [this](ViewerSessionState state, const std::string& message) {
            QueueState(state, message);
        });
    signaling_->SetOnCapabilities(
        [this](const ViewerHostCapabilities& capabilities) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            capabilities_ = capabilities;
        });
    signaling_->SetOnIceServers(
        [this](const std::vector<IceServerCfg>& iceServers) {
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingIceServers_ = iceServers;
            }
            if (hwnd_) PostMessageW(hwnd_, kMessageIceServers, 0, 0);
        });
    signaling_->SetOnPeerSignal(
        [this](const nlohmann::json& payload) { QueuePeerSignal(payload); });
    signaling_->SetOnError(
        [this](const std::string&, const std::string& message) {
            QueueState(ViewerSessionState::Failed, message);
        });
    signaling_->Start();
}

void DesktopViewer::StartTransport(std::vector<IceServerCfg> iceServers) {
    if (!signaling_ || transport_) return;

    ViewerHostCapabilities capabilities;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        capabilities = capabilities_;
    }
    const int width = capabilities.desktopWidth > 0 ? capabilities.desktopWidth : 1920;
    const int height = capabilities.desktopHeight > 0 ? capabilities.desktopHeight : 1080;

    if (!renderer_.Initialize(videoSurface_)) {
        QueueState(ViewerSessionState::Failed, "Could not initialize Direct3D 11");
        return;
    }
    if (!decoder_.Initialize(
            width, height,
            [this](std::vector<uint8_t> bgra, int frameWidth, int frameHeight, int stride) {
                renderer_.PresentBgra(
                    bgra.data(), frameWidth, frameHeight, stride);
            })) {
        QueueState(ViewerSessionState::Failed, "Could not initialize Windows H.264 decoder");
        return;
    }
    StartDecodeWorker();

    transport_ = std::make_unique<ViewerTransport>(std::move(iceServers));
    transport_->SetOnLocalSdp(
        [this](const nlohmann::json& payload) {
            if (signaling_) signaling_->SendSignal(payload);
        });
    transport_->SetOnLocalCandidate(
        [this](const nlohmann::json& payload) {
            if (signaling_) signaling_->SendSignal(payload);
        });
    transport_->SetOnVideoFrame(
        [this](const ViewerFrame& frame) { QueueVideoFrame(frame); });
    transport_->SetOnCursorUpdate(
        [this](float x, float y, bool visible) {
            renderer_.SetCursor(x, y, visible);
        });
    transport_->SetOnConnected(
        [this](bool connected, ViewerConnectionType type) {
            if (connected && signaling_) {
                signaling_->SendConnectionEstablished(type == ViewerConnectionType::Relay);
                transport_->SendKeyframeRequest();
            }
            if (hwnd_) {
                PostMessageW(
                    hwnd_, kMessageTransportState,
                    connected ? 1 : 0,
                    static_cast<LPARAM>(type));
            }
        });
    transport_->SetOnDiagnostic(
        [this](const std::string& message) {
            Logger::Warningf("Native viewer: %s", message.c_str());
        });
    transport_->Open();
}

void DesktopViewer::StopSession(const std::string& reason, bool notifyPeer) {
    if (transport_) {
        transport_->Close();
        transport_.reset();
    }
    StopDecodeWorker();
    decoder_.Shutdown();
    renderer_.Clear();
    if (signaling_) {
        if (notifyPeer) signaling_->SendSessionClosed(reason);
        signaling_->Stop();
        signaling_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingIceServers_.clear();
        pendingPeerSignals_.clear();
        capabilities_ = {};
    }
    EnableWindow(connectButton_, TRUE);
}

void DesktopViewer::StartDecodeWorker() {
    StopDecodeWorker();
    {
        std::lock_guard<std::mutex> lock(decodeMutex_);
        decodeQueue_.clear();
        decodeTimestamp100ns_ = 0;
        decodeRunning_ = true;
    }
    decodeThread_ = std::thread([this] {
        for (;;) {
            ViewerFrame frame;
            {
                std::unique_lock<std::mutex> lock(decodeMutex_);
                decodeCv_.wait(lock, [this] {
                    return !decodeRunning_ || !decodeQueue_.empty();
                });
                if (!decodeRunning_ && decodeQueue_.empty()) break;
                frame = std::move(decodeQueue_.front());
                decodeQueue_.pop_front();
            }
            if (!frame.data.empty()) {
                decoder_.Decode(
                    frame.data.data(), frame.data.size(), decodeTimestamp100ns_);
                decodeTimestamp100ns_ += 166667;
            }
        }
    });
}

void DesktopViewer::StopDecodeWorker() {
    {
        std::lock_guard<std::mutex> lock(decodeMutex_);
        decodeRunning_ = false;
        decodeQueue_.clear();
    }
    decodeCv_.notify_all();
    if (decodeThread_.joinable()) decodeThread_.join();
}

void DesktopViewer::QueueVideoFrame(const ViewerFrame& frame) {
    bool requestKeyframe = false;
    {
        std::lock_guard<std::mutex> lock(decodeMutex_);
        if (!decodeRunning_) return;
        // Never accumulate seconds of stale desktop video. If decoding falls
        // more than ~0.5 s behind at 60 fps, discard the damaged prediction
        // chain and request a fresh IDR.
        if (decodeQueue_.size() >= 30) {
            decodeQueue_.clear();
            requestKeyframe = true;
            if (!frame.keyframe) {
                if (hwnd_) PostMessageW(hwnd_, kMessageRequestKeyframe, 0, 0);
                return;
            }
        }
        decodeQueue_.push_back(frame);
    }
    decodeCv_.notify_one();
    if (requestKeyframe && hwnd_) {
        PostMessageW(hwnd_, kMessageRequestKeyframe, 0, 0);
    }
}

void DesktopViewer::QueueState(
    ViewerSessionState state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingState_ = state;
        pendingMessage_ = message;
    }
    if (hwnd_) PostMessageW(hwnd_, kMessageState, 0, 0);
}

void DesktopViewer::QueuePeerSignal(const nlohmann::json& payload) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingPeerSignals_.push_back(payload);
    }
    if (hwnd_) PostMessageW(hwnd_, kMessagePeerSignal, 0, 0);
}

void DesktopViewer::Close() {
    StopSession("Viewer closed", true);
    renderer_.Shutdown();
    if (fullscreen_) ToggleFullscreen();
    if (hwnd_) {
        HWND window = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(window);
    }
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
    titleFont_ = nullptr;
    bodyFont_ = nullptr;
    hInstance_ = nullptr;
}

bool DesktopViewer::NormalizeVideoPoint(
    LPARAM lParam, float& x, float& y) const {
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const RECT video = renderer_.VideoRect();
    const int width = video.right - video.left;
    const int height = video.bottom - video.top;
    if (width <= 0 || height <= 0 ||
        point.x < video.left || point.x >= video.right ||
        point.y < video.top || point.y >= video.bottom) {
        return false;
    }
    x = static_cast<float>(point.x - video.left) / static_cast<float>(width);
    y = static_cast<float>(point.y - video.top) / static_cast<float>(height);
    return true;
}

void DesktopViewer::ToggleFullscreen() {
    if (!hwnd_) return;
    if (!fullscreen_) {
        previousStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        GetWindowPlacement(hwnd_, &previousPlacement_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(
            hwnd_, GWL_STYLE,
            previousStyle_ & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(
            hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen_ = true;
        SetWindowTextW(fullscreenButton_, L"WINDOW");
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, previousStyle_);
        SetWindowPlacement(hwnd_, &previousPlacement_);
        SetWindowPos(
            hwnd_, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
        SetWindowTextW(fullscreenButton_, L"FULL SCREEN");
    }
}

LRESULT CALLBACK DesktopViewer::WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DesktopViewer* self = reinterpret_cast<DesktopViewer*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DesktopViewer*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->HandleWindowMessage(message, wParam, lParam);
}

LRESULT CALLBACK DesktopViewer::VideoProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DesktopViewer* self = reinterpret_cast<DesktopViewer*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DesktopViewer*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->HandleVideoMessage(message, wParam, lParam);
}

LRESULT DesktopViewer::HandleWindowMessage(
    UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kIdConnect:
            StartSession();
            return 0;
        case kIdDisconnect:
            StopSession("Viewer disconnected", true);
            ShowVideo(false);
            SetStatus(L"Disconnected — enter a Device ID to connect again");
            return 0;
        case kIdFullscreen:
            ToggleFullscreen();
            return 0;
        default:
            break;
        }
        break;
    case kMessageState: {
        ViewerSessionState state;
        std::string text;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            state = pendingState_;
            text = pendingMessage_;
        }
        SetStatus(
            StateText(state, text),
            state == ViewerSessionState::Failed);
        if (state == ViewerSessionState::Failed ||
            state == ViewerSessionState::Disconnected) {
            StopSession("", false);
            ShowVideo(false);
        }
        return 0;
    }
    case kMessageIceServers: {
        std::vector<IceServerCfg> iceServers;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            iceServers.swap(pendingIceServers_);
        }
        StartTransport(std::move(iceServers));
        return 0;
    }
    case kMessagePeerSignal: {
        std::deque<nlohmann::json> signals;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            signals.swap(pendingPeerSignals_);
        }
        if (!transport_) {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            for (auto& signal : signals) pendingPeerSignals_.push_back(std::move(signal));
            return 0;
        }
        for (const auto& signal : signals) {
            const std::string kind = signal.value("kind", "");
            if (kind == "answer") transport_->HandleAnswer(signal);
            else if (kind == "candidate") transport_->HandleRemoteCandidate(signal);
        }
        return 0;
    }
    case kMessageTransportState:
        if (wParam) {
            ShowVideo(true);
            SetStatus(
                static_cast<ViewerConnectionType>(lParam) == ViewerConnectionType::Relay
                    ? L"Connected through TURN relay"
                    : L"Connected peer-to-peer");
        } else if (signaling_) {
            SetStatus(L"Peer connection interrupted", true);
        }
        return 0;
    case kMessageRequestKeyframe:
        if (transport_) transport_->SendKeyframeRequest();
        return 0;
    case WM_CLOSE:
        StopSession("Viewer window closed", true);
        ShowVideo(false);
        renderer_.Shutdown();
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

LRESULT DesktopViewer::HandleVideoMessage(
    UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(videoSurface_, &paint);
        EndPaint(videoSurface_, &paint);
        renderer_.PaintFallback();
        return 0;
    }
    if (!transport_) return DefWindowProcW(videoSurface_, message, wParam, lParam);

    float x = 0.0f;
    float y = 0.0f;
    switch (message) {
    case WM_MOUSEMOVE:
        if (NormalizeVideoPoint(lParam, x, y)) transport_->SendPointerMove(x, y);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (NormalizeVideoPoint(lParam, x, y)) {
            SetFocus(videoSurface_);
            SetCapture(videoSurface_);
            const int button =
                message == WM_LBUTTONDOWN ? 0 :
                message == WM_MBUTTONDOWN ? 1 : 2;
            transport_->SendMouseButton(button, true, x, y);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
        if (NormalizeVideoPoint(lParam, x, y)) {
            const int button =
                message == WM_LBUTTONUP ? 0 :
                message == WM_MBUTTONUP ? 1 : 2;
            transport_->SendMouseButton(button, false, x, y);
        }
        ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(videoSurface_, &point);
        if (NormalizeVideoPoint(MAKELPARAM(point.x, point.y), x, y)) {
            transport_->SendWheel(
                0.0f,
                -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)),
                x,
                y);
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((lParam & (1LL << 30)) == 0) {
            uint32_t scanCode = static_cast<uint32_t>((lParam >> 16) & 0xff);
            if ((lParam & (1LL << 24)) != 0) scanCode |= 0xe000;
            transport_->SendKey("", scanCode, true);
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        uint32_t scanCode = static_cast<uint32_t>((lParam >> 16) & 0xff);
        if ((lParam & (1LL << 24)) != 0) scanCode |= 0xe000;
        transport_->SendKey("", scanCode, false);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        ToggleFullscreen();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(videoSurface_, message, wParam, lParam);
}

} // namespace remcote