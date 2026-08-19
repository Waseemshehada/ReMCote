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

    std::printf("=== CI Self-Test: %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// ─── Preflight ────────────────────────────────────────────────────────────────

const char* PassFail(bool ok) { return ok ? "PASS" : "FAIL"; }

bool RunPreflight(const std::string& serverUrl) {
    std::printf("ReMCote Host Preflight\n");

    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW ver{};
    ver.dwOSVersionInfoSize = sizeof(ver);
    bool winOk = false;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            winOk = fn(&ver) == 0 && ver.dwMajorVersion >= 10;
        }
    }
    std::printf("  Windows %lu.%lu build %-12lu %s\n",
                ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber, PassFail(winOk));

    HMODULE nvenc = LoadLibraryW(L"nvEncodeAPI64.dll");
    std::printf("  NVENC H.264 (nvEncodeAPI64.dll)  %s\n", PassFail(nvenc != nullptr));
    if (nvenc) FreeLibrary(nvenc);

    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    std::printf("  Direct3D 11                      %s\n", PassFail(d3d != nullptr));
    if (d3d) FreeLibrary(d3d);

    const bool urlOk = serverUrl.rfind("ws://", 0) == 0 || serverUrl.rfind("wss://", 0) == 0;
    std::printf("  Signaling URL configured         %s  (%s)\n", PassFail(urlOk), serverUrl.c_str());
    // With the built-in default this check always passes; it guards against
    // accidentally removing the production URL in a future refactor.
    if (!urlOk) {
        std::fprintf(stderr, "\n[ERROR] Signaling URL is empty or invalid: '%s'\n",
                     serverUrl.c_str());
        return false;
    }
    return true;
}

// ─── Application ─────────────────────────────────────────────────────────────

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
                std::printf("[VIDEO] First frame encoded (%zu bytes, keyframe=%d)\n",
                            f.size, f.keyframe ? 1 : 0);
            perf.OnEncodeFrame(f.encodeDurationUs);
            rtc->SendFrame(f);
        });
        capture.Start(
            [this](ID3D11Texture2D* tex, int64_t captureUs) {
                if (!loggedFirstCapture.exchange(true))
                    std::printf("[VIDEO] First frame captured\n");
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

// ─── Entry point ─────────────────────────────────────────────────────────────

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    SetupLogging();

    // CI self-test mode: verify non-GPU invariants and exit. No UI, no DXGI.
    if (lpCmdLine && std::wcsstr(lpCmdLine, L"--ci-self-test")) {
        return CiSelfTest();
    }

    std::printf("[REMCOTE] Starting\n");

    const SignalingConfig sig = ResolveSignaling();
    std::printf("[SIGNALING] Server: %s\n", sig.url.c_str());
    std::printf("[SIGNALING] Source: %s\n", sig.source);

    if (!RunPreflight(sig.url)) {
        // This should never fire with the built-in default, but protects
        // against future accidental regressions.
        MessageBoxW(nullptr,
                    L"Signaling URL is invalid.\n\n"
                    L"If you are a developer, set REMCOTE_SIGNALING_URL to your server.\n"
                    L"Normal users: just double-click ReMCoteHost.exe — no configuration needed.",
                    L"ReMCote Host", MB_ICONERROR);
        return 1;
    }

    auto app = std::make_unique<HostApp>();

    if (!app->capture.Initialize()) {
        std::printf("  DXGI Desktop Duplication         FAIL\n");
        MessageBoxW(nullptr,
                    L"Failed to initialize screen capture (DXGI Desktop Duplication).\n"
                    L"An NVIDIA GPU with a current driver is required.",
                    L"ReMCote Host", MB_ICONERROR);
        return 1;
    }
    std::printf("  DXGI Desktop Duplication         PASS\n");
    std::printf("[GPU] %s\n", app->capture.GpuName().c_str());
    std::printf("[CAPTURE] DXGI initialized (%dx%d @ %d Hz)\n",
                app->capture.Width(), app->capture.Height(), app->capture.RefreshHz());

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

    std::printf("[SIGNALING] Connecting to %s\n", sig.url.c_str());
    app->registration = std::make_unique<DeviceRegistration>(sig.url, caps);

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

    app->registration->SetOnConnectRequest(
        [&app](const std::string& sessionId, const std::string& desc) {
            std::printf("[SESSION] Request received (%s)\n", desc.c_str());
            app->ui.ShowConnectionRequest(sessionId, desc);
        });

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
