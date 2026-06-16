#pragma once

/**
 * @file js_engine.h
 * @brief Engine-neutral JavaScript execution boundary.
 */

#include <atomic>
#include <memory>
#include <string_view>

#include "wl2/errors.h"

namespace wl2 {

class Runtime;

/**
 * @brief Engine-neutral JavaScript execution boundary.
 *
 * Core runtime code depends on this interface instead of directly including
 * V8 or QuickJS headers. Per-engine binding adapters live behind this
 * boundary and are selected by CMake through `WL2_JS_ENGINE`.
 *
 * Embedders normally use Runtime instead of constructing JsEngine directly.
 *
 * @code{.cpp}
 * auto engine = wl2::createConfiguredJsEngine();
 * auto result = engine->runModule(runtime, "inline.js",
 *     "console.log('hello from wl2');");
 * @endcode
 */
class JsEngine {
public:
    /// Destroy the engine implementation.
    virtual ~JsEngine() = default;

    /**
     * @brief Execute one ES module source string.
     * @param runtime Host runtime services available to the engine.
     * @param specifier Logical source name used for diagnostics and resolving.
     * @param source JavaScript module source.
     * @param cancel Optional cooperative cancellation flag. When non-null and it
     * becomes true during execution, the engine aborts the script at the next
     * safe checkpoint and returns an error. Used for graceful-then-forced thread
     * shutdown. Native (non-JS) calls in progress are not interruptible.
     * @return Script exit code on success, or an Error if execution failed.
     */
    virtual Result<int> runModule(Runtime& runtime, std::string_view specifier, std::string_view source,
        const std::atomic<bool>* cancel = nullptr) = 0;
};

/**
 * @brief Create the JavaScript engine selected at build time.
 * @return A new engine implementation instance.
 */
std::unique_ptr<JsEngine> createConfiguredJsEngine();

/**
 * @brief Name of the JavaScript engine selected at build time.
 * @return Static engine name such as `"quickjs"` or `"v8"`.
 */
const char* configuredJsEngineName();

} // namespace wl2
