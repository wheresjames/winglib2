#include "wl2/wl2.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(const std::string& message) {
    std::cerr << "runtime test failed: " << message << '\n';
    return 1;
}

// Exercise the wl2.runtime environment policy and argv forwarding through the
// engine directly so the enabled allow-list path is covered. The wl2 command
// line only ever runs with environment access disabled.
int run_runtime_tests() {
    ::setenv("WL2_RUNTIME_ENV_TEST", "allowed-value", 1);

    wl2::RuntimeOptions options;
    options.scriptArgs = {"first", "second"};
    options.allowEnvironment = true;
    options.environmentAllowList = {"WL2_RUNTIME_ENV_TEST"};

    wl2::Runtime runtime{std::move(options)};
    auto engine = wl2::createConfiguredJsEngine();

    static const char* source = R"JS(
function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(wl2.runtime.argv.length === 2, "argv was not forwarded");
assert(wl2.runtime.argv[0] === "first" && wl2.runtime.argv[1] === "second", "argv mismatch");

// Allow-listed name returns the host value.
assert(wl2.runtime.env("WL2_RUNTIME_ENV_TEST") === "allowed-value",
  "allow-listed env var did not return its value");

// A name that is not on the allow-list is rejected even when access is enabled.
let blocked = false;
try {
  wl2.runtime.env("PATH");
} catch (error) {
  blocked = true;
}
assert(blocked, "non-allow-listed env var was not rejected");
)JS";

    auto result = engine->runModule(runtime, "runtime-env-test.js", source);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "runtime ok\n";
    return 0;
}

// Network capability policy: denied by default, empty allow-list denies, and
// allow-list entries permit matching endpoints with stable error codes.
int run_capability_tests() {
    // Denied by default.
    {
        wl2::Runtime runtime;
        auto connect = runtime.authorizeNetworkConnect("example.com", 443);
        if (connect || connect.error().code() != "network_connect_denied") {
            return fail("network connect should be denied by default");
        }
        auto listen = runtime.authorizeNetworkListen("127.0.0.1", 8080);
        if (listen || listen.error().code() != "network_listen_denied") {
            return fail("network listen should be denied by default");
        }
        auto graphics = runtime.authorizeGraphics();
        if (graphics || graphics.error().code() != "graphics_denied") {
            return fail("graphics should be denied by default");
        }
        auto shm = runtime.authorizeSharedMemory("/wl2/test");
        if (shm || shm.error().code() != "shared_memory_denied") {
            return fail("shared memory should be denied by default");
        }
    }

    // Enabled but empty allow-list denies all.
    {
        wl2::RuntimeOptions options;
        options.allowNetwork = true;
        wl2::Runtime runtime{std::move(options)};
        if (runtime.authorizeNetworkConnect("example.com", 443)) {
            return fail("empty allow-list should deny all connections");
        }
    }

    // Allow-list entries permit matching endpoints only.
    {
        wl2::RuntimeOptions options;
        options.allowNetwork = true;
        options.networkAllowList = {"example.com:443", "metrics", "*:9000"};
        wl2::Runtime runtime{std::move(options)};
        if (!runtime.authorizeNetworkConnect("example.com", 443)) {
            return fail("exact host:port should be permitted");
        }
        if (runtime.authorizeNetworkConnect("example.com", 80)) {
            return fail("non-listed port should be denied");
        }
        if (!runtime.authorizeNetworkConnect("metrics", 1234)) {
            return fail("host-only entry should permit any port");
        }
        if (!runtime.authorizeNetworkConnect("anything", 9000)) {
            return fail("*:port entry should permit any host on that port");
        }
        if (runtime.authorizeNetworkConnect("other", 443)) {
            return fail("unlisted host should be denied");
        }
    }

    // Listening uses its own switch and allow-list.
    {
        wl2::RuntimeOptions options;
        options.allowListening = true;
        options.listenAllowList = {"*"};
        wl2::Runtime runtime{std::move(options)};
        if (!runtime.authorizeNetworkListen("0.0.0.0", 8080)) {
            return fail("wildcard listen entry should permit any endpoint");
        }
        // Connecting is still denied: the switches are independent.
        if (runtime.authorizeNetworkConnect("0.0.0.0", 8080)) {
            return fail("listen policy must not grant connect");
        }
    }

    // Graphics is an independent host-resource switch.
    {
        wl2::RuntimeOptions options;
        options.allowGraphics = true;
        wl2::Runtime runtime{std::move(options)};
        if (!runtime.authorizeGraphics()) {
            return fail("allowGraphics should permit graphics");
        }
        if (runtime.authorizeUi()) {
            return fail("allowGraphics must not grant UI access");
        }
    }

    // Shared memory requires both the switch and a matching prefix.
    {
        wl2::RuntimeOptions options;
        options.allowSharedMemory = true;
        options.sharedMemoryAllowList = {"/wl2/run-123/"};
        wl2::Runtime runtime{std::move(options)};
        if (!runtime.authorizeSharedMemory("/wl2/run-123/3d/frames")) {
            return fail("matching shared-memory prefix should be permitted");
        }
        if (runtime.authorizeSharedMemory("/wl2/run-124/3d/frames")) {
            return fail("non-matching shared-memory prefix should be denied");
        }
    }

    std::cout << "runtime capabilities ok\n";
    return 0;
}

} // namespace

int wl2_runtime_tests_entry() {
    if (int rc = run_runtime_tests(); rc != 0) {
        return rc;
    }
    return run_capability_tests();
}
