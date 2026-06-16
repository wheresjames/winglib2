// End-to-end test of the async host support through the real engine event loop:
// a native module returns a JavaScript promise that is resolved from a worker
// thread via Runtime::async(); the engine run loop pumps the completion and the
// awaiting script observes the value. Proves a native async module can share the
// host's promise/completion/shutdown machinery.
#include "wl2/wl2.h"

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <chrono>

#include <quickjs.h>

namespace {

int fail(const std::string& message) {
    std::cerr << "async integration test failed: " << message << '\n';
    return 1;
}

// Worker threads spawned by the test module, joined when the runtime shuts down
// so they never outlive it.
struct Workers {
    std::mutex mutex;
    std::vector<std::thread> threads;
    void joinAll() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads.clear();
    }
};

Workers& workers() {
    static Workers instance;
    return instance;
}

// pause(): returns a promise resolved with 42 from a worker thread.
JSValue async_pause(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "runtime unavailable");
    }

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JS_FreeValue(ctx, funcs[1]);  // reject is unused in this fixture
    JSValue resolve = funcs[0];   // ownership transferred to the completion

    runtime->async().beginOperation();
    std::lock_guard<std::mutex> lock(workers().mutex);
    workers().threads.emplace_back([runtime, ctx, resolve] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        // post() runs the completion on the JavaScript thread, where touching
        // the context and resolving the promise is safe.
        runtime->async().post([ctx, resolve] {
            JSValue value = JS_NewInt32(ctx, 42);
            JSValue r = JS_Call(ctx, resolve, JS_UNDEFINED, 1, &value);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, value);
            JS_FreeValue(ctx, resolve);
        });
        runtime->async().endOperation();
    });
    return promise;
}

int init_async_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "pause", JS_NewCFunction(ctx, async_pause, "pause", 0));
    return 0;
}

void* async_module_factory(void* context, const char* moduleName) {
    auto* ctx = static_cast<JSContext*>(context);
    if (auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx))) {
        runtime->async().registerShutdownHook([] { workers().joinAll(); });
    }
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_async_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "pause");
    return module;
}

int run() {
    wl2::Runtime runtime;
    runtime.registerQuickJsModule("wl2:asynctest", &async_module_factory);

    auto engine = wl2::createConfiguredJsEngine();
    static const char* source = R"JS(
import { pause } from "wl2:asynctest";
const value = await pause();
if (value !== 42) {
  throw new Error("unexpected async result: " + value);
}
)JS";

    auto result = engine->runModule(runtime, "async-integration.js", source);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }
    std::cout << "async_integration ok\n";
    return 0;
}

} // namespace

int wl2_async_integration_tests_entry() {
    return run();
}

#else  // !WL2_HAVE_QUICKJS

int wl2_async_integration_tests_entry() {
    std::cout << "async_integration skipped (QuickJS unavailable)\n";
    return 0;
}

#endif
