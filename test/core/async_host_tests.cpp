#include "wl2/async_host.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int fail(const std::string& message) {
    std::cerr << "async host test failed: " << message << '\n';
    return 1;
}

// A completion posted from a worker thread runs when the host is drained, and
// operation counting tracks the work as outstanding until it ends.
int post_and_drain() {
    wl2::AsyncHost host;
    std::atomic<int> ran{0};

    host.beginOperation();
    if (!host.hasPendingWork() || host.outstandingOperations() != 1) {
        return fail("operation was not counted as outstanding");
    }

    std::thread worker([&host, &ran] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.post([&ran] { ran.fetch_add(1); });
        host.endOperation();
    });

    // Mimic an event loop: wait for the completion, then drain it.
    while (host.hasPendingWork()) {
        host.waitForWork(std::chrono::milliseconds(50));
        host.drain();
    }
    worker.join();

    if (ran.load() != 1) {
        return fail("completion did not run exactly once");
    }
    if (host.hasPendingWork() || host.outstandingOperations() != 0) {
        return fail("work remained outstanding after completion");
    }
    return 0;
}

// Completions run in the order they were posted, on the draining thread.
int drain_order() {
    wl2::AsyncHost host;
    std::string order;
    host.post([&order] { order += "a"; });
    host.post([&order] { order += "b"; });
    host.post([&order] { order += "c"; });
    const std::size_t ran = host.drain();
    if (ran != 3 || order != "abc") {
        return fail("completions did not run in order: " + order);
    }
    return 0;
}

// shutdown() invokes hooks (which stop worker threads and settle pending work)
// and drains what they post; workers do not outlive shutdown.
int shutdown_runs_hooks_and_settles() {
    wl2::AsyncHost host;
    std::atomic<bool> workerStopped{false};
    std::atomic<int> settled{0};
    std::thread worker;

    // A module-style native operation: a worker thread that never completes on
    // its own and a shutdown hook that stops it and settles the promise.
    host.beginOperation();
    std::atomic<bool> stop{false};
    worker = std::thread([&stop] {
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    host.registerShutdownHook([&] {
        stop.store(true);
        if (worker.joinable()) {
            worker.join();
        }
        workerStopped.store(true);
        // Settle the pending operation with an error completion.
        host.post([&settled] { settled.fetch_add(1); });
        host.endOperation();
    });

    host.shutdown();

    if (!host.isShuttingDown()) {
        return fail("host did not report shutting down");
    }
    if (!workerStopped.load()) {
        return fail("shutdown hook did not stop the worker thread");
    }
    if (settled.load() != 1) {
        return fail("pending operation was not settled during shutdown");
    }
    if (host.outstandingOperations() != 0) {
        return fail("operation still outstanding after shutdown");
    }

    // shutdown() is idempotent.
    host.shutdown();
    return 0;
}

} // namespace

int wl2_async_host_tests_entry() {
    if (int rc = post_and_drain(); rc != 0) {
        return rc;
    }
    if (int rc = drain_order(); rc != 0) {
        return rc;
    }
    if (int rc = shutdown_runs_hooks_and_settles(); rc != 0) {
        return rc;
    }
    std::cout << "async_host ok\n";
    return 0;
}
