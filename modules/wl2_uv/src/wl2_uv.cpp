/**
 * @file wl2_uv.cpp
 * @brief QuickJS bindings for libuv-backed system and network inspection.
 */

#include "wl2_uv/wl2_uv.h"

#include "wl2/runtime.h"

#include <uv.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif
#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_UV_STATIC_MODULE
#define WL2_UV_STATIC_MODULE 0
#endif

namespace {

constexpr const char* UvApi = R"(Exports JavaScript module wl2:uv.

Functions:
  networkInterfaces(options?) -> NetworkInterfaceAddress[]
  localNetworks(options?) -> string[]

Options:
  family: "all" | "IPv4" | "IPv6" = "all"
  includeInternal: boolean = false
  maximumHosts: positive number = 65536 (localNetworks only)

The module currently exposes synchronous network-interface inspection backed by
libuv. Inspection requires permission to listen on 0.0.0.0:0. No socket is
opened; this conservative gate protects local topology until Winglib2 provides a
dedicated network-inspection permission.)";

#if WL2_HAVE_QUICKJS

/** Owns the array allocated by uv_interface_addresses(). */
struct InterfaceList {
    uv_interface_address_t* values{};
    int count{};

    ~InterfaceList() {
        if (values) {
            uv_free_interface_addresses(values, count);
        }
    }
};

/** Normalized options shared by the two JavaScript inspection functions. */
struct QueryOptions {
    enum class Family { all, ipv4, ipv6 } family{Family::all};
    bool includeInternal{false};
    std::uint64_t maximumHosts{65536};
};

/** Construct a stable Winglib error while retaining optional libuv diagnostics. */
JSValue make_error(JSContext* ctx, const char* code, const char* operation,
                   const std::string& message, int nativeCode = 0) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "UvError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_uv"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    if (nativeCode) {
        JS_SetPropertyStr(ctx, error, "nativeCode", JS_NewInt32(ctx, nativeCode));
        JS_SetPropertyStr(ctx, error, "nativeName",
                          JS_NewString(ctx, uv_err_name(nativeCode)));
    } else {
        JS_SetPropertyStr(ctx, error, "nativeCode", JS_NULL);
        JS_SetPropertyStr(ctx, error, "nativeName", JS_NULL);
    }
    return error;
}

JSValue throw_error(JSContext* ctx, const char* code, const char* operation,
                    const std::string& message, int nativeCode = 0) {
    return JS_Throw(ctx, make_error(ctx, code, operation, message, nativeCode));
}

bool string_property(JSContext* ctx, JSValueConst object, const char* name,
                     std::string& output) {
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return true;
    }
    const char* text = JS_ToCString(ctx, value);
    if (!text) {
        JS_FreeValue(ctx, value);
        return false;
    }
    output = text;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return true;
}

/** Parse and validate JavaScript query options into native values. */
bool parse_options(JSContext* ctx, JSValueConst value, bool localNetworks,
                   QueryOptions& options) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        return true;
    }
    if (!JS_IsObject(value)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return false;
    }

    std::string family = "all";
    if (!string_property(ctx, value, "family", family)) {
        return false;
    }
    if (family == "all") {
        options.family = QueryOptions::Family::all;
    } else if (family == "IPv4") {
        options.family = QueryOptions::Family::ipv4;
    } else if (family == "IPv6") {
        options.family = QueryOptions::Family::ipv6;
    } else {
        JS_ThrowTypeError(ctx, "family must be 'all', 'IPv4', or 'IPv6'");
        return false;
    }

    JSValue internal = JS_GetPropertyStr(ctx, value, "includeInternal");
    if (!JS_IsUndefined(internal) && !JS_IsNull(internal)) {
        const int converted = JS_ToBool(ctx, internal);
        if (converted < 0) {
            JS_FreeValue(ctx, internal);
            return false;
        }
        options.includeInternal = converted != 0;
    }
    JS_FreeValue(ctx, internal);

    if (localNetworks) {
        JSValue maximum = JS_GetPropertyStr(ctx, value, "maximumHosts");
        if (!JS_IsUndefined(maximum) && !JS_IsNull(maximum)) {
            int64_t converted{};
            if (JS_ToInt64(ctx, &converted, maximum) != 0 || converted <= 0) {
                JS_FreeValue(ctx, maximum);
                JS_ThrowTypeError(ctx, "maximumHosts must be a positive integer");
                return false;
            }
            options.maximumHosts = static_cast<std::uint64_t>(converted);
        }
        JS_FreeValue(ctx, maximum);
    }
    return true;
}

bool family_selected(const QueryOptions& options, int family) {
    if (family == AF_INET) {
        return options.family != QueryOptions::Family::ipv6;
    }
    if (family == AF_INET6) {
        return options.family != QueryOptions::Family::ipv4;
    }
    return false;
}

std::string address_text(const sockaddr* address) {
    std::array<char, INET6_ADDRSTRLEN> text{};
    int result = UV_EAFNOSUPPORT;
    if (address->sa_family == AF_INET) {
        result = uv_ip4_name(reinterpret_cast<const sockaddr_in*>(address),
                             text.data(), text.size());
    } else if (address->sa_family == AF_INET6) {
        result = uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(address),
                             text.data(), text.size());
    }
    return result == 0 ? std::string(text.data()) : std::string{};
}

/** Return a contiguous netmask's prefix length, or -1 for an invalid mask. */
int prefix_length(const unsigned char* bytes, std::size_t size) {
    int prefix = 0;
    bool sawZero = false;
    for (std::size_t i = 0; i < size; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool set = (bytes[i] & (1U << bit)) != 0;
            if (sawZero && set) {
                return -1;
            }
            if (set) {
                ++prefix;
            } else {
                sawZero = true;
            }
        }
    }
    return prefix;
}

int netmask_prefix(const sockaddr* mask) {
    if (mask->sa_family == AF_INET) {
        const auto* value = reinterpret_cast<const sockaddr_in*>(mask);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value->sin_addr);
        return prefix_length(bytes, sizeof(value->sin_addr));
    }
    if (mask->sa_family == AF_INET6) {
        const auto* value = reinterpret_cast<const sockaddr_in6*>(mask);
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value->sin6_addr);
        return prefix_length(bytes, sizeof(value->sin6_addr));
    }
    return -1;
}

std::string mac_text(const uv_interface_address_t& item) {
    char text[18]{};
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                  static_cast<unsigned char>(item.phys_addr[0]),
                  static_cast<unsigned char>(item.phys_addr[1]),
                  static_cast<unsigned char>(item.phys_addr[2]),
                  static_cast<unsigned char>(item.phys_addr[3]),
                  static_cast<unsigned char>(item.phys_addr[4]),
                  static_cast<unsigned char>(item.phys_addr[5]));
    return text;
}

/** Apply the temporary listen-permission gate used for topology inspection. */
bool authorize_inspection(JSContext* ctx, const char* operation) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        JS_ThrowInternalError(ctx, "runtime unavailable");
        return false;
    }
    auto allowed = runtime->authorizeNetworkListen("0.0.0.0", 0);
    if (!allowed) {
        throw_error(ctx, "uv_permission_denied", operation,
                    allowed.error().message());
        return false;
    }
    return true;
}

/** Enumerate interface addresses and translate native failures to UvError. */
bool enumerate_interfaces(JSContext* ctx, const char* operation,
                          InterfaceList& list) {
    const int result = uv_interface_addresses(&list.values, &list.count);
    if (result != 0) {
        throw_error(ctx, "uv_interface_enumeration_failed", operation,
                    uv_strerror(result), result);
        return false;
    }
    return true;
}

/** QuickJS implementation of networkInterfaces(options). */
JSValue network_interfaces(JSContext* ctx, JSValueConst, int argc,
                           JSValueConst* argv) {
    QueryOptions options;
    if (!parse_options(ctx, argc ? argv[0] : JS_UNDEFINED, false, options)) {
        return JS_EXCEPTION;
    }
    if (!authorize_inspection(ctx, "networkInterfaces")) {
        return JS_EXCEPTION;
    }

    InterfaceList interfaces;
    if (!enumerate_interfaces(ctx, "networkInterfaces", interfaces)) {
        return JS_EXCEPTION;
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (int i = 0; i < interfaces.count; ++i) {
        const auto& item = interfaces.values[i];
        const sockaddr* address = reinterpret_cast<const sockaddr*>(&item.address);
        const sockaddr* netmask = reinterpret_cast<const sockaddr*>(&item.netmask);
        if (!family_selected(options, address->sa_family) ||
            (!options.includeInternal && item.is_internal)) {
            continue;
        }

        const std::string addressValue = address_text(address);
        const std::string netmaskValue = address_text(netmask);
        if (addressValue.empty() || netmaskValue.empty()) {
            continue;
        }

        JSValue object = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, object, "name", JS_NewString(ctx, item.name));
        JS_SetPropertyStr(ctx, object, "family",
                          JS_NewString(ctx, address->sa_family == AF_INET ? "IPv4" : "IPv6"));
        JS_SetPropertyStr(ctx, object, "address", JS_NewString(ctx, addressValue.c_str()));
        JS_SetPropertyStr(ctx, object, "netmask", JS_NewString(ctx, netmaskValue.c_str()));
        JS_SetPropertyStr(ctx, object, "internal", JS_NewBool(ctx, item.is_internal));
        JS_SetPropertyStr(ctx, object, "mac", JS_NewString(ctx, mac_text(item).c_str()));
        const int prefix = netmask_prefix(netmask);
        if (prefix >= 0) {
            JS_SetPropertyStr(ctx, object, "prefixLength", JS_NewInt32(ctx, prefix));
        } else {
            JS_SetPropertyStr(ctx, object, "prefixLength", JS_NULL);
        }
        JS_SetPropertyUint32(ctx, array, index++, object);
    }
    return array;
}

std::string ipv4_network(const sockaddr_in& address, const sockaddr_in& netmask,
                         int prefix) {
    sockaddr_in network{};
    network.sin_family = AF_INET;
    network.sin_addr.s_addr = address.sin_addr.s_addr & netmask.sin_addr.s_addr;
    const std::string text = address_text(reinterpret_cast<const sockaddr*>(&network));
    return text.empty() ? std::string{} : text + "/" + std::to_string(prefix);
}

/** QuickJS implementation of localNetworks(options). */
JSValue local_networks(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    QueryOptions options;
    if (!parse_options(ctx, argc ? argv[0] : JS_UNDEFINED, true, options)) {
        return JS_EXCEPTION;
    }
    if (options.family == QueryOptions::Family::ipv6) {
        return JS_ThrowTypeError(ctx, "localNetworks currently supports IPv4 only");
    }
    if (!authorize_inspection(ctx, "localNetworks")) {
        return JS_EXCEPTION;
    }

    InterfaceList interfaces;
    if (!enumerate_interfaces(ctx, "localNetworks", interfaces)) {
        return JS_EXCEPTION;
    }

    std::set<std::string> networks;
    for (int i = 0; i < interfaces.count; ++i) {
        const auto& item = interfaces.values[i];
        const auto* address = reinterpret_cast<const sockaddr*>(&item.address);
        const auto* netmask = reinterpret_cast<const sockaddr*>(&item.netmask);
        if (address->sa_family != AF_INET || netmask->sa_family != AF_INET ||
            (!options.includeInternal && item.is_internal)) {
            continue;
        }
        const int prefix = netmask_prefix(netmask);
        if (prefix < 0) {
            continue;
        }
        const std::uint64_t hostCount = std::uint64_t{1} << (32 - prefix);
        if (hostCount > options.maximumHosts) {
            continue;
        }
        const auto cidr = ipv4_network(
            *reinterpret_cast<const sockaddr_in*>(address),
            *reinterpret_cast<const sockaddr_in*>(netmask), prefix);
        if (!cidr.empty()) {
            networks.insert(cidr);
        }
    }

    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& network : networks) {
        JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, network.c_str()));
    }
    return array;
}

JSValue error_constructor(JSContext* ctx, JSValueConst, int argc,
                          JSValueConst* argv) {
    std::string message = "libuv error";
    if (argc > 0) {
        const char* text = JS_ToCString(ctx, argv[0]);
        if (!text) {
            return JS_EXCEPTION;
        }
        message = text;
        JS_FreeCString(ctx, text);
    }
    return make_error(ctx, "uv_internal", "unknown", message);
}

int init_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "networkInterfaces",
                       JS_NewCFunction(ctx, network_interfaces, "networkInterfaces", 1));
    JS_SetModuleExport(ctx, module, "localNetworks",
                       JS_NewCFunction(ctx, local_networks, "localNetworks", 1));
    JS_SetModuleExport(ctx, module, "UvError",
                       JS_NewCFunction2(ctx, error_constructor, "UvError", 1,
                                        JS_CFUNC_constructor, 0));
    JS_SetModuleExport(ctx, module, "version", JS_NewString(ctx, uv_version_string()));
    return 0;
}

#endif

}  // namespace

wl2::ModuleInfo wl2_uv_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:uv", wl2_uv_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:uv",
        .version = "0.1.0",
        .build = WL2_BUILD,
        .stableId = "8f6fdd67-04ed-4590-a3c1-4273a85ce672",
        .summary = "Cross-platform system and network utilities backed by libuv.",
        .api = UvApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_uv_quickjs_module_factory(void* context,
                                                 const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "networkInterfaces");
    JS_AddModuleExport(ctx, module, "localNetworks");
    JS_AddModuleExport(ctx, module, "UvError");
    JS_AddModuleExport(ctx, module, "version");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_UV_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:uv";
    out->version = "0.1.0";
    out->build = WL2_BUILD;
    out->stable_id = "8f6fdd67-04ed-4590-a3c1-4273a85ce672";
    out->summary = "Cross-platform system and network utilities backed by libuv.";
    out->api = UvApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
