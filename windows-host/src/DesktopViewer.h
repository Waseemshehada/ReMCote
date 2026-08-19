#pragma once

#include <deque>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include <nlohmann/json.hpp>

#include "D3D11Renderer.h"
#include "H264Decoder.h"
#include "ViewerSignaling.h"
#include "ViewerTransport.h"

namespace remcote {

class DesktopViewer {
public:
    DesktopViewer() = default;
    ~DesktopViewer();

    DesktopViewer(const DesktopViewer&) = delete;
    DesktopViewer& operator=(const DesktopViewer&) = delete;

    bool Open(HINSTANCE hInstance, const std::string& signalingUrl,
              const std::string& initialDeviceId = {});
    void Close();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    LRESULT HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleVideoMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void Layout();
    void StartSession();
    void StartTransport(std::vector<IceServerCfg> iceServers);
    void StopSession(const std::string& reason, bool notifyPeer);
    void StartDecodeWorker();
    void StopDecodeWorker();
    void QueueVideoFrame(const ViewerFrame& frame);
    void SetStatus(const std::wstring& status, bool error = false);
    void ShowVideo(bool visible);
    void ToggleFullscreen();

    bool NormalizeVideoPoint(LPARAM lParam, float& x, float& y) const;
    void QueueState(ViewerSessionState state, const std::string& message);
    void QueuePeerSignal(const nlohmann::json& payload);

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND deviceLabel_ = nullptr;
    HWND deviceEdit_ = nullptr;
    HWND connectButton_ = nullptr;
    HWND disconnectButton_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND videoSurface_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;

    std::string signalingUrl_;
    std::unique_ptr<ViewerSignaling> signaling_;
    std::unique_ptr<ViewerTransport> transport_;
    H264Decoder decoder_;
    D3D11Renderer renderer_;

    mutable std::mutex pendingMutex_;
    ViewerSessionState pendingState_{ViewerSessionState::Offline};
    std::string pendingMessage_;
    ViewerHostCapabilities capabilities_;
    std::vector<IceServerCfg> pendingIceServers_;
    std::deque<nlohmann::json> pendingPeerSignals_;

    std::mutex decodeMutex_;
    std::condition_variable decodeCv_;
    std::deque<ViewerFrame> decodeQueue_;
    std::thread decodeThread_;
    bool decodeRunning_ = false;
    int64_t decodeTimestamp100ns_ = 0;

    bool videoVisible_ = false;
    bool fullscreen_ = false;
    WINDOWPLACEMENT previousPlacement_{sizeof(WINDOWPLACEMENT)};
    DWORD previousStyle_ = 0;
};

} // namespace remcote