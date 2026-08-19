// ReMCote Host — entry point. Wires capture -> encode -> WebRTC and
// browser input -> SendInput, gated behind explicit Host approval.
//
// Threads (spec §17): capture, encoder (runs inside capture callback but on a
// dedicated encode submission), WebRTC/network (libdatachannel), input
// injection, UI (this thread). Input never waits on video.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <windows.h>

#include "CaptureEngine.h"
#include "DeviceRegistration.h"
#include "EncoderEngine.h"
#include "HostUI.h"
#include "InputEngine.h"
#include "PerformanceMonitor.h"
#include "WebRtcTransport.h"

using namespace remcote;

namespace {

// Route stdout/stderr somewhere useful. When launched from a terminal the
// logs appear there; when double-clicked they go to remcote-host.log next to
// the exe. All [TAG] diagnostics below rely on this.
void SetupLogging() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    } else {
        FILE* f = nullptr;
        freopen_s(&f, "remcote-host.log", "w", stdout);
        freopen_s(&f, "remcote-host.log", "a", stderr);
    }
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered — logs survive a crash
    setvbuf(stderr, nullptr, _IONBF, 0);
}

// Signaling URL resolution, in priority order. There is deliberately no
// baked-in default: a fabricated URL would fail confusingly at test time.
std::string ServerUrl() {
    if (const char* env = std::getenv("REMCOTE_SIGNALING_URL")) return env;
    if (const char* env = std::getenv("REMCOTE_SERVER")) return env;
    std::ifstream cfg("remcote-server.txt");
    if (cfg) {
        std::string url;
        std::getline(cfg, url);
        while (!url.empty() && (url.back() == '\r' || url.back() == ' ')) url.pop_back();
        if (!url.empty()) return url;
    }
    return "";
}

const char* PassFail(bool ok) { return ok ? "PASS" : "FAIL"; }

// Real startup checks — nothing here is ever faked. DXGI/D3D11/GPU checks
// happen when CaptureEngine initializes; signaling reachability is proven by
// the actual WebSocket registration (logged by DeviceRegistration).
bool RunPreflight(const std::string& serverUrl) {
    std::printf("ReMCote Host Preflight\n");

    // Windows version (RtlGetVersion — unaffected by manifest compatibility lies)
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    bool winOk = false;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            winOk = fn(&ver) == 0 && ver.dwMajorVersion >= 10;
        }
    }
    std::printf("  Windows %lu.%lu build %-12lu %s\n", ver.dwMajorVersion, ver.dwMinorVersion,
                ver.dwBuildNumber, PassFail(winOk));

    // NVENC runtime — the encoder API ships with the NVIDIA driver.
    HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    std::printf("  NVENC H.264 (nvEncodeAPI64.dll)  %s\n", PassFail(nvenc != nullptr));
    if (nvenc) FreeLibrary(nvenc);

    // D3D11 runtime presence (device creation itself is done by CaptureEngine).
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    std::printf("  Direct3D 11                      %s\n", PassFail(d3d != nullptr));
    if (d3d) FreeLibrary(d3d);

    const bool urlOk = serverUrl.rfind("ws://", 0) == 0 || serverUrl.rfind("wss://", 0) == 0;
    std::printf("  Signaling URL configured         %s\n", PassFail(urlOk));
    if (!urlOk) {
        std::fprintf(stderr,
                     "\nNo signaling server configured. Set it with either:\n"
                     "  $env:REMCOTE_SIGNALING_URL = \"wss://<your-remcote-server>/api/ws\"\n"
                     "or create remcote-server.txt next to ReMCoteHost.exe containing that URL.\n");
        return false;
    }
    std::printf("  (GPU / DXGI capture checked during capture init below)\n");
    return true;
}

// Session gating: capture/encode run only after the Host clicks ALLOW and a
// WebRTC peer exists. This is the consent boundary (spec §4).
struct HostApp {
    HostUI ui;
    CaptureEngine capture;
    EncoderEngine encoder;
    InputEngine input;
    PerformanceMonitor perf;
    std::unique_ptr<WebRtcTransport> rtc;
    std::unique_ptr<DeviceRegistration> registration;
    std::atomic<bool> approved{false};
    std::atomic<bool> pipelineRunning{false};
    std::string activeSessionId;
    // First-frame diagnostics (spec: log state transitions only, never per frame)
    std::atomic<bool> loggedFirstCapture{false};
    std::atomic<bool> loggedFirstEncode{false};

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
            pipelineRunning = false;
            return;
        }
        encoder.SetOutputCallback([this](const EncodedFrame& f) {
            if (!loggedFirstEncode.exchange(true))
                std::printf("[VIDEO] First frame encoded (%zu bytes, keyframe=%d)\n", f.size,
                            f.keyframe ? 1 : 0);
            perf.OnEncodeFrame(f.encodeDurationUs);
            rtc->SendFrame(f);
        });
        capture.Start(
            [this](ID3D11Texture2D* tex, int64_t captureUs) {
                if (!loggedFirstCapture.exchange(true))
                    std::printf("[VIDEO] First frame captured\n");
                const int64_t t0 = NowUs();
                if (!encoder.SubmitFrame(tex, captureUs)) {
                    perf.OnFrameDropped(); // encoder busy — newest-wins drop
                } else {
                    perf.OnCaptureFrame(NowUs() - t0);
                }
            },
            [this](float x, float y, bool visible) { rtc->SendCursor(x, y, visible); });
        input.Start();
        ui.OnSessionActive(true);
        ui.SetStatusLine("Remote session active");
    }

    void StopPipeline(const std::string& reason) {
        if (!pipelineRunning.exchange(false)) {
            approved = false;
            return;
        }
        capture.Stop();
        input.Stop();
        encoder.Shutdown();
        approved = false;
        if (rtc) rtc->CloseAll();
        if (registration && !activeSessionId.empty())
            registration->NotifySessionClosed(activeSessionId, reason);
        activeSessionId.clear();
        ui.OnSessionActive(false);
        ui.SetStatusLine("Ready for connection");
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    SetupLogging();
    std::printf("[REMCOTE] Starting\n");

    const std::string serverUrl = ServerUrl();
    if (!RunPreflight(serverUrl)) {
        MessageBoxW(nullptr,
                    L"No signaling server configured.\n\n"
                    L"Set REMCOTE_SIGNALING_URL (or create remcote-server.txt next to the exe)\n"
                    L"to your ReMCote server, e.g. wss://your-app.replit.app/api/ws",
                    L"ReMCote Host", MB_ICONERROR);
        return 1;
    }

    auto app = std::make_unique<HostApp>();

    if (!app->capture.Initialize()) {
        std::printf("  DXGI Desktop Duplication         FAIL\n");
        MessageBoxW(nullptr, L"Failed to initialize screen capture (DXGI Desktop Duplication).",
                    L"ReMCote Host", MB_ICONERROR);
        return 1;
    }
    std::printf("  DXGI Desktop Duplication         PASS\n");
    std::printf("[GPU] %s\n", app->capture.GpuName().c_str());
    std::printf("[CAPTURE] DXGI initialized (%dx%d @ %d Hz)\n", app->capture.Width(),
                app->capture.Height(), app->capture.RefreshHz());

    HostCapabilities caps;
    caps.h264 = true;
    caps.gpuName = app->capture.GpuName();
    caps.encoderName = "NVENC H.264";
    caps.desktopWidth = app->capture.Width();
    caps.desktopHeight = app->capture.Height();
    caps.desktopHz = app->capture.RefreshHz();

    if (!app->ui.Create(hInstance)) return 1;
    app->ui.SetGpuInfo(caps.gpuName, "NVENC Ready");
    app->ui.SetStatusLine("Connecting to ReMCote...");

    std::printf("[SIGNALING] Connecting to %s\n", serverUrl.c_str());
    app->registration = std::make_unique<DeviceRegistration>(serverUrl, caps);

    app->registration->SetOnRegistered(
        [&app](const std::string& id, const std::vector<std::string>& iceServers) {
            app->ui.SetDeviceId(id);
            app->ui.SetOnline(true);
            app->ui.SetStatusLine("Ready for connection");
            app->rtc = std::make_unique<WebRtcTransport>(app->input, iceServers);
            app->rtc->SetSignalOut([&app](const std::string& sid, const nlohmann::json& payload) {
                app->registration->SendSignal(sid, payload);
            });
            app->rtc->SetBitrateRequest([&app](int kbps) { app->encoder.SetBitrate(kbps); });
            app->rtc->SetKeyframeRequest([&app] { app->encoder.RequestKeyframe(); });
            app->rtc->SetSessionEnded([&app](const std::string&) {
                app->StopPipeline("Peer disconnected");
            });
        });

    // Incoming request: show the consent prompt. Nothing streams yet.
    app->registration->SetOnConnectRequest(
        [&app](const std::string& sessionId, const std::string& desc) {
            std::printf("[SESSION] Request received (%s)\n", desc.c_str());
            app->ui.ShowConnectionRequest(sessionId, desc);
        });

    // Host clicked ALLOW / DECLINE.
    app->ui.SetApprovalDecision([&app](const std::string& sessionId, bool accept) {
        app->registration->RespondToConnect(sessionId, accept);
        if (accept) {
            std::printf("[SESSION] Host approved\n");
            app->approved = true;
            app->activeSessionId = sessionId;
            app->ui.SetStatusLine("Negotiating direct connection...");
            app->StartPipeline();
        } else {
            app->ui.SetStatusLine("Ready for connection");
        }
    });

    // SDP/ICE from the client — only for the specifically approved session.
    // Signals for any other sessionId are dropped (consent boundary, spec §4).
    app->registration->SetOnPeerSignal(
        [&app](const std::string& sessionId, const nlohmann::json& payload) {
            if (!app->approved || !app->rtc) return;
            if (sessionId != app->activeSessionId) {
                std::fprintf(stderr, "[SESSION] Dropped signal for non-approved session\n");
                return;
            }
            app->rtc->HandlePeerSignal(sessionId, payload);
        });

    app->registration->SetOnSessionEnded(
        [&app](const std::string&, const std::string& reason) { app->StopPipeline(reason); });

    app->ui.SetStopSession([&app] { app->StopPipeline("Host stopped the session"); });
    app->ui.SetExitRequest([&app] {
        app->StopPipeline("Host closed ReMCote");
        if (app->registration) app->registration->Stop();
    });

    app->ui.SetTelemetryProvider([&app] { return app->perf.Sample(); });

    app->registration->Start();

    const int rc = app->ui.RunMessageLoop();
    app->StopPipeline("Host exited");
    return rc;
}
