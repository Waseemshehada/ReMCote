#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace remcote {

// Coordinates teardown with callbacks dispatched by third-party worker
// threads. A callback must hold a Lease before touching its owner. Teardown
// stops new leases, closes the third-party object, then waits for active
// callbacks before destroying owner state.
class CallbackFence {
public:
    class Lease {
    public:
        Lease() = default;
        explicit Lease(CallbackFence* owner) : owner_(owner) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept : owner_(other.owner_) {
            other.owner_ = nullptr;
        }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                Release();
                owner_ = other.owner_;
                other.owner_ = nullptr;
            }
            return *this;
        }
        ~Lease() { Release(); }

        explicit operator bool() const { return owner_ != nullptr; }

    private:
        void Release() {
            if (!owner_) return;
            owner_->Release();
            owner_ = nullptr;
        }

        CallbackFence* owner_ = nullptr;
    };

    Lease TryEnter() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) return {};
        ++active_;
        return Lease(this);
    }

    void StopAccepting() {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
    }

    void WaitForIdle() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return active_ == 0; });
    }

private:
    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--active_ == 0) idle_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable idle_;
    bool accepting_ = true;
    std::size_t active_ = 0;
};

} // namespace remcote