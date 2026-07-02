# Build option and dependency-provider knobs for the wl2_restinio module
# (JS specifier wl2:http). Included both from the module CMakeLists and, after
# installation, from the packaged config, so it must be safe to include more
# than once.
if(COMMAND wl2_module_option)
    # This is expected to be a heavily used capability, so it is ON by default
    # (unlike wl2:asio). WL2_ENABLE_HTTP=OFF gives a dependency-minimized build.
    wl2_module_option(ENABLE DEFAULT ON DOC "Build wl2_restinio HTTP/WebSocket server module (wl2:http)")
endif()

# RESTinio is header-only and not commonly packaged, so the default provider
# fetches a pinned release. Standalone Asio is reused from the wl2:asio fetch
# when present, otherwise located/fetched here.
set(WL2_HTTP_PROVIDER "auto" CACHE STRING "restinio provider: auto, local, fetch, or off")
set_property(CACHE WL2_HTTP_PROVIDER PROPERTY STRINGS auto local fetch off)

# Pinned dependency versions (match RESTinio 0.7.9.1 externals.rb).
set(WL2_HTTP_RESTINIO_TAG "v0.7.9.1" CACHE STRING "RESTinio git tag for the fetch provider")
set(WL2_HTTP_FMT_TAG "12.0.0" CACHE STRING "fmt git tag")
set(WL2_HTTP_LLHTTP_TAG "release/v9.3.0" CACHE STRING "llhttp git tag")
set(WL2_HTTP_EXPECTED_LITE_TAG "v0.8.0" CACHE STRING "expected-lite git tag")

# HTTPS/TLS. Default ON; the provider auto-disables it when OpenSSL is not found.
option(WL2_HTTP_TLS "Enable HTTPS/TLS support in wl2:http (requires OpenSSL)" ON)
