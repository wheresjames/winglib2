# The wl2_core runtime library: sources, public includes, the selected JS engine
# backend, optional libmembus, and the threads/dlopen dependencies.
#
# Included from the root CMakeLists.txt (same scope) after dependencies are
# resolved, so CMAKE_CURRENT_SOURCE_DIR points at the project root.

# Core runtime library shared by the wl2 runner, modules, and embedders.
add_library(wl2_core
    src/core/app_store.cpp
    src/core/async_host.cpp
    src/core/buffer.cpp
    src/core/crash_report.cpp
    src/core/hash.cpp
    src/core/manifest.cpp
    src/core/membus.cpp
    src/core/module_deps.cpp
    src/core/module_loader.cpp
    src/core/module_resolver.cpp
    src/core/module_store.cpp
    src/core/resources.cpp
    src/core/runtime.cpp
    src/core/thread_tree.cpp)

target_include_directories(wl2_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# WL2_VERSION is the canonical version seen by C++ (module ABI checks, wl2 version).
target_compile_definitions(wl2_core
    PUBLIC
        WL2_VERSION="${PROJECT_VERSION}")
target_compile_definitions(wl2_core
    PRIVATE
        WL2_BUILD="${APPBUILD}")
target_compile_features(wl2_core PUBLIC cxx_std_20)

# Optional libmembus integration (shared-memory bus); compiled out when absent.
wl2_find_libmembus()
if(WL2_HAVE_LIBMEMBUS)
    target_link_libraries(wl2_core PUBLIC ${WL2_LIBMEMBUS_TARGET})
    target_compile_definitions(wl2_core PUBLIC WL2_HAVE_LIBMEMBUS=1)
else()
    target_compile_definitions(wl2_core PUBLIC WL2_HAVE_LIBMEMBUS=0)
endif()

# Compile and link the selected engine backend (exactly one of V8 / QuickJS).
message(STATUS "Winglib2 JavaScript engine: ${WL2_JS_ENGINE}")
if(WL2_JS_ENGINE STREQUAL "v8")
    wl2_find_v8()
    target_sources(wl2_core PRIVATE src/js/v8_engine.cpp)
    target_compile_definitions(wl2_core PUBLIC WL2_HAVE_V8=1 WL2_JS_ENGINE_V8=1)
    target_link_libraries(wl2_core PUBLIC v8)
else()
    wl2_find_quickjs()
    target_sources(wl2_core PRIVATE src/js/quickjs_engine.cpp)
    target_compile_definitions(wl2_core PUBLIC WL2_JS_ENGINE_QUICKJS=1)
    if(WL2_HAVE_QUICKJS)
        target_compile_definitions(wl2_core PUBLIC WL2_HAVE_QUICKJS=1)
        target_link_libraries(wl2_core PUBLIC quickjs)
    else()
        target_compile_definitions(wl2_core PUBLIC WL2_HAVE_QUICKJS=0)
    endif()
endif()

# Threads and the dynamic loader (dlopen) are required for native modules.
find_package(Threads REQUIRED)
target_link_libraries(wl2_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
