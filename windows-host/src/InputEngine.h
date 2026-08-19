#pragma once
// Input injection via SendInput (spec §11). Runs at TIME-CRITICAL thread
// priority — input latency wins over everything else (spec §19).

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <cstdint>

namespace remcote {

struct InputEvent {
    enum class Kind { PointerMove, MouseButton, Wheel, Key } kind;
    float x = 0, y = 0;      // normalized 0..1
    int button = 0;          // 0 left, 1 middle, 2 right, 3 back, 4 forward
    bool down = false;
    float dx = 0, dy = 0;    // wheel deltas in viewer pixels
    uint32_t scanCode = 0;   // PS/2 set-1; >=0xE000 means extended
    std::string code;        // KeyboardEvent.code fallback when scanCode == 0
};

class InputEngine {
public:
    void Start();
    void Stop();

    // Called from WebRTC data-channel threads. Pointer moves collapse to the
    // newest value; discrete events (buttons/keys/wheel) are never dropped.
    void Enqueue(const InputEvent& ev);

private:
    void InjectLoop();
    void Inject(const InputEvent& ev);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<InputEvent> reliableQueue_;
    InputEvent latestMove_{};
    bool hasMove_ = false;
};

} // namespace remcote
