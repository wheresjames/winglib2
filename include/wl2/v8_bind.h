#pragma once

/**
 * @file v8_bind.h
 * @brief Legacy V8 execution helper retained for the optional V8 backend.
 */

#include <string_view>

namespace wl2 {

class Runtime;

/**
 * @brief Execute one JavaScript source string in a plain V8 context.
 *
 * Most embedders should call Runtime::runModule instead. This helper is kept
 * for the V8 backend path and early binding-layer experiments.
 *
 * @code{.cpp}
 * int exitCode = wl2::run_v8_script(runtime, "inline.js",
 *     "console.log('hello from V8');");
 * @endcode
 *
 * @param runtime Host runtime services available to the script.
 * @param name Logical source name used for diagnostics.
 * @param source JavaScript source to execute.
 * @return Script exit code.
 */
int run_v8_script(Runtime& runtime, std::string_view name, std::string_view source);

} // namespace wl2
