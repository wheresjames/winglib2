#pragma once

/**
 * @file async_host.h
 * @brief Reusable host support for native asynchronous module work.
 *
 * AsyncHost gives native modules one shared place to schedule completions back
 * onto the JavaScript runtime thread, to keep the engine event loop alive while
 * native work is outstanding, and to be notified when the runtime shuts down so
 * worker threads do not outlive it. Modules that own worker threads use this
 * instead of each re-implementing the queue, wakeup, and shutdown machinery.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace wl2 {

/**
 * @brief Thread-safe scheduler for native async completions and shutdown.
 *
 * A native module typically: marks an operation outstanding with
 * beginOperation(), does work on its own thread, then post()s a completion that
 * runs on the JavaScript thread (where it may resolve or reject a promise) and
 * calls endOperation(). The engine event loop drains posted completions and
 * keeps running while operations are outstanding.
 *
 * post(), beginOperation(), and endOperation() are safe to call from any
 * thread. drain() and the run-loop helpers must run on the JavaScript thread.
 *
 * @code{.cpp}
 * host.beginOperation();
 * std::thread([&host, resolve]{
 *     // ... native work off-thread ...
 *     host.post([resolve]{ resolve(); });   // runs on the JS thread
 *     host.endOperation();
 * }).detach();
 * @endcode
 */
class AsyncHost {
public:
    /// A completion to run on the JavaScript thread.
    using Completion = std::function<void()>;

    /// A hook invoked once during shutdown so a module can stop and join its
    /// worker threads and settle pending operations.
    using ShutdownHook = std::function<void()>;

    AsyncHost() = default;
    ~AsyncHost() = default;

    AsyncHost(const AsyncHost&) = delete;
    AsyncHost& operator=(const AsyncHost&) = delete;

    /**
     * @brief Queue a completion to run on the JavaScript thread. Thread-safe.
     *
     * After shutdown begins, completions are still accepted and drained so that
     * worker threads winding down can settle their promises with errors.
     */
    void post(Completion completion);

    /// Mark one native operation as outstanding so the event loop keeps running.
    void beginOperation() noexcept;

    /// Mark one outstanding native operation as finished.
    void endOperation() noexcept;

    /// Number of outstanding operations.
    std::size_t outstandingOperations() const noexcept;

    /// True when operations are outstanding or completions are queued.
    bool hasPendingWork() const noexcept;

    /**
     * @brief Run all currently queued completions on the calling thread.
     * @return The number of completions executed.
     */
    std::size_t drain();

    /**
     * @brief Block until a completion is queued, shutdown begins, or @p timeout.
     * @return True if there is work to drain (or shutdown began), false on timeout.
     */
    bool waitForWork(std::chrono::milliseconds timeout);

    /// Register a hook invoked once when shutdown() runs.
    void registerShutdownHook(ShutdownHook hook);

    /**
     * @brief Begin shutdown: invoke shutdown hooks, then drain remaining
     * completions. Idempotent. After this, isShuttingDown() is true and any
     * worker threads owned by modules must have been stopped by their hooks.
     */
    void shutdown();

    /// True once shutdown() has begun.
    bool isShuttingDown() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Completion> completions_;
    std::vector<ShutdownHook> shutdownHooks_;
    std::atomic<std::size_t> outstanding_{0};
    bool shuttingDown_ = false;
};

} // namespace wl2
