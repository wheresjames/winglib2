#pragma once

/**
 * @file wl2.h
 * @brief Primary public include for applications embedding Winglib2.
 *
 * This is the shortest include path for embedders:
 *
 * @code{.cpp}
 * #include <wl2/wl2.h>
 *
 * int main() {
 *     wl2::Runtime runtime;
 *     auto result = runtime.runModule("app.js");
 *     return result ? result.value() : 1;
 * }
 * @endcode
 */

#include "wl2/api.h"
