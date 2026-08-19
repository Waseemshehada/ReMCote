#include "DesktopViewer.h"
#include "Logger.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <shlobj.h>

namespace remcote {
struct DesktopViewer::ViewerInstance {
    std::atomic<bool> active{true};
    HWND hwnd = nullptr;
    std::wstring startUrl;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webView;
};

namespace {

constexpr wchar_t kViewerClassName[] = L"ReMCoteDesktopViewerWindow";
std::once_flag g_viewerClassRegistration;

std::wstring ViewerDataDirectory() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        return L"";
    }

    std::filesystem::path dataPath(localAppData);
    CoTaskMemFree(localAppData);
    dataPath /= L"ReMCote";
    dataPath /= L"WebView2";
    std::error_code error;
    std::filesystem::create_directories(dataPath, error);
    return error ? L"" : dataPath.wstring();
}

void ShowWebViewError(HWND owner) {
    Logger::Error("Desktop Viewer could not start; Microsoft Edge WebView2 Runtime is unavailable or failed to initialize");
    MessageBoxW(owner,
                L"ReMCote needs the Microsoft Edge WebView2 Runtime to open the desktop viewer.\n\n"
                L"Run ReMCoteSetup.exe again while connected to the internet, then try again.",
                L"ReMCote Desktop Viewer",
                MB_OK | MB_ICONERROR);
}

} // namespace

bool DesktopViewer::Open(HINSTANCE hInstance, const std::wstring& startUrl) {
    if (hwnd_ && IsWindow(hwnd_)) {
        Logger::Debug("Desktop Viewer is already open; bringing its window to the foreground");
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return true;
    }

    hInstance_ = hInstance;
    std::call_once(g_viewerClassRegistration, [hInstance] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &DesktopViewer::WndProc;
        windowClass.hInstance = hInstance;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kViewerClassName;
        RegisterClassExW(&windowClass);
    });

    auto instance = std::make_shared<ViewerInstance>();
    instance->startUrl = startUrl;
    hwnd_ = CreateWindowExW(
        0, kViewerClassName, L"ReMCote — Connect to a device",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1180, 820,
        nullptr, nullptr, hInstance_, this);
    if (!hwnd_) return false;
    instance->hwnd = hwnd_;
    instance_ = instance;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    CreateWebView(instance);
    Logger::Info("Desktop Viewer window opened");
    return true;
}

void DesktopViewer::Close() {
    auto instance = std::move(instance_);
    if (instance) {
        instance->active = false;
        instance->webView.Reset();
        instance->controller.Reset();
    }
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    Logger::Info("Desktop Viewer window closed");
}

void DesktopViewer::CreateWebView(const std::shared_ptr<ViewerInstance>& instance) {
    const std::wstring userDataDirectory = ViewerDataDirectory();
    const wchar_t* userDataFolder = userDataDirectory.empty() ? nullptr : userDataDirectory.c_str();

    const HRESULT createResult = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder, nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [instance](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || !environment || !instance->active || !IsWindow(instance->hwnd)) {
                    if (instance->active) ShowWebViewError(instance->hwnd);
                    return FAILED(result) ? result : E_ABORT;
                }

                return environment->CreateCoreWebView2Controller(
                    instance->hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [instance](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controllerResult) || !controller || !instance->active || !IsWindow(instance->hwnd)) {
                                if (instance->active) ShowWebViewError(instance->hwnd);
                                return FAILED(controllerResult) ? controllerResult : E_ABORT;
                            }

                            instance->controller = controller;
                            if (FAILED(instance->controller->get_CoreWebView2(&instance->webView)) || !instance->webView) {
                                ShowWebViewError(instance->hwnd);
                                return E_FAIL;
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(instance->webView->get_Settings(&settings)) && settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                            }

                            RECT bounds{};
                            GetClientRect(instance->hwnd, &bounds);
                            instance->controller->put_Bounds(bounds);
                            return instance->webView->Navigate(instance->startUrl.c_str());
                        }).Get());
            }).Get());

    if (FAILED(createResult)) {
        if (instance->active) ShowWebViewError(instance->hwnd);
    }
}

void DesktopViewer::ResizeWebView() {
    const auto instance = instance_;
    if (!instance || !instance->active || !instance->controller || !hwnd_) return;
    RECT bounds{};
    GetClientRect(hwnd_, &bounds);
    instance->controller->put_Bounds(bounds);
}

LRESULT CALLBACK DesktopViewer::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    DesktopViewer* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<DesktopViewer*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DesktopViewer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message) {
    case WM_SIZE:
        self->ResizeWebView();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (self->instance_ && self->instance_->hwnd == hwnd) {
            self->instance_->active = false;
            self->instance_->webView.Reset();
            self->instance_->controller.Reset();
            self->instance_.reset();
        }
        self->hwnd_ = nullptr;
        Logger::Info("Desktop Viewer window destroyed");
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace remcote