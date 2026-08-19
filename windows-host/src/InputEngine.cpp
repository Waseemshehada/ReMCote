#include "InputEngine.h"

#include <windows.h>
#include <cstdio>

namespace remcote {

void InputEngine::Start() {
    running_ = true;
    thread_ = std::thread([this] { InjectLoop(); });
}

void InputEngine::Stop() {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void InputEngine::Enqueue(const InputEvent& ev) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ev.kind == InputEvent::Kind::PointerMove) {
            latestMove_ = ev;   // newest-wins; stale motion is worthless
            hasMove_ = true;
        } else {
            reliableQueue_.push(ev); // clicks and keys must never be lost
        }
    }
    cv_.notify_one();
}

void InputEngine::InjectLoop() {
    // Input has priority over video (spec §19).
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (running_) {
        InputEvent ev;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_ || hasMove_ || !reliableQueue_.empty(); });
            if (!running_) break;
            // Discrete events first to preserve click/move ordering fairness,
            // then the single freshest pointer position.
            if (!reliableQueue_.empty()) {
                ev = reliableQueue_.front();
                reliableQueue_.pop();
                have = true;
            } else if (hasMove_) {
                ev = latestMove_;
                hasMove_ = false;
                have = true;
            }
        }
        if (have) Inject(ev);
    }
}

static void InjectMouseAt(float x, float y, DWORD extraFlags, DWORD mouseData = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>(x * 65535.0f);
    in.mi.dy = static_cast<LONG>(y * 65535.0f);
    in.mi.mouseData = mouseData;
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK | extraFlags;
    SendInput(1, &in, sizeof(in));
}

void InputEngine::Inject(const InputEvent& ev) {
    switch (ev.kind) {
    case InputEvent::Kind::PointerMove:
        InjectMouseAt(ev.x, ev.y, 0);
        break;

    case InputEvent::Kind::MouseButton: {
        DWORD flag = 0;
        DWORD data = 0;
        switch (ev.button) {
        case 0: flag = ev.down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: flag = ev.down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: flag = ev.down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 3: flag = ev.down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON1; break;
        case 4: flag = ev.down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; data = XBUTTON2; break;
        default: return;
        }
        InjectMouseAt(ev.x, ev.y, flag, data);
        break;
    }

    case InputEvent::Kind::Wheel: {
        INPUT in{};
        in.type = INPUT_MOUSE;
        // Browser deltaY is positive scrolling down; Windows wheel positive is up.
        if (ev.dy != 0) {
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = static_cast<DWORD>(-ev.dy);
            SendInput(1, &in, sizeof(in));
        }
        if (ev.dx != 0) {
            in.mi.mouseData = static_cast<DWORD>(ev.dx);
            in.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            SendInput(1, &in, sizeof(in));
        }
        break;
    }

    case InputEvent::Kind::Key: {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        uint32_t sc = ev.scanCode;
        if (sc == 0) {
            // Unmapped code — nothing safe to inject.
            std::fprintf(stderr, "[INPUT] unmapped key code: %s\n", ev.code.c_str());
            return;
        }
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (sc >= 0xE000) {
            in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            sc &= 0xFF;
        }
        in.ki.wScan = static_cast<WORD>(sc);
        if (!ev.down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
        break;
    }
    }
}

} // namespace remcote
