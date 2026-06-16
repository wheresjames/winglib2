#include "wl2/async_host.h"

#include <utility>

namespace wl2 {

void AsyncHost::post(Completion completion) {
    if (!completion) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completions_.push_back(std::move(completion));
    }
    condition_.notify_all();
}

void AsyncHost::beginOperation() noexcept {
    outstanding_.fetch_add(1, std::memory_order_relaxed);
}

void AsyncHost::endOperation() noexcept {
    // Never underflow if endOperation is called more than once.
    std::size_t current = outstanding_.load(std::memory_order_relaxed);
    while (current > 0
        && !outstanding_.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
    condition_.notify_all();
}

std::size_t AsyncHost::outstandingOperations() const noexcept {
    return outstanding_.load(std::memory_order_relaxed);
}

bool AsyncHost::hasPendingWork() const noexcept {
    if (outstanding_.load(std::memory_order_relaxed) > 0) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return !completions_.empty();
}

std::size_t AsyncHost::drain() {
    // Move the queued completions out under the lock, then run them unlocked so a
    // completion may post() more work without deadlocking.
    std::vector<Completion> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(completions_);
    }
    for (auto& completion : ready) {
        completion();
    }
    return ready.size();
}

bool AsyncHost::waitForWork(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, timeout, [this] {
        return !completions_.empty() || shuttingDown_;
    });
    return !completions_.empty() || shuttingDown_;
}

void AsyncHost::registerShutdownHook(ShutdownHook hook) {
    if (!hook) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    shutdownHooks_.push_back(std::move(hook));
}

void AsyncHost::shutdown() {
    std::vector<ShutdownHook> hooks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            return;
        }
        shuttingDown_ = true;
        hooks.swap(shutdownHooks_);
    }
    condition_.notify_all();
    // Hooks run unlocked: they stop worker threads and may post() final
    // completions that settle pending promises with shutdown errors.
    for (auto& hook : hooks) {
        hook();
    }
    // Drain whatever the hooks posted so settlement is not lost.
    drain();
}

bool AsyncHost::isShuttingDown() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return shuttingDown_;
}

} // namespace wl2
