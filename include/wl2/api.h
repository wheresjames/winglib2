#pragma once

/**
 * @file api.h
 * @brief Convenience include for the public Winglib2 C++ API.
 *
 * Include this header when embedding the runtime from an application that wants
 * access to buffers, resources, modules, runtime execution, and the thread tree.
 */

#include "wl2/buffer.h"
#include "wl2/errors.h"
#include "wl2/js_engine.h"
#include "wl2/manifest.h"
#include "wl2/membus.h"
#include "wl2/module.h"
#include "wl2/module_resolver.h"
#include "wl2/resources.h"
#include "wl2/runtime.h"
#include "wl2/thread_tree.h"
