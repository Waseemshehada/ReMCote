// ReMCote Host — entry point. Wires capture -> encode -> WebRTC and
// native viewer input -> SendInput, gated behind explicit Host approval.
//
// Threads (spec §17): capture, encoder (runs inside capture callback but on a
// dedicated encode submission), WebRTC/network (libdatachannel), input
// injection, UI (this thread). Input never waits on video.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>     // std::wcsstr
#include <filesystem>
#include <objbase.h>  // CoInitializeEx, CoUninitialize, CoTaskMemFree (excluded by WIN32_LEAN_AND_MEAN)
#include <fstream>
#include <memory>
#include <string>
#include <windows.h>

#include "CaptureEngine.h"
#include "DesktopViewer.h"
#include "DeviceRegistration.h"
#include "EncoderEngine.h"
#include "HostUI.h"
#include "InputEngine.h"
#include "Logger.h"
#include "PerformanceMonitor.h"
#include "WebRtcTransport.h"

// Tell NVIDIA Optimus and AMD switchable graphics to run ReMCote on the
// discrete GPU before DXGI chooses a device. NVENC cannot encode textures
// created on an integrated-GPU D3D11 device.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

using namespace remcote;

// ─── Signaling URL resolution ─────────────────────────────────────────────────
//
// Priority order:
//   1. REMCOTE_SIGNALING_URL environment variable  (developer override)
//   2. REMCOTE_SERVER environment variable          (developer override)
//   3. remcote-server.txt next to the exe           (on-site override)
//   4. Built-in production default                  (normal users — zero config)
//
// The built-in default points at the production ReMCote server so that a
// normal user can extract the ZIP and double-click ReMCoteHost.exe with no
// additional configuration.

static const char* kDefaultSignalingUrl = "wss://remcote.replit.app/api/ws";

namespace {

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

    bool Ready() const { return SUCCEEDED(result_); }
    HRESULT Result() const { return result_; }

private:
    HRESULT result_;
};

struct SignalingConfig {
    std::string url;
    const char* source; // human-readable origin, for startup diagnostics
};

SignalingConfig ResolveSignaling() {
    if (const char* e = std::getenv("REMCOTE_SIGNALING_URL"))
        return {e, "REMCOTE_SIGNALING_URL"};
    if (const char* e = std::getenv("REMCOTE_SERVER"))
        return {e, "REMCOTE_SERVER"};
    std::ifstream cfg("remcote-server.txt");
    if (cfg) {
        std::string u;
        std::getline(cfg, u);
        while (!u.empty() && (u.back() == '\r' || u.back() == ' ')) u.pop_back();
        if (!u.empty()) return {u, "remcote-server.txt"};
    }
    return {kDefaultSignalingUrl, "built-in production default"};
}

// ─── Logging setup ────────────────────────────────────────────────────────────

std::string DefaultLogPath() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    std::filesystem::path directory = localAppData && *localAppData
        ? std::filesystem::path(localAppData)
        : std::filesystem::current_path();
    directory /= "ReMCote";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / "remcote-host.log").string();
}

void SetupLogging() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// ─── CI self-test ─────────────────────────────────────────────────────────────
//
// Usage:  ReMCoteHost.exe --ci-self-test
//
// Verifies things that are possible without an NVIDIA GPU or live network:
//   - Default signaling URL resolves to the correct production address.
//   - D3D11 system DLL is present.
//
// NVENC availability is reported but does NOT gate the exit code, because
// GitHub-hosted Windows runners have no NVIDIA GPU.  GPU/DXGI/NVENC runtime
// tests remain a real-PC task.

static int CiSelfTest() {
    std::printf("=== ReMCote CI Self-Test ===\n");
    bool pass = true;

    // Test 1: Default signaling URL
    // If a developer override is present in the environment this warning fires,
    // but the test still checks what WOULD be used with no overrides.
    const SignalingConfig sig = ResolveSignaling();
    if (sig.source != std::string("built-in production default")) {
        std::printf("[WARN] URL sourced from '%s' — unset overrides to test the built-in default\n",
                    sig.source);
    }
    const bool urlOk = (sig.url == kDefaultSignalingUrl);
    std::printf("[TEST] Default URL (%s)  %s\n", sig.url.c_str(), urlOk ? "PASS" : "FAIL");
    if (!urlOk) {
        std::fprintf(stderr,
                     "[FAIL] Expected '%s', got '%s'\n", kDefaultSignalingUrl, sig.url.c_str());
        pass = false;
    }

    // Test 2: D3D11 DLL (always installed on Windows 10/11)
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    std::printf("[TEST] D3D11 DLL                %s\n", d3d ? "PASS" : "FAIL");
    if (!d3d) pass = false;
    else FreeLibrary(d3d);

    // Test 3: NVENC DLL — INFORMATIONAL only; absent on CI runner is expected.
    HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    std::printf("[INFO] NVENC DLL                %s  (absent on CI runner is expected)\n",
                nvenc ? "PRESENT" : "ABSENT");
    if (nvenc) FreeLibrary(nvenc);

    // Test 4: Windows' built-in Media Foundation decoder runtime used by the
    // native viewer. No browser or WebView2 runtime is required.
    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");
    HMODULE codec = LoadLibraryW(L"msmpeg2vdec.dll");
    std::printf("[TEST] Media Foundation runtime   %s\n",
                mfplat && codec ? "PASS" : "FAIL");
    if (mfplat) FreeLibrary(mfplat);
    if (codec) FreeLibrary(codec);
    if (!mfplat || !codec) pass = false;

    std::printf("=== CI Self-Test: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// ─── Preflight ────────────────────────────────────────────────────────────────

const char* PassFail(bool ok) { return ok ? "PASS" : "FAIL"; }

bool RunPreflight(const std::string& serverUrl) {
    Logger::Info("Starting host preflight checks");

    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    bool winOk = false;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            winOk = fn(&ver) == 0 && ver.dwMajorVersion >= 10;
        }
    }
    Logger::Infof("Windows %lu.%lu build %lu: %s",
                  ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber, PassFail(winOk));

    HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    Logger::Infof("NVENC driver availability: %s", PassFail(nvenc != nullptr));
    if (nvenc) FreeLibrary(nvenc);

    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    Logger::Infof("Direct3D 11 availability: %s", PassFail(d3d != nullptr));
    if (d3d) FreeLibrary(d3d);

    const bool urlOk = serverUrl.rfind("ws://", 0) == 0 || serverUrl.rfind("wss://", 0) == 0;
    Logger::Infof("Signaling URL configured: %s (%s)",
                  PassFail(urlOk), Logger::RedactUrl(serverUrl).c_str());
    // With the built-in default this check always passes; it guards against
    // accidentally removing the production URL in a future refactor.
    if (!urlOk) {
        Logger::Errorf("Signaling URL is empty or invalid: %s",
                       Logger::RedactUrl(serverUrl).c_str());
        return false;
    }
    return true;
}

// ─── Application ─────────────────────────────────────────────────────────────

struct HostApp {
    HostUI ui;
    DesktopViewer viewer;
    CaptureEngine capture;
    EncoderEngine encoder;
    InputEngine input;
    PerformanceMonitor perf;
    std::unique_ptr<WebRtcTransport> rtc;
    std::unique_ptr<DeviceRegistration> registration;
    std::atomic<bool> approved{false};
    std::atomic<bool> pipelineRunning{false};
    std::string activeSessionId;
    std::atomic<bool> loggedFirstCapture{false};
    std::atomic<bool> loggedFirstEncode{false};
    std::atomic<bool> shutdownStarted{false};

    void StartPipeline() {
        if (pipelineRunning.exchange(true)) return;
        EncoderConfig cfg;
        cfg.width = capture.Width();
        cfg.height = capture.Height();
        cfg.fps = capture.RefreshHz() > 0 ? capture.RefreshHz() : 60;
        cfg.bitrateKbps = 20000;
        cfg.maxBitrateKbps = 80000;
        if (!encoder.Initialize(capture.Device(), cfg)) {
            ui.SetStatusLine("Encoder init failed - is this an NVIDIA GPU?");
            Logger::Error("NVENC encoder initialization failed; remote session cannot start");
            pipelineRunning = false;
            return;
        }
        encoder.SetOutputCallback([this](const EncodedFrame& f) {
            if (!loggedFirstEncode.exchange(true))
                Logger::Infof("First video frame encoded: %zu bytes, keyframe=%d",
                              f.size, f.keyframe ? 1 : 0);
            perf.OnEncodeFrame(f.encodeDurationUs);
            rtc->SendFrame(f);
        });
        capture.Start(
            [this](ID3D11Texture2D* tex, int64_t captureUs) {
                if (!loggedFirstCapture.exchange(true))
                    Logger::Info("First desktop frame captured");
                const int64_t t0 = NowUs();
                if (!encoder.SubmitFrame(tex, captureUs)) {
                    perf.OnFrameDropped();
                } else {
                    perf.OnCaptureFrame(NowUs() - t0);
                }
            },
            [this](float x, float y, bool visible) { rtc->SendCursor(x, y, visible); });
        input.Start();
        ui.OnSessionActive(true);
        ui.SetStatusLine("Remote session active");
        Logger::Info("Remote session pipeline is active");
    }

    void StopPipeline(const std::string& reason, bool notifyPeer = true) {
        if (!pipelineRunning.exchange(false)) {
            approved = false;
            return;
        }
        capture.Stop();
        input.Stop();
        encoder.Shutdown();
        approved = false;
        if (rtc) rtc->CloseAll();
        if (notifyPeer && registration && !activeSessionId.empty())
            registration->NotifySessionClosed(activeSessionId, reason);
        activeSessionId.clear();
        ui.OnSessionActive(false);
        ui.SetStatusLine("Ready for connection");
        Logger::Infof("Remote session stopped: %s", reason.c_str());
    }

    void Shutdown() {
        if (shutdownStarted.exchange(true)) return;
        Logger::Info("ReMCote Desktop is shutting down");
        viewer.Close();
        if (registration && !activeSessionId.empty()) {
            registration->NotifySessionClosed(
                activeSessionId, "Application closed");
        }
        if (registration) registration->Stop();
        StopPipeline("Application closed", false);
        rtc.reset();
        registration.reset();
        Logger::Info("All host services stopped");
        Logger::Shutdown();
    }
};

} // namespace

// ─── Entry point ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    SetupLogging();

    // CI self-test mode: verify non-GPU invariants and exit. No UI, no DXGI.
    if (lpCmdLine && std::wcsstr(lpCmdLine, L"--ci-self-test")) {
        return CiSelfTest();
    }

    ComApartment comApartment;
    if (!comApartment.Ready()) {
        MessageBoxW(nullptr,
                    L"ReMCote could not initialize Windows desktop services.\n\n"
                    L"Close other copies of ReMCote and try again.",
                    L"ReMCote Desktop",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    auto app = std::make_unique<HostApp>();
    Logger::Initialize(DefaultLogPath(), [&app](const std::string& line) {
        app->ui.AppendLog(line);
    });
    Logger::Info("ReMCote Desktop starting");

    const SignalingConfig sig = ResolveSignaling();
    Logger::Infof("Signaling server: %s", Logger::RedactUrl(sig.url).c_str());
    Logger::Debugf("Signaling source: %s", sig.source);

    if (!RunPreflight(sig.url)) {
        // This should never fire with the built-in default, but protects
        // against future accidental regressions.
        MessageBoxW(nullptr,
                    L"Signaling URL is invalid.\n\n"
                    L"If you are a developer, set REMCOTE_SIGNALING_URL to your server.\n"
                    L"Normal users: just double-click ReMCoteHost.exe — no configuration needed.",
                    L"ReMCote Host", MB_ICONERROR);
        Logger::Shutdown();
        return 1;
    }

    if (!app->ui.Create(hInstance)) {
        Logger::Shutdown();
        return 1;
    }
    app->ui.RefreshLogView();

    app->ui.SetViewerRequest([&app, hInstance, signalingUrl = sig.url] {
        app->viewer.Open(hInstance, signalingUrl);
    });
    app->ui.SetStopSession([&app] { app->StopPipeline("Host stopped the session"); });
    app->ui.SetPeerDisconnected(
        [&app] { app->StopPipeline("Peer disconnected"); });
    app->ui.SetExitRequest([&app] {
        app->Shutdown();
    });
    app->ui.SetTelemetryProvider([&app] { return app->perf.Sample(); });

    auto runViewerOnly = [&app](const wchar_t* reason) {
        Logger::Warning("Host prerequisites unavailable; starting viewer-only mode");
        app->ui.SetGpuInfo("Host unavailable", "Viewer ready");
        app->ui.SetStatusLine("Viewer mode: connect to another device");
        MessageBoxW(
            nullptr,
            reason,
            L"ReMCote Desktop",
            MB_ICONINFORMATION);
        const int rc = app->ui.RunMessageLoop();
        app->Shutdown();
        return rc;
    };

    if (!app->capture.Initialize()) {
        return runViewerOnly(
            L"This computer cannot host a ReMCote session because screen "
            L"capture is unavailable.\n\n"
            L"You can still use it to connect to another ReMCote device.");
    }
    Logger::Info("DXGI desktop duplication initialized");
    Logger::Infof("Capture GPU: %s", app->capture.GpuName().c_str());
    Logger::Infof("Capture dimensions: %dx%d @ %d Hz",
                  app->capture.Width(), app->capture.Height(), app->capture.RefreshHz());

    if (!app->capture.IsNvidiaAdapter()) {
        std::wstring gpu;
        const std::string gpuUtf8 = app->capture.GpuName();
        if (!gpuUtf8.empty()) {
            const int chars = MultiByteToWideChar(
                CP_UTF8, 0, gpuUtf8.c_str(), -1, nullptr, 0);
            if (chars > 1) {
                gpu.resize(static_cast<size_t>(chars));
                MultiByteToWideChar(
                    CP_UTF8, 0, gpuUtf8.c_str(), -1, gpu.data(), chars);
                if (!gpu.empty() && gpu.back() == L'\0') gpu.pop_back();
            }
        }
        const std::wstring reason =
            L"ReMCote found an NVIDIA GPU, but Windows assigned desktop "
            L"capture to a different graphics adapter:\n\n" +
            (gpu.empty() ? L"Unknown adapter" : gpu) +
            L"\n\nOpen Windows Settings > System > Display > Graphics, add "
            L"ReMCoteHost.exe, select Options, and choose High performance. "
            L"Then restart ReMCote.";
        return runViewerOnly(reason.c_str());
    }

    EncoderConfig encoderProbe;
    encoderProbe.width = app->capture.Width();
    encoderProbe.height = app->capture.Height();
    encoderProbe.fps =
        app->capture.RefreshHz() > 0 ? app->capture.RefreshHz() : 60;
    encoderProbe.bitrateKbps = 20000;
    encoderProbe.maxBitrateKbps = 80000;
    if (!app->encoder.Initialize(app->capture.Device(), encoderProbe)) {
        return runViewerOnly(
            L"This computer cannot host a ReMCote session because a compatible "
            L"NVIDIA NVENC encoder is unavailable.\n\n"
            L"You can still use it to connect to another ReMCote device.");
    }
    app->encoder.Shutdown();
    Logger::Info("NVENC host capability probe succeeded");

    HostCapabilities caps;
    caps.h264 = true;
    caps.gpuName = app->capture.GpuName();
    caps.encoderName = "NVENC H.264";
    caps.desktopWidth = app->capture.Width();
    caps.desktopHeight = app->capture.Height();
    caps.desktopHz = app->capture.RefreshHz();

    app->ui.SetGpuInfo(caps.gpuName, "NVENC Ready");
    app->ui.SetStatusLine("Connecting to ReMCote...");

    Logger::Infof("Opening signaling connection to %s", Logger::RedactUrl(sig.url).c_str());
    app->registration = std::make_unique<DeviceRegistration>(sig.url, caps);

    app->registration->SetOnRegistered(
        [&app](const std::string& id, const std::vector<IceServerCfg>& iceServers) {
            app->ui.SetDeviceId(id);
            app->ui.SetOnline(true);
            app->ui.SetStatusLine("Ready for connection");
             // Re-registration after a signaling reconnect must not replace a
             // live WebRTC transport or discard its pending peer session.
             if (!app->rtc) {
                 app->rtc = std::make_unique<WebRtcTransport>(app->input, iceServers);
                 app->rtc->SetSignalOut([&app](const std::string& sid, const nlohmann::json& payload) {
                     app->registration->SendSignal(sid, payload);
                 });
                 app->rtc->SetBitrateRequest([&app](int kbps) { app->encoder.SetBitrate(kbps); });
                 app->rtc->SetKeyframeRequest([&app] { app->encoder.RequestKeyframe(); });
                  app->rtc->SetSessionEnded([&app](const std::string&) {
                      app->ui.NotifyPeerDisconnected();
                 });
             }
        });

    app->registration->SetOnConnectRequest(
        [&app](const std::string& sessionId, const std::string& desc) {
            Logger::Info("Incoming remote-session request received");
            app->ui.ShowConnectionRequest(sessionId, desc);
        });

    app->ui.SetApprovalDecision([&app](const std::string& sessionId, bool accept) {
        app->registration->RespondToConnect(sessionId, accept);
        if (accept) {
            Logger::Info("Host approved the remote-session request");
            app->approved = true;
            app->activeSessionId = sessionId;
            app->ui.SetStatusLine("Negotiating direct connection...");
            app->StartPipeline();
        } else {
            app->ui.SetStatusLine("Ready for connection");
            Logger::Info("Host declined the remote-session request");
        }
    });

    app->registration->SetOnPeerSignal(
        [&app](const std::string& sessionId, const nlohmann::json& payload) {
            if (!app->approved || !app->rtc) return;
            if (sessionId != app->activeSessionId) {
                Logger::Warning("Dropped signaling message for a non-approved session");
                return;
            }
            app->rtc->HandlePeerSignal(sessionId, payload);
        });

    app->registration->SetOnSessionEnded(
        [&app](const std::string&, const std::string& reason) { app->StopPipeline(reason); });

    app->registration->Start();

    const int rc = app->ui.RunMessageLoop();
    app->Shutdown();
    return rc;
}
