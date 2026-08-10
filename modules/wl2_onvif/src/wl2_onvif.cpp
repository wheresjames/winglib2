/**
 * @file wl2_onvif.cpp
 * @brief QuickJS adapter for the native wlonvif client library.
 *
 * The adapter owns the JavaScript/native lifetime bridge, cancellation and
 * promise settlement, runtime network authorization, session child cleanup,
 * error conversion, and JavaScript value conversion for the ONVIF APIs.
 */

#include "wl2_onvif/wl2_onvif.h"

#include "wl2/runtime.h"

#include <wlonvif/wlonvif.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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
#ifndef WL2_ONVIF_STATIC_MODULE
#define WL2_ONVIF_STATIC_MODULE 0
#endif

namespace {

constexpr const char* OnvifApi = R"(Exports JavaScript module wl2:onvif.

Functions:
  connect(deviceServiceUrl, options?) -> Promise<DeviceSession>
  discover({networks, maximumHosts?, timeoutMs?, signal?}) -> Promise<DiscoverySession>
  localNetworks() -> Promise<string[]>

Exports:
  OnvifError
  version

DeviceSession provides Device queries, Media1, PTZ, PullPoint events, and close().
Authentication is automatic and TLS uses verified platform trust.)";

#if WL2_HAVE_QUICKJS

using namespace std::chrono_literals;

JSClassID session_class_id{};
JSClassID media_class_id{};
JSClassID ptz_class_id{};
JSClassID events_class_id{};
JSClassID subscription_class_id{};
JSClassID stream_class_id{};
JSClassID discovery_class_id{};

const char* category_name(wlonvif::ErrorCategory category) {
    using C = wlonvif::ErrorCategory;
    switch (category) {
        case C::cancelled: return "cancellation";
        case C::timeout: return "timeout";
        case C::network: return "network";
        case C::tls: return "tls";
        case C::authentication: return "authentication";
        case C::authorization: return "authorization";
        case C::device_busy: return "device_busy";
        case C::rate_limited: return "rate_limiting";
        case C::malformed_http: return "malformed_http";
        case C::malformed_xml: return "malformed_xml";
        case C::soap_fault: return "soap_fault";
        case C::unsupported: return "unsupported";
        case C::invalid_argument: return "invalid_argument";
        case C::endpoint_rejected: return "permission";
        case C::host_permission_denied: return "permission";
        case C::closed_handle: return "closed_handle";
        case C::protocol_violation: return "protocol_violation";
        case C::closed: return "closed_handle";
        case C::queue_overflow: return "queue_overflow";
        case C::internal: return "internal";
    }
    return "internal";
}

const char* error_code(wlonvif::ErrorCategory category) {
    using C = wlonvif::ErrorCategory;
    switch (category) {
        case C::cancelled: return "onvif_cancelled";
        case C::timeout: return "onvif_timeout";
        case C::host_permission_denied: return "onvif_permission_denied";
        case C::endpoint_rejected: return "onvif_endpoint_rejected";
        case C::authentication: return "onvif_authentication_failed";
        case C::authorization: return "onvif_authorization_failed";
        case C::tls: return "onvif_tls";
        case C::network: return "onvif_network";
        case C::device_busy: return "onvif_device_busy";
        case C::rate_limited: return "onvif_rate_limited";
        case C::malformed_http: return "onvif_malformed_http";
        case C::malformed_xml: return "onvif_malformed_xml";
        case C::soap_fault: return "onvif_soap_fault";
        case C::unsupported: return "onvif_unsupported";
        case C::invalid_argument: return "onvif_invalid_argument";
        case C::closed_handle:
        case C::closed: return "onvif_closed_handle";
        case C::protocol_violation: return "onvif_protocol_violation";
        case C::queue_overflow: return "onvif_queue_overflow";
        case C::internal: return "onvif_internal";
    }
    return "onvif_internal";
}

const char* operation_name(wlonvif::OperationId operation) {
    using O = wlonvif::OperationId;
    switch (operation) {
        case O::get_device_information: return "GetDeviceInformation";
        case O::get_system_date_and_time: return "GetSystemDateAndTime";
        case O::get_scopes: return "GetScopes";
        case O::get_services: return "GetServices";
        case O::get_capabilities: return "GetCapabilities";
        case O::get_profiles: return "GetProfiles";
        case O::get_stream_uri: return "GetStreamUri";
        case O::get_snapshot_uri: return "GetSnapshotUri";
        case O::ptz_status: return "GetStatus";
        case O::get_ptz_configuration_options: return "GetConfigurationOptions";
        case O::continuous_move: return "ContinuousMove";
        case O::relative_move: return "RelativeMove";
        case O::absolute_move: return "AbsoluteMove";
        case O::ptz_stop: return "Stop";
        case O::get_presets: return "GetPresets";
        case O::goto_preset: return "GotoPreset";
        case O::discovery_probe: return "Probe";
        case O::event_subscribe: return "CreatePullPointSubscription";
        case O::event_pull: return "PullMessages";
        case O::event_renew: return "Renew";
        case O::event_close: return "Unsubscribe";
    }
    return "Unknown";
}

JSValue make_error(JSContext* ctx, const wlonvif::Error& native) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "OnvifError"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, error_code(native.category)));
    JS_SetPropertyStr(ctx, error, "category", JS_NewString(ctx, category_name(native.category)));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation_name(native.operation)));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, native.message.empty() ? error_code(native.category) : native.message.c_str()));
    JS_SetPropertyStr(ctx, error, "retryable", JS_NewBool(ctx, native.retryable));
    JS_SetPropertyStr(ctx, error, "requestMayHaveBeenApplied", JS_NewBool(ctx, native.request_may_have_been_applied));
    if (native.http) JS_SetPropertyStr(ctx, error, "httpStatus", JS_NewInt64(ctx, native.http->status));
    else JS_SetPropertyStr(ctx, error, "httpStatus", JS_NULL);
    if (native.soap_fault) {
        JSValue fault = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, fault, "code", JS_NewString(ctx, native.soap_fault->code.c_str()));
        JS_SetPropertyStr(ctx, fault, "subcode", JS_NewString(ctx, native.soap_fault->subcode.c_str()));
        JS_SetPropertyStr(ctx, fault, "reason", JS_NewString(ctx, native.soap_fault->reason.c_str()));
        JS_SetPropertyStr(ctx, fault, "detail", JS_NewString(ctx, native.soap_fault->detail.c_str()));
        JS_SetPropertyStr(ctx, error, "soapFault", fault);
    } else JS_SetPropertyStr(ctx, error, "soapFault", JS_NULL);
    if (native.safety_action) {
        JSValue safety = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, safety, "operation", JS_NewString(ctx, operation_name(native.safety_action->operation)));
        JS_SetPropertyStr(ctx, safety, "attempted", JS_NewBool(ctx, native.safety_action->attempted));
        JS_SetPropertyStr(ctx, safety, "succeeded", JS_NewBool(ctx, native.safety_action->succeeded));
        if (native.safety_action->failure_category)
            JS_SetPropertyStr(ctx, safety, "errorCode", JS_NewString(ctx, error_code(*native.safety_action->failure_category)));
        else JS_SetPropertyStr(ctx, safety, "errorCode", JS_NULL);
        JS_SetPropertyStr(ctx, error, "safetyAction", safety);
    } else JS_SetPropertyStr(ctx, error, "safetyAction", JS_NULL);
    return error;
}

JSValue local_error(JSContext* ctx, wlonvif::ErrorCategory category, const char* operation, const std::string& message) {
    wlonvif::Error error{category, wlonvif::OperationId::get_device_information, message};
    JSValue value = make_error(ctx, error);
    JS_SetPropertyStr(ctx, value, "operation", JS_NewString(ctx, operation));
    return value;
}

JSValue local_named_error(JSContext* ctx, wlonvif::ErrorCategory category, const char* code,
                          const char* operation, const std::string& message) {
    JSValue value = local_error(ctx, category, operation, message);
    JS_SetPropertyStr(ctx, value, "code", JS_NewString(ctx, code));
    return value;
}

/** Tracks an AbortSignal listener and its native cancellation source. */
struct AbortRegistration {
    JSContext* ctx{};
    JSValue signal{JS_UNDEFINED};
    JSValue listener{JS_UNDEFINED};
    std::shared_ptr<wlonvif::CancellationSource> source;
};

/** Owns a pending QuickJS promise and Winglib runtime operation lease. */
struct Promise {
    JSContext* ctx{};
    wl2::Runtime* host{};
    JSValue resolve{JS_UNDEFINED};
    JSValue reject{JS_UNDEFINED};
    std::atomic_bool settled{false};
    std::unique_ptr<AbortRegistration> abort;
};

JSValue abort_listener(JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* data) {
    int64_t address{};
    if (JS_ToInt64(ctx, &address, data[0]) == 0 && address) {
        auto* registration = reinterpret_cast<AbortRegistration*>(static_cast<intptr_t>(address));
        if (registration->source) registration->source->request_stop();
    }
    return JS_UNDEFINED;
}

/** Attach a JavaScript AbortSignal to a native wlonvif cancellation source. */
bool install_abort_listener(JSContext* ctx, JSValueConst options,
                            const std::shared_ptr<wlonvif::CancellationSource>& source,
                            const std::shared_ptr<Promise>& promise) {
    if (!source || !JS_IsObject(options)) return true;
    JSValue signal = JS_GetPropertyStr(ctx, options, "signal");
    if (!JS_IsObject(signal)) { JS_FreeValue(ctx, signal); return true; }
    auto registration = std::make_unique<AbortRegistration>();
    registration->ctx = ctx;
    registration->signal = signal;
    registration->source = source;
    JSValue pointer = JS_NewInt64(ctx, static_cast<int64_t>(reinterpret_cast<intptr_t>(registration.get())));
    registration->listener = JS_NewCFunctionData(ctx, abort_listener, 0, 0, 1, &pointer);
    JS_FreeValue(ctx, pointer);
    JSValue event = JS_NewString(ctx, "abort");
    JSValue args[] = {event, registration->listener};
    JSAtom atom = JS_NewAtom(ctx, "addEventListener");
    JSValue added = JS_Invoke(ctx, signal, atom, 2, args);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, event);
    if (JS_IsException(added)) {
        JS_FreeValue(ctx, added);
        JS_FreeValue(ctx, registration->listener);
        JS_FreeValue(ctx, registration->signal);
        return false;
    }
    JS_FreeValue(ctx, added);
    promise->abort = std::move(registration);
    return true;
}

void remove_abort_listener(const std::shared_ptr<Promise>& promise) {
    if (!promise->abort) return;
    JSContext* ctx = promise->abort->ctx;
    JSValue event = JS_NewString(ctx, "abort");
    JSValue args[] = {event, promise->abort->listener};
    JSAtom atom = JS_NewAtom(ctx, "removeEventListener");
    JSValue removed = JS_Invoke(ctx, promise->abort->signal, atom, 2, args);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, removed);
    JS_FreeValue(ctx, event);
    JS_FreeValue(ctx, promise->abort->listener);
    JS_FreeValue(ctx, promise->abort->signal);
    promise->abort.reset();
}

JSValue new_promise(JSContext* ctx, wl2::Runtime* host, std::shared_ptr<Promise>& promise) {
    JSValue funcs[2];
    JSValue result = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(result)) return result;
    promise = std::make_shared<Promise>();
    promise->ctx = ctx;
    promise->host = host;
    promise->resolve = funcs[0];
    promise->reject = funcs[1];
    return result;
}

/** Settle a promise exactly once and release its QuickJS-owned values. */
void settle(const std::shared_ptr<Promise>& promise, bool success, JSValue value) {
    if (promise->settled.exchange(true)) { JS_FreeValue(promise->ctx, value); return; }
    remove_abort_listener(promise);
    JSValue fn = success ? promise->resolve : promise->reject;
    JSValue called = JS_Call(promise->ctx, fn, JS_UNDEFINED, 1, &value);
    JS_FreeValue(promise->ctx, called);
    JS_FreeValue(promise->ctx, value);
    JS_FreeValue(promise->ctx, promise->resolve);
    JS_FreeValue(promise->ctx, promise->reject);
}

JSValue rejected(JSContext* ctx, wl2::Runtime* host, JSValue error) {
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, host, promise);
    if (JS_IsException(result)) { JS_FreeValue(ctx, error); return result; }
    settle(promise, false, error);
    return result;
}

/** Start a typed native task and marshal its result onto the JavaScript thread. */
template <class T, class Convert>
JSValue start_task(JSContext* ctx, wl2::Runtime* host, wlonvif::Task<wlonvif::Result<T>> task, Convert convert,
                   JSValueConst signal_options = JS_UNDEFINED,
                   std::shared_ptr<wlonvif::CancellationSource> cancellation = {}) {
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, host, promise);
    if (JS_IsException(result)) return result;
    if (!install_abort_listener(ctx, signal_options, cancellation, promise)) {
        settle(promise, false, local_error(ctx, wlonvif::ErrorCategory::invalid_argument,
                                          "AbortSignal", "signal does not provide addEventListener"));
        return result;
    }
    host->async().beginOperation();
    try {
        std::move(task).start([promise, convert = std::move(convert)](wlonvif::Result<T> native) mutable {
            promise->host->async().post([promise, native = std::move(native), convert = std::move(convert)]() mutable {
                if (native) settle(promise, true, convert(promise->ctx, native.value()));
                else settle(promise, false, make_error(promise->ctx, native.error()));
                promise->host->async().endOperation();
            });
        });
    } catch (const std::exception& exception) {
        host->async().post([promise, message = std::string(exception.what())] {
            settle(promise, false, local_error(promise->ctx, wlonvif::ErrorCategory::internal, "start", message));
            promise->host->async().endOperation();
        });
    }
    return result;
}

/** Start a void native task and marshal completion onto the JavaScript thread. */
template <class Convert>
JSValue start_void_task(JSContext* ctx, wl2::Runtime* host, wlonvif::Task<wlonvif::Result<void>> task, Convert convert,
                        JSValueConst signal_options = JS_UNDEFINED,
                        std::shared_ptr<wlonvif::CancellationSource> cancellation = {}) {
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, host, promise);
    if (JS_IsException(result)) return result;
    if (!install_abort_listener(ctx, signal_options, cancellation, promise)) {
        settle(promise, false, local_error(ctx, wlonvif::ErrorCategory::invalid_argument,
                                          "AbortSignal", "signal does not provide addEventListener"));
        return result;
    }
    host->async().beginOperation();
    try {
        std::move(task).start([promise, convert = std::move(convert)](wlonvif::Result<void> native) mutable {
            promise->host->async().post([promise, native = std::move(native), convert = std::move(convert)]() mutable {
                if (native) settle(promise, true, convert(promise->ctx));
                else settle(promise, false, make_error(promise->ctx, native.error()));
                promise->host->async().endOperation();
            });
        });
    } catch (const std::exception& exception) {
        host->async().post([promise, message = std::string(exception.what())] {
            settle(promise, false, local_error(promise->ctx, wlonvif::ErrorCategory::internal, "start", message));
            promise->host->async().endOperation();
        });
    }
    return result;
}

bool string_value(JSContext* ctx, JSValueConst value, std::string& output) {
    const char* text = JS_ToCString(ctx, value);
    if (!text) return false;
    output = text;
    JS_FreeCString(ctx, text);
    return true;
}

JSValue get_prop(JSContext* ctx, JSValueConst object, const char* name) {
    return JS_IsObject(object) ? JS_GetPropertyStr(ctx, object, name) : JS_UNDEFINED;
}

bool string_prop(JSContext* ctx, JSValueConst object, const char* name, std::string& output) {
    JSValue value = get_prop(ctx, object, name);
    bool present = !JS_IsUndefined(value) && !JS_IsNull(value);
    bool ok = !present || string_value(ctx, value, output);
    JS_FreeValue(ctx, value);
    return present && ok;
}

int timeout_ms(JSContext* ctx, JSValueConst options, int fallback = 10000) {
    JSValue value = get_prop(ctx, options, "timeoutMs");
    int32_t timeout = fallback;
    if (!JS_IsUndefined(value) && JS_ToInt32(ctx, &timeout, value) != 0) timeout = -1;
    JS_FreeValue(ctx, value);
    return timeout;
}

bool already_aborted(JSContext* ctx, JSValueConst options) {
    JSValue signal = get_prop(ctx, options, "signal");
    JSValue aborted = get_prop(ctx, signal, "aborted");
    bool result = !JS_IsUndefined(aborted) && JS_ToBool(ctx, aborted) > 0;
    JS_FreeValue(ctx, aborted);
    JS_FreeValue(ctx, signal);
    return result;
}

wlonvif::CallOptions call_options(int timeout, const wlonvif::CancellationSource& source) {
    return {std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout), source.token()};
}

/** Validated timeout, signal, and cancellation state for one API operation. */
struct OperationInput {
    JSValueConst options{JS_UNDEFINED};
    int timeout{10000};
    std::shared_ptr<wlonvif::CancellationSource> cancellation{
        std::make_shared<wlonvif::CancellationSource>()};
};

std::optional<OperationInput> operation_input(JSContext* ctx, int argc, JSValueConst* argv,
                                              int options_index) {
    OperationInput input;
    if (argc > options_index) input.options = argv[options_index];
    input.timeout = timeout_ms(ctx, input.options);
    if (input.timeout <= 0 || already_aborted(ctx, input.options)) return std::nullopt;
    return input;
}

/**
 * @brief Bridges resolved ONVIF connection candidates to Winglib policy.
 *
 * The original hostname and every resolved or proposed address must pass the
 * runtime's outbound-network authorization before wlonvif may connect.
 */
class HostAuthorizer final : public wlonvif::CandidateAuthorizer {
public:
    explicit HostAuthorizer(wl2::Runtime* host) : host_(host) {}
    wlonvif::Task<wlonvif::Result<void>> authorize(wlonvif::CandidateAuthorizationRequest request, wlonvif::CallOptions options) override {
        return wlonvif::Task<wlonvif::Result<void>>([this, request = std::move(request), options](auto done) mutable {
            if (options.cancellation.stop_requested()) { done(wlonvif::Error{wlonvif::ErrorCategory::cancelled}); return; }
            if (std::chrono::steady_clock::now() >= options.deadline) { done(wlonvif::Error{wlonvif::ErrorCategory::timeout}); return; }
            std::vector<std::string> candidates{request.hostname};
            candidates.insert(candidates.end(), request.resolved_addresses.begin(), request.resolved_addresses.end());
            if (!request.proposed_address.empty()) candidates.push_back(request.proposed_address);
            for (const auto& candidate : candidates) {
                auto allowed = host_->authorizeNetworkConnect(candidate, request.port);
                if (!allowed) {
                    wlonvif::Error error{wlonvif::ErrorCategory::host_permission_denied, request.operation.value_or(wlonvif::OperationId::get_device_information), "host denied ONVIF network candidate"};
                    error.host_permission = wlonvif::HostPermissionDetail{request.hostname, request.port, candidate, request.resolution_attempt_id};
                    done(std::move(error)); return;
                }
            }
            done(wlonvif::Result<void>{});
        });
    }
private:
    wl2::Runtime* host_{};
};

/**
 * @brief Maps native discovery actions to Winglib connect/listen permissions.
 *
 * Implicit interface enumeration and multicast membership are denied. Direct
 * scans are accepted only for explicitly supplied CIDRs, with their resulting
 * network operations authorized independently.
 */
class HostDiscoveryAuthorizer final : public wlonvif::DiscoveryAuthorizer {
public:
    explicit HostDiscoveryAuthorizer(wl2::Runtime* host) : host_(host) {}
    wlonvif::Task<wlonvif::Result<void>> authorize(
        wlonvif::DiscoveryAuthorizationRequest request, wlonvif::CallOptions options) override {
        return wlonvif::Task<wlonvif::Result<void>>(
            [this, request = std::move(request), options](auto done) mutable {
                if (options.cancellation.stop_requested()) {
                    done(wlonvif::Error{wlonvif::ErrorCategory::cancelled}); return;
                }
                if (std::chrono::steady_clock::now() >= options.deadline) {
                    done(wlonvif::Error{wlonvif::ErrorCategory::timeout}); return;
                }
                using A = wlonvif::DiscoveryNetworkAction;
                if (request.action == A::enumerate_interfaces || request.action == A::join_multicast) {
                    done(wlonvif::Error{wlonvif::ErrorCategory::host_permission_denied,
                        wlonvif::OperationId::discovery_probe,
                        "implicit interface enumeration and multicast membership are unavailable"});
                    return;
                }
                if (request.action == A::scan_network) {
                    // Expanding an explicitly supplied CIDR performs no I/O. Every resulting
                    // TCP connection is authorized separately below before it is attempted.
                    done(wlonvif::Result<void>{}); return;
                }
                const std::string host = !request.destination.empty()
                    ? request.destination : request.interface_address;
                const uint16_t port = request.port;
                auto allowed = request.action == A::bind_udp
                    ? host_->authorizeNetworkListen(host, port)
                    : host_->authorizeNetworkConnect(host, port);
                if (!allowed) {
                    done(wlonvif::Error{wlonvif::ErrorCategory::host_permission_denied,
                        wlonvif::OperationId::discovery_probe, allowed.error().message()});
                    return;
                }
                done(wlonvif::Result<void>{});
            });
    }
private:
    wl2::Runtime* host_{};
};

/**
 * Owns the wlonvif runtime and authorization adapters for one Winglib runtime.
 */
struct NativeService {
    wl2::Runtime* host{};
    std::shared_ptr<wlonvif::Runtime> runtime;
    std::shared_ptr<HostAuthorizer> authorizer;
    std::shared_ptr<HostDiscoveryAuthorizer> discovery_authorizer;
    std::atomic_bool closing{false};
    std::mutex cleanup_mutex;
    std::vector<std::function<void()>> cleanup_hooks;
    void shutdown() {
        if (closing.exchange(true)) return;
        std::vector<std::function<void()>> hooks;
        { std::lock_guard lock(cleanup_mutex); hooks.swap(cleanup_hooks); }
        for (auto& hook : hooks) hook();
        if (runtime) runtime->shutdown();
        runtime.reset();
        authorizer.reset();
        discovery_authorizer.reset();
    }
};

std::mutex services_mutex;
std::unordered_map<wl2::Runtime*, std::weak_ptr<NativeService>> services;

/** Return the lazily created native service associated with @p host. */
std::shared_ptr<NativeService> service_for(wl2::Runtime* host) {
    std::lock_guard lock(services_mutex);
    if (auto existing = services[host].lock()) return existing;
    auto created = wlonvif::Runtime::create();
    if (!created) return {};
    auto service = std::make_shared<NativeService>();
    service->host = host;
    service->runtime = created.value();
    service->authorizer = std::make_shared<HostAuthorizer>(host);
    service->discovery_authorizer = std::make_shared<HostDiscoveryAuthorizer>(host);
    services[host] = service;
    host->async().registerShutdownHook([weak = std::weak_ptr<NativeService>(service), host] {
        if (auto locked = weak.lock()) locked->shutdown();
        std::lock_guard guard(services_mutex);
        services.erase(host);
    });
    return service;
}

struct SubscriptionHandle;
struct StreamHandle;
/** Shared native state retained by a DeviceSession and all of its child APIs. */
struct SessionHandle {
    std::shared_ptr<NativeService> service;
    std::shared_ptr<wlonvif::DeviceSession> session;
    std::shared_ptr<wlonvif::MediaClient> media;
    std::shared_ptr<wlonvif::PtzClient> ptz;
    std::shared_ptr<wlonvif::EventClient> events;
    std::string url;
    std::atomic_bool closed{false};
    std::mutex children_mutex;
    std::vector<std::weak_ptr<SubscriptionHandle>> subscriptions;
    std::vector<std::weak_ptr<StreamHandle>> streams;
};
using HandleRef = std::shared_ptr<SessionHandle>;

/** Native PullPoint subscription plus its owning device session. */
struct SubscriptionHandle {
    HandleRef owner;
    std::shared_ptr<wlonvif::PullPointSubscription> native;
    std::atomic_bool closed{false};
};
using SubscriptionRef = std::shared_ptr<SubscriptionHandle>;

/** Native managed event stream plus iteration and close state. */
struct StreamHandle {
    HandleRef owner;
    std::shared_ptr<wlonvif::ManagedEventStream> native;
    std::atomic_bool closed{false};
};
using StreamRef = std::shared_ptr<StreamHandle>;

/** Buffered discovery result set exposed through an async iterator. */
struct DiscoveryHandle {
    std::shared_ptr<NativeService> service;
    std::vector<wlonvif::DeviceCandidate> candidates;
    std::size_t cursor{};
    std::atomic_bool closed{false};
};
using DiscoveryRef = std::shared_ptr<DiscoveryHandle>;

HandleRef handle_from(JSContext* ctx, JSValueConst value, JSClassID id) {
    auto* box = static_cast<HandleRef*>(JS_GetOpaque2(ctx, value, id));
    return box ? *box : HandleRef{};
}

void handle_finalizer(JSRuntime*, JSValue value, JSClassID id) {
    delete static_cast<HandleRef*>(JS_GetOpaque(value, id));
}
void session_finalizer(JSRuntime* rt, JSValue value) { handle_finalizer(rt, value, session_class_id); }
void media_finalizer(JSRuntime* rt, JSValue value) { handle_finalizer(rt, value, media_class_id); }
void ptz_finalizer(JSRuntime* rt, JSValue value) { handle_finalizer(rt, value, ptz_class_id); }
void events_finalizer(JSRuntime* rt, JSValue value) { handle_finalizer(rt, value, events_class_id); }

/** Schedule bounded subscription cleanup away from a QuickJS finalizer. */
void schedule_subscription_cleanup(SubscriptionRef ref) {
    if (!ref || ref->closed.exchange(true) || !ref->native) return;
    auto runtime = ref->owner->service->runtime;
    if (!runtime || !runtime->post([ref = std::move(ref)]() mutable {
            auto task = ref->native->close(5000ms);
            std::move(task).start([ref = std::move(ref)](wlonvif::Result<wlonvif::CloseResult>) mutable {
                ref->native.reset();
            });
        })) ref->native.reset();
}

/** Schedule managed-stream shutdown away from a QuickJS finalizer. */
void schedule_stream_cleanup(StreamRef ref) {
    if (!ref || ref->closed.exchange(true) || !ref->native) return;
    auto runtime = ref->owner->service->runtime;
    if (!runtime || !runtime->post([ref = std::move(ref)]() mutable {
            auto source = std::make_shared<wlonvif::CancellationSource>();
            auto task = ref->native->close(call_options(5000, *source));
            std::move(task).start([ref = std::move(ref), source](wlonvif::Result<void>) mutable {
                ref->native.reset();
            });
        })) std::thread([ref = std::move(ref)]() mutable { ref->native.reset(); }).detach();
}

void subscription_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<SubscriptionRef*>(JS_GetOpaque(value, subscription_class_id));
    if (box) { schedule_subscription_cleanup(*box); delete box; }
}
void stream_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<StreamRef*>(JS_GetOpaque(value, stream_class_id));
    if (box) { schedule_stream_cleanup(*box); delete box; }
}
void discovery_finalizer(JSRuntime*, JSValue value) {
    delete static_cast<DiscoveryRef*>(JS_GetOpaque(value, discovery_class_id));
}

JSValue make_vector(JSContext* ctx, const wlonvif::PtzVector& vector) {
    JSValue object = JS_NewObject(ctx);
    if (vector.pan) JS_SetPropertyStr(ctx, object, "pan", JS_NewFloat64(ctx, *vector.pan));
    if (vector.tilt) JS_SetPropertyStr(ctx, object, "tilt", JS_NewFloat64(ctx, *vector.tilt));
    if (vector.zoom) JS_SetPropertyStr(ctx, object, "zoom", JS_NewFloat64(ctx, *vector.zoom));
    return object;
}

bool parse_vector(JSContext* ctx, JSValueConst value, wlonvif::PtzVector& vector) {
    if (!JS_IsObject(value)) return false;
    const auto read = [&](const char* name, std::optional<double>& target) {
        JSValue property = JS_GetPropertyStr(ctx, value, name);
        if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
            double number{};
            if (JS_ToFloat64(ctx, &number, property) != 0 || !std::isfinite(number) || number < -1 || number > 1) {
                JS_FreeValue(ctx, property); return false;
            }
            target = number;
        }
        JS_FreeValue(ctx, property); return true;
    };
    return read("pan", vector.pan) && read("tilt", vector.tilt) && read("zoom", vector.zoom) &&
           (vector.pan || vector.tilt || vector.zoom);
}

JSValue js_string_array(JSContext* ctx, const std::vector<std::string>& values) {
    JSValue array = JS_NewArray(ctx);
    for (uint32_t i = 0; i < values.size(); ++i) JS_SetPropertyUint32(ctx, array, i, JS_NewString(ctx, values[i].c_str()));
    return array;
}

JSValue media_uri_value(JSContext* ctx, const wlonvif::MediaUri& uri) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "uri", JS_NewString(ctx, uri.uri.c_str()));
    JS_SetPropertyStr(ctx, object, "validForMs", JS_NewInt64(ctx, std::chrono::duration_cast<std::chrono::milliseconds>(uri.valid_for).count()));
    JS_SetPropertyStr(ctx, object, "invalidAfterConnect", JS_NewBool(ctx, uri.invalid_after_connect));
    return object;
}

std::string iso_utc(std::chrono::system_clock::time_point value) {
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(value - seconds).count();
    const std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    char date[32]{};
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    char result[48]{};
    std::snprintf(result, sizeof(result), "%s.%03lldZ", date,
                  static_cast<long long>(millis < 0 ? millis + 1000 : millis));
    return result;
}

JSValue session_object(JSContext* ctx, HandleRef handle);

JSValue session_info(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto handle = handle_from(ctx, self, session_class_id);
    if (!handle) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    auto* host = handle->service->host;
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, host, local_error(ctx, wlonvif::ErrorCategory::cancelled, "GetDeviceInformation", "invalid timeout or aborted signal"));
    auto task = handle->session->get_device_information(call_options(input->timeout, *input->cancellation));
    return start_task(ctx, host, std::move(task), [](JSContext* c, wlonvif::DeviceInformation& info) {
        JSValue o = JS_NewObject(c);
        JS_SetPropertyStr(c, o, "manufacturer", JS_NewString(c, info.manufacturer.c_str()));
        JS_SetPropertyStr(c, o, "model", JS_NewString(c, info.model.c_str()));
        JS_SetPropertyStr(c, o, "firmwareVersion", JS_NewString(c, info.firmware_version.c_str()));
        JS_SetPropertyStr(c, o, "serialNumber", JS_NewString(c, info.serial_number.c_str()));
        JS_SetPropertyStr(c, o, "hardwareId", JS_NewString(c, info.hardware_id.c_str()));
        return o;
    }, input->options, input->cancellation);
}

JSValue session_time(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, session_class_id); if (!h) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    auto input=operation_input(ctx,argc,argv,0);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetSystemDateAndTime","invalid timeout or aborted signal"));
    auto task=h->session->get_system_date_and_time(call_options(input->timeout,*input->cancellation));
    return start_task(ctx, h->service->host, std::move(task), [](JSContext* c, wlonvif::SystemDateAndTime& time) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.utc.time_since_epoch()).count();
        JSValue o = JS_NewObject(c); JS_SetPropertyStr(c, o, "utc", JS_NewString(c, iso_utc(time.utc).c_str())); JS_SetPropertyStr(c, o, "epochMilliseconds", JS_NewInt64(c, ms)); return o;
    },input->options,input->cancellation);
}

JSValue session_scopes(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, session_class_id); if (!h) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    auto input=operation_input(ctx,argc,argv,0);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetScopes","invalid timeout or aborted signal"));auto task=h->session->get_scopes(call_options(input->timeout,*input->cancellation));return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,std::vector<std::string>&v){return js_string_array(c,v);},input->options,input->cancellation);
}

JSValue session_services(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, session_class_id); if (!h) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    auto input=operation_input(ctx,argc,argv,0);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetServices","invalid timeout or aborted signal"));auto task=h->session->get_services(call_options(input->timeout,*input->cancellation));
    return start_task(ctx, h->service->host, std::move(task), [](JSContext* c, std::vector<wlonvif::Service>& values) {
        JSValue a = JS_NewArray(c); for (uint32_t i=0;i<values.size();++i) { JSValue o=JS_NewObject(c); JS_SetPropertyStr(c,o,"namespaceUri",JS_NewString(c,values[i].namespace_uri.c_str())); JS_SetPropertyStr(c,o,"xaddr",JS_NewString(c,values[i].xaddr.c_str())); JS_SetPropertyUint32(c,a,i,o); } return a;
    },input->options,input->cancellation);
}

JSValue session_capabilities(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, session_class_id); if (!h) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    auto input=operation_input(ctx,argc,argv,0);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetCapabilities","invalid timeout or aborted signal"));auto task=h->session->get_capabilities(call_options(input->timeout,*input->cancellation));
    return start_task(ctx, h->service->host, std::move(task), [](JSContext* c, wlonvif::Capabilities& v) {
        JSValue o=JS_NewObject(c); if(v.media_xaddr)JS_SetPropertyStr(c,o,"mediaXaddr",JS_NewString(c,v.media_xaddr->c_str())); if(v.ptz_xaddr)JS_SetPropertyStr(c,o,"ptzXaddr",JS_NewString(c,v.ptz_xaddr->c_str())); if(v.events_xaddr)JS_SetPropertyStr(c,o,"eventsXaddr",JS_NewString(c,v.events_xaddr->c_str())); return o;
    },input->options,input->cancellation);
}

JSValue session_close(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto h = handle_from(ctx, self, session_class_id); if (!h) return JS_ThrowTypeError(ctx, "invalid DeviceSession");
    h->closed = true;
    JS_SetPropertyStr(ctx, self, "closed", JS_NewBool(ctx, true));
    std::vector<SubscriptionRef> subscriptions;
    std::vector<StreamRef> streams;
    {
        std::lock_guard lock(h->children_mutex);
        for (auto& weak : h->subscriptions) if (auto child = weak.lock()) subscriptions.push_back(std::move(child));
        for (auto& weak : h->streams) if (auto child = weak.lock()) streams.push_back(std::move(child));
        h->subscriptions.clear(); h->streams.clear();
    }
    for (auto& child : subscriptions) schedule_subscription_cleanup(std::move(child));
    for (auto& child : streams) schedule_stream_cleanup(std::move(child));
    return start_task(ctx, h->service->host, h->session->close(5000ms), [](JSContext* c, wlonvif::CloseResult& result) { JSValue o=JS_NewObject(c); JS_SetPropertyStr(c,o,"cleanupComplete",JS_NewBool(c,result.cleanup_complete)); JS_SetPropertyStr(c,o,"safetyComplete",JS_NewBool(c,result.safety_complete)); return o; });
}

JSValue media_profiles(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h=handle_from(ctx,self,media_class_id); if(!h||!h->media)return JS_ThrowTypeError(ctx,"invalid MediaClient");auto input=operation_input(ctx,argc,argv,0);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetProfiles","invalid timeout or aborted signal"));auto task=h->media->get_profiles(call_options(input->timeout,*input->cancellation));
    return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,std::vector<wlonvif::MediaProfile>&v){JSValue a=JS_NewArray(c);for(uint32_t i=0;i<v.size();++i){JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"token",JS_NewString(c,v[i].token.c_str()));JS_SetPropertyStr(c,o,"name",JS_NewString(c,v[i].name.c_str()));JS_SetPropertyUint32(c,a,i,o);}return a;},input->options,input->cancellation);
}

JSValue media_uri_call(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, bool snapshot) {
    auto h=handle_from(ctx,self,media_class_id); if(!h||!h->media)return JS_ThrowTypeError(ctx,"invalid MediaClient"); std::string token;if(argc<1||!string_value(ctx,argv[0],token)||token.empty())return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,snapshot?"GetSnapshotUri":"GetStreamUri","profile token is required"));auto input=operation_input(ctx,argc,argv,1);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,snapshot?"GetSnapshotUri":"GetStreamUri","invalid timeout or aborted signal"));
    auto native_options=call_options(input->timeout,*input->cancellation);auto task=snapshot?h->media->get_snapshot_uri(token,native_options):h->media->get_stream_uri(token,native_options);
    return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,wlonvif::MediaUri&v){return media_uri_value(c,v);},input->options,input->cancellation);
}
JSValue media_stream(JSContext*c,JSValueConst s,int n,JSValueConst*a){return media_uri_call(c,s,n,a,false);} JSValue media_snapshot(JSContext*c,JSValueConst s,int n,JSValueConst*a){return media_uri_call(c,s,n,a,true);}

JSValue ptz_spaces_value(JSContext* c,wlonvif::PtzSpaces&v){
    JSValue o=JS_NewObject(c);
    auto range=[c](const wlonvif::NativeRange&r){JSValue x=JS_NewObject(c);JS_SetPropertyStr(c,x,"minimum",JS_NewFloat64(c,r.minimum));JS_SetPropertyStr(c,x,"maximum",JS_NewFloat64(c,r.maximum));return x;};
    JS_SetPropertyStr(c,o,"pan",range(v.pan));JS_SetPropertyStr(c,o,"tilt",range(v.tilt));JS_SetPropertyStr(c,o,"zoom",range(v.zoom));
    // Preserve the operation-specific spaces. A fallback range alone cannot say
    // whether a camera actually advertises an axis for a particular move mode.
    JS_SetPropertyStr(c,o,"absolutePanTilt",JS_NewBool(c,!v.absolute_pan_tilt.empty()));
    JS_SetPropertyStr(c,o,"absoluteZoom",JS_NewBool(c,!v.absolute_zoom.empty()));
    JS_SetPropertyStr(c,o,"relativePanTilt",JS_NewBool(c,!v.relative_pan_tilt.empty()));
    JS_SetPropertyStr(c,o,"relativeZoom",JS_NewBool(c,!v.relative_zoom.empty()));
    JS_SetPropertyStr(c,o,"continuousPanTilt",JS_NewBool(c,!v.continuous_pan_tilt.empty()));
    JS_SetPropertyStr(c,o,"continuousZoom",JS_NewBool(c,!v.continuous_zoom.empty()));
    return o;
}

bool profile_arg(JSContext*ctx,int argc,JSValueConst*argv,std::string&profile){return argc>0&&string_value(ctx,argv[0],profile)&&!profile.empty();}
JSValue ptz_spaces(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv){auto h=handle_from(ctx,self,ptz_class_id);std::string p;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");if(!profile_arg(ctx,argc,argv,p))return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"GetConfigurationOptions","profile token is required"));auto input=operation_input(ctx,argc,argv,1);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetConfigurationOptions","invalid timeout or aborted signal"));auto task=h->ptz->spaces(p,call_options(input->timeout,*input->cancellation));return start_task(ctx,h->service->host,std::move(task),ptz_spaces_value,input->options,input->cancellation);}
JSValue ptz_status(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv){auto h=handle_from(ctx,self,ptz_class_id);std::string p;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");if(!profile_arg(ctx,argc,argv,p))return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"GetStatus","profile token is required"));auto input=operation_input(ctx,argc,argv,1);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetStatus","invalid timeout or aborted signal"));auto task=h->ptz->status(p,call_options(input->timeout,*input->cancellation));return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,wlonvif::PtzStatus&v){JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"position",make_vector(c,v.position));JS_SetPropertyStr(c,o,"moving",JS_NewBool(c,v.moving));JS_SetPropertyStr(c,o,"positionReliable",JS_NewBool(c,v.position_reliable));return o;},input->options,input->cancellation);}

enum class MoveKind{continuous,relative,absolute};
JSValue ptz_move(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv,MoveKind kind){auto h=handle_from(ctx,self,ptz_class_id);std::string p;wlonvif::PtzVector v;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");const char*op=kind==MoveKind::continuous?"ContinuousMove":kind==MoveKind::relative?"RelativeMove":"AbsoluteMove";if(!profile_arg(ctx,argc,argv,p)||argc<2||!parse_vector(ctx,argv[1],v))return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,op,"profile token and normalized PTZ vector are required"));auto input=operation_input(ctx,argc,argv,2);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,op,"invalid timeout or aborted signal"));auto options=call_options(input->timeout,*input->cancellation);wlonvif::Task<wlonvif::Result<wlonvif::MoveResult>> task=kind==MoveKind::continuous?h->ptz->continuous_move(p,v,options):kind==MoveKind::relative?h->ptz->relative_move(p,v,options):h->ptz->absolute_move(p,v,std::nullopt,options);return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,wlonvif::MoveResult&v){JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"requestMayHaveBeenApplied",JS_NewBool(c,v.request_may_have_been_applied));return o;},input->options,input->cancellation);}
JSValue ptz_continuous(JSContext*c,JSValueConst s,int n,JSValueConst*a){return ptz_move(c,s,n,a,MoveKind::continuous);}JSValue ptz_relative(JSContext*c,JSValueConst s,int n,JSValueConst*a){return ptz_move(c,s,n,a,MoveKind::relative);}JSValue ptz_absolute(JSContext*c,JSValueConst s,int n,JSValueConst*a){return ptz_move(c,s,n,a,MoveKind::absolute);}

JSValue ptz_stop_call(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv,bool emergency){auto h=handle_from(ctx,self,ptz_class_id);std::string p;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");if(!profile_arg(ctx,argc,argv,p))return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"Stop","profile token is required"));auto input=operation_input(ctx,argc,argv,1);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"Stop","invalid timeout or aborted signal"));auto native_options=call_options(input->timeout,*input->cancellation);auto task=emergency?h->ptz->emergency_stop(p,native_options):h->ptz->stop(p,native_options);return start_void_task(ctx,h->service->host,std::move(task),[](JSContext*){return JS_UNDEFINED;},input->options,input->cancellation);}
JSValue ptz_stop(JSContext*c,JSValueConst s,int n,JSValueConst*a){return ptz_stop_call(c,s,n,a,false);}JSValue ptz_emergency(JSContext*c,JSValueConst s,int n,JSValueConst*a){return ptz_stop_call(c,s,n,a,true);}

JSValue ptz_presets(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv){auto h=handle_from(ctx,self,ptz_class_id);std::string p;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");if(!profile_arg(ctx,argc,argv,p))return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"GetPresets","profile token is required"));auto input=operation_input(ctx,argc,argv,1);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GetPresets","invalid timeout or aborted signal"));auto task=h->ptz->get_presets(p,call_options(input->timeout,*input->cancellation));return start_task(ctx,h->service->host,std::move(task),[](JSContext*c,std::vector<wlonvif::Preset>&v){JSValue a=JS_NewArray(c);for(uint32_t i=0;i<v.size();++i){JSValue o=JS_NewObject(c);JS_SetPropertyStr(c,o,"token",JS_NewString(c,v[i].token.c_str()));JS_SetPropertyStr(c,o,"name",JS_NewString(c,v[i].name.c_str()));JS_SetPropertyStr(c,o,"position",make_vector(c,v[i].position));JS_SetPropertyUint32(c,a,i,o);}return a;},input->options,input->cancellation);}
JSValue ptz_goto(JSContext*ctx,JSValueConst self,int argc,JSValueConst*argv){auto h=handle_from(ctx,self,ptz_class_id);std::string p,t;if(!h||!h->ptz)return JS_ThrowTypeError(ctx,"invalid PtzClient");if(!profile_arg(ctx,argc,argv,p)||argc<2||!string_value(ctx,argv[1],t)||t.empty())return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"GotoPreset","profile and preset tokens are required"));auto input=operation_input(ctx,argc,argv,2);if(!input)return rejected(ctx,h->service->host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"GotoPreset","invalid timeout or aborted signal"));auto task=h->ptz->goto_preset(p,t,call_options(input->timeout,*input->cancellation));return start_void_task(ctx,h->service->host,std::move(task),[](JSContext*){return JS_UNDEFINED;},input->options,input->cancellation);}

template <class Ref>
Ref object_ref(JSContext* ctx, JSValueConst value, JSClassID id) {
    auto* box = static_cast<Ref*>(JS_GetOpaque2(ctx, value, id));
    return box ? *box : Ref{};
}

bool positive_size_prop(JSContext* ctx, JSValueConst object, const char* name,
                        std::size_t fallback, std::size_t& output) {
    JSValue value = get_prop(ctx, object, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) { JS_FreeValue(ctx, value); output = fallback; return true; }
    int64_t number{};
    const bool ok = JS_ToInt64(ctx, &number, value) == 0 && number > 0;
    JS_FreeValue(ctx, value);
    if (ok) output = static_cast<std::size_t>(number);
    return ok;
}

bool empty_topics(JSContext* ctx, JSValueConst options) {
    JSValue topics = get_prop(ctx, options, "topics");
    if (JS_IsUndefined(topics) || JS_IsNull(topics)) { JS_FreeValue(ctx, topics); return true; }
    if (!JS_IsArray(ctx, topics)) { JS_FreeValue(ctx, topics); return false; }
    JSValue length = JS_GetPropertyStr(ctx, topics, "length");
    int64_t count{};
    const bool empty = JS_ToInt64(ctx, &count, length) == 0 && count == 0;
    JS_FreeValue(ctx, length);
    JS_FreeValue(ctx, topics);
    return empty;
}

JSValue subscription_info_value(JSContext* ctx, const wlonvif::PullPointSubscriptionInfo& info) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "token", JS_NewString(ctx, info.token.c_str()));
    JS_SetPropertyStr(ctx, object, "endpoint", JS_NewString(ctx, info.endpoint.c_str()));
    JS_SetPropertyStr(ctx, object, "terminationTime", JS_NewString(ctx, iso_utc(info.termination_time).c_str()));
    return object;
}

JSValue event_value(JSContext* ctx, const wlonvif::Event& event) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "kind", JS_NewString(ctx, "event"));
    JS_SetPropertyStr(ctx, object, "topic", JS_NewString(ctx, event.topic.c_str()));
    JS_SetPropertyStr(ctx, object, "utcTime", JS_NewString(ctx, iso_utc(event.utc_time).c_str()));
    JS_SetPropertyStr(ctx, object, "source", JS_NewString(ctx, event.source.c_str()));
    JS_SetPropertyStr(ctx, object, "data", JS_NewString(ctx, event.data.c_str()));
    JS_SetPropertyStr(ctx, object, "sequence", JS_NewInt64(ctx, static_cast<int64_t>(event.sequence)));
    return object;
}

JSValue iterator_result(JSContext* ctx, JSValue value, bool done) {
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "value", value);
    JS_SetPropertyStr(ctx, result, "done", JS_NewBool(ctx, done));
    return result;
}

JSValue events_properties(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto h = handle_from(ctx, self, events_class_id);
    if (!h) return JS_ThrowTypeError(ctx, "invalid EventClient");
    return rejected(ctx, h->service->host, local_error(ctx, wlonvif::ErrorCategory::unsupported,
                    "GetEventProperties", "event topic discovery is not implemented by wlonvif"));
}

JSValue new_subscription(JSContext* ctx, HandleRef owner,
                         std::shared_ptr<wlonvif::PullPointSubscription> native) {
    JSValue object = JS_NewObjectClass(ctx, subscription_class_id);
    if (JS_IsException(object)) return object;
    auto ref = std::make_shared<SubscriptionHandle>();
    ref->owner = std::move(owner); ref->native = std::move(native);
    { std::lock_guard lock(ref->owner->children_mutex); ref->owner->subscriptions.push_back(ref); }
    { std::lock_guard lock(ref->owner->service->cleanup_mutex);
      ref->owner->service->cleanup_hooks.push_back([weak = std::weak_ptr<SubscriptionHandle>(ref)] {
          if (auto child = weak.lock()) schedule_subscription_cleanup(std::move(child));
      }); }
    JS_SetOpaque(object, new SubscriptionRef(std::move(ref)));
    return object;
}

JSValue events_create(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, events_class_id);
    if (!h || !h->events) return JS_ThrowTypeError(ctx, "invalid EventClient");
    JSValueConst options = argc ? argv[0] : JS_UNDEFINED;
    if (!empty_topics(ctx, options)) return rejected(ctx, h->service->host,
        local_error(ctx, wlonvif::ErrorCategory::unsupported, "CreatePullPointSubscription",
                    "topic filters are not supported; topics must be empty"));
    std::size_t lifetime{};
    if (!positive_size_prop(ctx, options, "lifetimeMs", 60000, lifetime)) return rejected(ctx, h->service->host,
        local_error(ctx, wlonvif::ErrorCategory::invalid_argument, "CreatePullPointSubscription", "lifetimeMs must be positive"));
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, h->service->host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                "CreatePullPointSubscription", "invalid timeout or aborted signal"));
    wlonvif::PullPointSubscriptionOptions native;
    native.requested_lifetime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(lifetime));
    if (native.requested_lifetime < 1s) native.requested_lifetime = 1s;
    return start_task(ctx, h->service->host,
        h->events->create_pullpoint(native, call_options(input->timeout, *input->cancellation)),
        [h](JSContext* c, std::shared_ptr<wlonvif::PullPointSubscription>& value) {
            return new_subscription(c, h, value);
        }, input->options, input->cancellation);
}

JSValue subscription_pull(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto ref = object_ref<SubscriptionRef>(ctx, self, subscription_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid PullPointSubscription");
    JSValueConst options = argc ? argv[0] : JS_UNDEFINED;
    std::size_t limit{};
    if (!positive_size_prop(ctx, options, "messageLimit", 32, limit)) return rejected(ctx, ref->owner->service->host,
        local_error(ctx, wlonvif::ErrorCategory::invalid_argument, "PullMessages", "messageLimit must be positive"));
    int server_timeout = timeout_ms(ctx, options, 15000);
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, ref->owner->service->host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                "PullMessages", "invalid timeout or aborted signal"));
    return start_task(ctx, ref->owner->service->host,
        ref->native->pull(std::chrono::milliseconds(server_timeout), limit,
                          call_options(input->timeout, *input->cancellation)),
        [](JSContext* c, std::vector<wlonvif::Event>& events) {
            JSValue array = JS_NewArray(c);
            for (uint32_t i = 0; i < events.size(); ++i) JS_SetPropertyUint32(c, array, i, event_value(c, events[i]));
            return array;
        }, input->options, input->cancellation);
}

JSValue subscription_renew(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto ref = object_ref<SubscriptionRef>(ctx, self, subscription_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid PullPointSubscription");
    JSValueConst options = argc ? argv[0] : JS_UNDEFINED;
    std::size_t lifetime{};
    if (!positive_size_prop(ctx, options, "lifetimeMs", 60000, lifetime)) return rejected(ctx, ref->owner->service->host,
        local_error(ctx, wlonvif::ErrorCategory::invalid_argument, "Renew", "lifetimeMs must be positive"));
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, ref->owner->service->host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                "Renew", "invalid timeout or aborted signal"));
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(lifetime));
    if (seconds < 1s) seconds = 1s;
    return start_task(ctx, ref->owner->service->host,
        ref->native->renew(seconds, call_options(input->timeout, *input->cancellation)),
        subscription_info_value, input->options, input->cancellation);
}

JSValue subscription_info(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<SubscriptionRef>(ctx, self, subscription_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid PullPointSubscription");
    return subscription_info_value(ctx, ref->native->info());
}

JSValue subscription_state(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<SubscriptionRef>(ctx, self, subscription_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid PullPointSubscription");
    const auto state = ref->native->state();
    return JS_NewString(ctx, state == wlonvif::PullPointSubscriptionState::open ? "open" :
                             state == wlonvif::PullPointSubscriptionState::closing ? "closing" : "closed");
}

JSValue subscription_close(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<SubscriptionRef>(ctx, self, subscription_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid PullPointSubscription");
    return start_task(ctx, ref->owner->service->host, ref->native->close(5000ms),
        [ref](JSContext* c, wlonvif::CloseResult& value) {
            if (value.cleanup_complete) ref->closed = true;
            JSValue result = JS_NewObject(c);
            JS_SetPropertyStr(c, result, "cleanupComplete", JS_NewBool(c, value.cleanup_complete));
            JS_SetPropertyStr(c, result, "safetyComplete", JS_NewBool(c, value.safety_complete));
            return result;
        });
}

JSValue new_stream(JSContext* ctx, HandleRef owner, std::shared_ptr<wlonvif::ManagedEventStream> native) {
    JSValue object = JS_NewObjectClass(ctx, stream_class_id);
    if (JS_IsException(object)) return object;
    auto ref = std::make_shared<StreamHandle>(); ref->owner = std::move(owner); ref->native = std::move(native);
    { std::lock_guard lock(ref->owner->children_mutex); ref->owner->streams.push_back(ref); }
    { std::lock_guard lock(ref->owner->service->cleanup_mutex);
      ref->owner->service->cleanup_hooks.push_back([weak = std::weak_ptr<StreamHandle>(ref)] {
          if (auto child = weak.lock()) schedule_stream_cleanup(std::move(child));
      }); }
    JS_SetOpaque(object, new StreamRef(std::move(ref)));
    return object;
}

JSValue events_subscribe(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto h = handle_from(ctx, self, events_class_id);
    if (!h || !h->events) return JS_ThrowTypeError(ctx, "invalid EventClient");
    JSValueConst options = argc ? argv[0] : JS_UNDEFINED;
    if (!empty_topics(ctx, options)) return rejected(ctx, h->service->host, local_error(ctx,
        wlonvif::ErrorCategory::unsupported, "CreatePullPointSubscription", "topic filters are not supported; topics must be empty"));
    JSValue reconnect = get_prop(ctx, options, "reconnect");
    const bool disabled = !JS_IsUndefined(reconnect) && JS_ToBool(ctx, reconnect) == 0;
    JS_FreeValue(ctx, reconnect);
    if (disabled) return rejected(ctx, h->service->host, local_error(ctx, wlonvif::ErrorCategory::unsupported,
                                "subscribe", "managed streams always reconnect; reconnect:false is unsupported"));
    wlonvif::ManagedEventOptions native;
    if (!positive_size_prop(ctx, options, "queueLimit", native.queue_capacity, native.queue_capacity))
        return rejected(ctx, h->service->host, local_error(ctx, wlonvif::ErrorCategory::invalid_argument,
                                                           "subscribe", "queueLimit must be positive"));
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, h->service->host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                                                   "subscribe", "invalid timeout or aborted signal"));
    return start_task(ctx, h->service->host,
        h->events->open_managed_stream(native, call_options(input->timeout, *input->cancellation)),
        [h](JSContext* c, std::shared_ptr<wlonvif::ManagedEventStream>& stream) { return new_stream(c, h, stream); },
        input->options, input->cancellation);
}

JSValue stream_next(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    auto ref = object_ref<StreamRef>(ctx, self, stream_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid ManagedEventStream");
    if (ref->closed) return rejected(ctx, ref->owner->service->host,
                                     local_error(ctx, wlonvif::ErrorCategory::closed, "next", "stream is closed"));
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, ref->owner->service->host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                                                           "next", "invalid timeout or aborted signal"));
    return start_task(ctx, ref->owner->service->host,
        ref->native->next(call_options(input->timeout, *input->cancellation)),
        [](JSContext* c, wlonvif::EventOrHealth& item) {
            JSValue value;
            if (auto* event = std::get_if<wlonvif::Event>(&item)) value = event_value(c, *event);
            else { value = JS_NewObject(c); JS_SetPropertyStr(c, value, "kind", JS_NewString(c,
                std::get<wlonvif::StreamHealth>(item) == wlonvif::StreamHealth::possible_event_loss ?
                "continuityLost" : "eventsDropped")); }
            return iterator_result(c, value, false);
        }, input->options, input->cancellation);
}

JSValue stream_identity(JSContext* ctx, JSValueConst self, int, JSValueConst*) { return JS_DupValue(ctx, self); }

JSValue stream_close_impl(JSContext* ctx, JSValueConst self, bool iterator_return) {
    auto ref = object_ref<StreamRef>(ctx, self, stream_class_id);
    if (!ref || !ref->native) return JS_ThrowTypeError(ctx, "invalid ManagedEventStream");
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, ref->owner->service->host, promise);
    if (JS_IsException(result)) return result;
    if (ref->closed.exchange(true)) {
        settle(promise, true, iterator_return ? iterator_result(ctx, JS_UNDEFINED, true) : JS_UNDEFINED);
        return result;
    }
    auto* host = ref->owner->service->host;
    host->async().beginOperation();
    auto runtime = ref->owner->service->runtime;
    if (!runtime || !runtime->post([ref, promise, iterator_return]() mutable {
        auto source = std::make_shared<wlonvif::CancellationSource>();
        auto task = ref->native->close(call_options(5000, *source));
        std::move(task).start([ref, promise, source, iterator_return](wlonvif::Result<void> native) mutable {
            promise->host->async().post([ref, promise, native = std::move(native), iterator_return]() mutable {
                if (native) settle(promise, true, iterator_return ? iterator_result(promise->ctx, JS_UNDEFINED, true) : JS_UNDEFINED);
                else settle(promise, false, make_error(promise->ctx, native.error()));
                ref->native.reset();
                promise->host->async().endOperation();
            });
        });
    })) {
        std::thread([ref]() mutable { ref->native.reset(); }).detach();
        settle(promise, false, local_error(ctx, wlonvif::ErrorCategory::closed, "close", "native runtime is closed"));
        host->async().endOperation();
    }
    return result;
}
JSValue stream_close(JSContext* c, JSValueConst s, int, JSValueConst*) { return stream_close_impl(c, s, false); }
JSValue stream_return(JSContext* c, JSValueConst s, int, JSValueConst*) { return stream_close_impl(c, s, true); }

JSValue new_child(JSContext*ctx,HandleRef h,JSClassID id){JSValue o=JS_NewObjectClass(ctx,id);if(JS_IsException(o))return o;JS_SetOpaque(o,new HandleRef(std::move(h)));return o;}
JSValue session_object(JSContext*ctx,HandleRef h){JSValue o=new_child(ctx,h,session_class_id);if(JS_IsException(o))return o;JS_SetPropertyStr(ctx,o,"deviceServiceUrl",JS_NewString(ctx,h->url.c_str()));JS_SetPropertyStr(ctx,o,"authentication",JS_NewString(ctx,"auto"));JS_SetPropertyStr(ctx,o,"closed",JS_NewBool(ctx,false));JS_SetPropertyStr(ctx,o,"media",new_child(ctx,h,media_class_id));JS_SetPropertyStr(ctx,o,"ptz",new_child(ctx,h,ptz_class_id));JS_SetPropertyStr(ctx,o,"events",new_child(ctx,h,events_class_id));return o;}

/** QuickJS implementation of connect(deviceServiceUrl, options). */
JSValue connect_js(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){auto*host=static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));if(!host)return JS_ThrowInternalError(ctx,"runtime unavailable");std::string url;if(argc<1||!string_value(ctx,argv[0],url)||url.empty())return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"connect","device service URL is required"));JSValueConst options=argc>1?argv[1]:JS_UNDEFINED;int timeout=timeout_ms(ctx,options);if(timeout<=0)return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::invalid_argument,"connect","timeoutMs must be positive"));if(already_aborted(ctx,options))return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::cancelled,"connect","operation was already aborted"));std::string auth="auto";if(string_prop(ctx,options,"authentication",auth)&&auth!="auto")return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::unsupported,"connect","only automatic authentication is supported"));JSValue tls=get_prop(ctx,options,"tls");if(JS_IsObject(tls)){JSPropertyEnum*props=nullptr;uint32_t count=0;if(JS_GetOwnPropertyNames(ctx,&props,&count,tls,JS_GPN_STRING_MASK|JS_GPN_ENUM_ONLY)==0){for(uint32_t i=0;i<count;++i)JS_FreeAtom(ctx,props[i].atom);js_free(ctx,props);}if(count){JS_FreeValue(ctx,tls);return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::unsupported,"connect","custom TLS material is unsupported"));}}JS_FreeValue(ctx,tls);auto service=service_for(host);if(!service)return rejected(ctx,host,local_error(ctx,wlonvif::ErrorCategory::internal,"connect","failed to create native ONVIF runtime"));wlonvif::Credentials credentials;bool have_credentials=false;JSValue creds=get_prop(ctx,options,"credentials");if(JS_IsObject(creds)){have_credentials=string_prop(ctx,creds,"username",credentials.username);std::string password;if(string_prop(ctx,creds,"password",password)){credentials.password=std::move(password);have_credentials=true;}}JS_FreeValue(ctx,creds);auto source=std::make_shared<wlonvif::CancellationSource>();wlonvif::CurlTransportOptions transport_options;transport_options.runtime=service->runtime;transport_options.candidate_authorizer=service->authorizer;wlonvif::DeviceSessionOptions session_options;session_options.device_url=url;session_options.transport=wlonvif::make_curl_transport(std::move(transport_options));session_options.endpoint_policy=wlonvif::make_same_host_policy();session_options.runtime=service->runtime;if(have_credentials)session_options.credential_provider=wlonvif::make_fixed_credential_provider(std::move(credentials));session_options.call=call_options(timeout,*source);return start_task(ctx,host,wlonvif::DeviceSession::connect(std::move(session_options)),[service,url=std::move(url)](JSContext*c,std::shared_ptr<wlonvif::DeviceSession>&session){auto h=std::make_shared<SessionHandle>();h->service=service;h->session=session;h->url=url;auto media=session->media();if(media)h->media=media.value();auto ptz=session->ptz();if(ptz)h->ptz=ptz.value();auto events=session->event_client();if(events)h->events=events.value();return session_object(c,std::move(h));},options,std::move(source));}

bool string_array_prop(JSContext* ctx, JSValueConst object, const char* name,
                       std::vector<std::string>& output) {
    JSValue array = get_prop(ctx, object, name);
    if (JS_IsUndefined(array) || JS_IsNull(array)) { JS_FreeValue(ctx, array); return true; }
    if (!JS_IsArray(ctx, array)) { JS_FreeValue(ctx, array); return false; }
    JSValue length_value = JS_GetPropertyStr(ctx, array, "length");
    int64_t length{};
    if (JS_ToInt64(ctx, &length, length_value) != 0 || length < 0 || length > 1024) {
        JS_FreeValue(ctx, length_value); JS_FreeValue(ctx, array); return false;
    }
    JS_FreeValue(ctx, length_value);
    for (int64_t i = 0; i < length; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, array, static_cast<uint32_t>(i));
        std::string text;
        const bool ok = string_value(ctx, item, text) && !text.empty();
        JS_FreeValue(ctx, item);
        if (!ok) { JS_FreeValue(ctx, array); return false; }
        output.push_back(std::move(text));
    }
    JS_FreeValue(ctx, array);
    return true;
}

JSValue candidate_value(JSContext* ctx, const wlonvif::DeviceCandidate& candidate) {
    JSValue object = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, object, "address", JS_NewString(ctx, candidate.address.c_str()));
    if (candidate.device_service)
        JS_SetPropertyStr(ctx, object, "deviceServiceUrl", JS_NewString(ctx, candidate.device_service->c_str()));
    const char* evidence = candidate.evidence == wlonvif::DeviceEvidence::onvif_response ? "onvifResponse" :
        candidate.evidence == wlonvif::DeviceEvidence::soap_fault ? "soapFault" :
        candidate.evidence == wlonvif::DeviceEvidence::authentication_challenge ? "authenticationChallenge" : "discovery";
    JS_SetPropertyStr(ctx, object, "evidence", JS_NewString(ctx, evidence));
    JS_SetPropertyStr(ctx, object, "diagnostic", JS_NewString(ctx, candidate.diagnostic.c_str()));
    if (candidate.discovery) {
        JS_SetPropertyStr(ctx, object, "endpointReference", JS_NewString(ctx, candidate.discovery->endpoint_reference.c_str()));
        JS_SetPropertyStr(ctx, object, "scopes", js_string_array(ctx, candidate.discovery->scopes));
        JS_SetPropertyStr(ctx, object, "xaddrs", js_string_array(ctx, candidate.discovery->xaddrs));
    }
    return object;
}

JSValue discovery_next(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<DiscoveryRef>(ctx, self, discovery_class_id);
    if (!ref) return JS_ThrowTypeError(ctx, "invalid DiscoverySession");
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, ref->service->host, promise);
    if (JS_IsException(result)) return result;
    if (ref->closed || ref->cursor >= ref->candidates.size())
        settle(promise, true, iterator_result(ctx, JS_UNDEFINED, true));
    else settle(promise, true, iterator_result(ctx, candidate_value(ctx, ref->candidates[ref->cursor++]), false));
    return result;
}
JSValue discovery_identity(JSContext* ctx, JSValueConst self, int, JSValueConst*) { return JS_DupValue(ctx, self); }
JSValue discovery_close(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<DiscoveryRef>(ctx, self, discovery_class_id);
    if (!ref) return JS_ThrowTypeError(ctx, "invalid DiscoverySession");
    ref->closed = true; ref->candidates.clear();
    std::shared_ptr<Promise> promise; JSValue result = new_promise(ctx, ref->service->host, promise);
    if (!JS_IsException(result)) settle(promise, true, JS_UNDEFINED);
    return result;
}
JSValue discovery_return(JSContext* ctx, JSValueConst self, int, JSValueConst*) {
    auto ref = object_ref<DiscoveryRef>(ctx, self, discovery_class_id);
    if (!ref) return JS_ThrowTypeError(ctx, "invalid DiscoverySession");
    ref->closed = true; ref->candidates.clear();
    std::shared_ptr<Promise> promise; JSValue result = new_promise(ctx, ref->service->host, promise);
    if (!JS_IsException(result)) settle(promise, true, iterator_result(ctx, JS_UNDEFINED, true));
    return result;
}

/** QuickJS implementation of discover(options). */
JSValue discover_js(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!host) return JS_ThrowInternalError(ctx, "runtime unavailable");
    JSValueConst options = argc ? argv[0] : JS_UNDEFINED;
    std::vector<std::string> networks;
    if (!string_array_prop(ctx, options, "networks", networks) || networks.empty())
        return rejected(ctx, host, local_named_error(ctx, wlonvif::ErrorCategory::unsupported,
            "onvif_permission_unavailable", "discover",
            "explicit networks are required; implicit interface enumeration is unavailable"));
    std::size_t maximum_hosts{};
    if (!positive_size_prop(ctx, options, "maximumHosts", 256, maximum_hosts) || maximum_hosts > 4096)
        return rejected(ctx, host, local_error(ctx, wlonvif::ErrorCategory::invalid_argument,
                                               "discover", "maximumHosts must be between 1 and 4096"));
    auto input = operation_input(ctx, argc, argv, 0);
    if (!input) return rejected(ctx, host, local_error(ctx, wlonvif::ErrorCategory::cancelled,
                                                       "discover", "invalid timeout or aborted signal"));
    auto service = service_for(host);
    if (!service) return rejected(ctx, host, local_error(ctx, wlonvif::ErrorCategory::internal,
                                                         "discover", "failed to create native ONVIF runtime"));
    wlonvif::NetworkDiscoveryOptions native;
    native.multicast = false;
    native.direct_probe = true;
    native.interfaces = {"0.0.0.0"};
    native.scan_networks = std::move(networks);
    native.maximum_hosts = maximum_hosts;
    native.maximum_results = std::min<std::size_t>(maximum_hosts, 256);
    native.authorizer = service->discovery_authorizer;
    native.require_authorization = true;
    native.connect_timeout = 300ms;
    native.request_timeout = 1000ms;
    return start_task(ctx, host,
        wlonvif::discover_devices(service->runtime, wlonvif::make_same_host_policy(), native,
                                  call_options(input->timeout, *input->cancellation)),
        [service](JSContext* c, std::vector<wlonvif::DeviceCandidate>& candidates) {
            JSValue object = JS_NewObjectClass(c, discovery_class_id);
            if (JS_IsException(object)) return object;
            auto ref = std::make_shared<DiscoveryHandle>(); ref->service = service; ref->candidates = std::move(candidates);
            JS_SetOpaque(object, new DiscoveryRef(std::move(ref)));
            return object;
        }, input->options, input->cancellation);
}

/** Compatibility implementation of localNetworks(); new code uses wl2:uv. */
JSValue local_networks_js(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* host = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!host) return JS_ThrowInternalError(ctx, "runtime unavailable");
    auto allowed = host->authorizeNetworkListen("0.0.0.0", 0);
    if (!allowed) return rejected(ctx, host, local_named_error(ctx,
        wlonvif::ErrorCategory::host_permission_denied, "onvif_permission_denied",
        "localNetworks", allowed.error().message()));
    auto networks = wlonvif::local_ipv4_networks(65536);
    if (!networks) return rejected(ctx, host, make_error(ctx, networks.error()));
    std::shared_ptr<Promise> promise;
    JSValue result = new_promise(ctx, host, promise);
    if (!JS_IsException(result)) settle(promise, true, js_string_array(ctx, networks.value()));
    return result;
}
JSValue error_ctor(JSContext*ctx,JSValueConst,int argc,JSValueConst*argv){std::string message="ONVIF error";if(argc>0)string_value(ctx,argv[0],message);return local_error(ctx,wlonvif::ErrorCategory::internal,"unknown",message);}

void register_classes(JSContext*ctx){JSRuntime*rt=JS_GetRuntime(ctx);if(!session_class_id)JS_NewClassID(&session_class_id);JSClassDef sd{};sd.class_name="DeviceSession";sd.finalizer=session_finalizer;JS_NewClass(rt,session_class_id,&sd);JSValue sp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,sp,"getDeviceInformation",JS_NewCFunction(ctx,session_info,"getDeviceInformation",1));JS_SetPropertyStr(ctx,sp,"getSystemDateAndTime",JS_NewCFunction(ctx,session_time,"getSystemDateAndTime",1));JS_SetPropertyStr(ctx,sp,"getScopes",JS_NewCFunction(ctx,session_scopes,"getScopes",1));JS_SetPropertyStr(ctx,sp,"getServices",JS_NewCFunction(ctx,session_services,"getServices",1));JS_SetPropertyStr(ctx,sp,"getCapabilities",JS_NewCFunction(ctx,session_capabilities,"getCapabilities",1));JS_SetPropertyStr(ctx,sp,"close",JS_NewCFunction(ctx,session_close,"close",0));JS_SetClassProto(ctx,session_class_id,sp);if(!media_class_id)JS_NewClassID(&media_class_id);JSClassDef md{};md.class_name="OnvifMediaClient";md.finalizer=media_finalizer;JS_NewClass(rt,media_class_id,&md);JSValue mp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,mp,"getProfiles",JS_NewCFunction(ctx,media_profiles,"getProfiles",1));JS_SetPropertyStr(ctx,mp,"getStreamUri",JS_NewCFunction(ctx,media_stream,"getStreamUri",2));JS_SetPropertyStr(ctx,mp,"getSnapshotUri",JS_NewCFunction(ctx,media_snapshot,"getSnapshotUri",2));JS_SetClassProto(ctx,media_class_id,mp);if(!ptz_class_id)JS_NewClassID(&ptz_class_id);JSClassDef pd{};pd.class_name="OnvifPtzClient";pd.finalizer=ptz_finalizer;JS_NewClass(rt,ptz_class_id,&pd);JSValue pp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,pp,"getCapabilities",JS_NewCFunction(ctx,ptz_spaces,"getCapabilities",2));JS_SetPropertyStr(ctx,pp,"getStatus",JS_NewCFunction(ctx,ptz_status,"getStatus",2));JS_SetPropertyStr(ctx,pp,"continuousMove",JS_NewCFunction(ctx,ptz_continuous,"continuousMove",3));JS_SetPropertyStr(ctx,pp,"relativeMove",JS_NewCFunction(ctx,ptz_relative,"relativeMove",3));JS_SetPropertyStr(ctx,pp,"absoluteMove",JS_NewCFunction(ctx,ptz_absolute,"absoluteMove",3));JS_SetPropertyStr(ctx,pp,"stop",JS_NewCFunction(ctx,ptz_stop,"stop",2));JS_SetPropertyStr(ctx,pp,"emergencyStop",JS_NewCFunction(ctx,ptz_emergency,"emergencyStop",2));JS_SetPropertyStr(ctx,pp,"getPresets",JS_NewCFunction(ctx,ptz_presets,"getPresets",2));JS_SetPropertyStr(ctx,pp,"gotoPreset",JS_NewCFunction(ctx,ptz_goto,"gotoPreset",3));JS_SetClassProto(ctx,ptz_class_id,pp);
if(!events_class_id)JS_NewClassID(&events_class_id);JSClassDef ed{};ed.class_name="OnvifEventClient";ed.finalizer=events_finalizer;JS_NewClass(rt,events_class_id,&ed);JSValue ep=JS_NewObject(ctx);JS_SetPropertyStr(ctx,ep,"createPullPoint",JS_NewCFunction(ctx,events_create,"createPullPoint",1));JS_SetPropertyStr(ctx,ep,"subscribe",JS_NewCFunction(ctx,events_subscribe,"subscribe",1));JS_SetPropertyStr(ctx,ep,"getEventProperties",JS_NewCFunction(ctx,events_properties,"getEventProperties",0));JS_SetClassProto(ctx,events_class_id,ep);
if(!subscription_class_id)JS_NewClassID(&subscription_class_id);JSClassDef subd{};subd.class_name="PullPointSubscription";subd.finalizer=subscription_finalizer;JS_NewClass(rt,subscription_class_id,&subd);JSValue subp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,subp,"pull",JS_NewCFunction(ctx,subscription_pull,"pull",1));JS_SetPropertyStr(ctx,subp,"renew",JS_NewCFunction(ctx,subscription_renew,"renew",1));JS_SetPropertyStr(ctx,subp,"info",JS_NewCFunction(ctx,subscription_info,"info",0));JS_SetPropertyStr(ctx,subp,"state",JS_NewCFunction(ctx,subscription_state,"state",0));JS_SetPropertyStr(ctx,subp,"close",JS_NewCFunction(ctx,subscription_close,"close",0));JS_SetClassProto(ctx,subscription_class_id,subp);
if(!stream_class_id)JS_NewClassID(&stream_class_id);JSClassDef std{};std.class_name="ManagedEventStream";std.finalizer=stream_finalizer;JS_NewClass(rt,stream_class_id,&std);JSValue stp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,stp,"next",JS_NewCFunction(ctx,stream_next,"next",1));JS_SetPropertyStr(ctx,stp,"return",JS_NewCFunction(ctx,stream_return,"return",0));JS_SetPropertyStr(ctx,stp,"close",JS_NewCFunction(ctx,stream_close,"close",0));JSValue global=JS_GetGlobalObject(ctx);JSValue symbol=JS_GetPropertyStr(ctx,global,"Symbol");JSValue async_symbol=JS_GetPropertyStr(ctx,symbol,"asyncIterator");JSAtom async_atom=JS_ValueToAtom(ctx,async_symbol);JS_SetProperty(ctx,stp,async_atom,JS_NewCFunction(ctx,stream_identity,"[Symbol.asyncIterator]",0));JS_SetClassProto(ctx,stream_class_id,stp);
if(!discovery_class_id)JS_NewClassID(&discovery_class_id);JSClassDef dd{};dd.class_name="DiscoverySession";dd.finalizer=discovery_finalizer;JS_NewClass(rt,discovery_class_id,&dd);JSValue dp=JS_NewObject(ctx);JS_SetPropertyStr(ctx,dp,"next",JS_NewCFunction(ctx,discovery_next,"next",0));JS_SetPropertyStr(ctx,dp,"return",JS_NewCFunction(ctx,discovery_return,"return",0));JS_SetPropertyStr(ctx,dp,"close",JS_NewCFunction(ctx,discovery_close,"close",0));JS_SetProperty(ctx,dp,async_atom,JS_NewCFunction(ctx,discovery_identity,"[Symbol.asyncIterator]",0));JS_SetClassProto(ctx,discovery_class_id,dp);JS_FreeAtom(ctx,async_atom);JS_FreeValue(ctx,async_symbol);JS_FreeValue(ctx,symbol);JS_FreeValue(ctx,global);}

int init_module(JSContext*ctx,JSModuleDef*module){register_classes(ctx);JS_SetModuleExport(ctx,module,"connect",JS_NewCFunction(ctx,connect_js,"connect",2));JS_SetModuleExport(ctx,module,"discover",JS_NewCFunction(ctx,discover_js,"discover",1));JS_SetModuleExport(ctx,module,"localNetworks",JS_NewCFunction(ctx,local_networks_js,"localNetworks",0));JS_SetModuleExport(ctx,module,"OnvifError",JS_NewCFunction2(ctx,error_ctor,"OnvifError",1,JS_CFUNC_constructor,0));JS_SetModuleExport(ctx,module,"version",JS_NewString(ctx,wlonvif::library_version().data()));return 0;}

#endif
} // namespace

wl2::ModuleInfo wl2_onvif_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:onvif", wl2_onvif_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{.abiVersion=wl2::ModuleAbiVersion,.name="wl2:onvif",.version="0.1.0",.build=WL2_BUILD,.stableId="53f57f40-5507-4a48-82d1-122fad7c6d71",.summary="Secure ONVIF Device, Media1, PTZ, and PullPoint Event bindings.",.api=OnvifApi,.unloadSafe=false};
}

extern "C" void* wl2_onvif_quickjs_module_factory(void* context,const char*moduleName){
#if WL2_HAVE_QUICKJS
    auto*ctx=static_cast<JSContext*>(context);JSModuleDef*module=JS_NewCModule(ctx,moduleName,init_module);if(!module)return nullptr;JS_AddModuleExport(ctx,module,"connect");JS_AddModuleExport(ctx,module,"discover");JS_AddModuleExport(ctx,module,"localNetworks");JS_AddModuleExport(ctx,module,"OnvifError");JS_AddModuleExport(ctx,module,"version");return module;
#else
    (void)context;(void)moduleName;return nullptr;
#endif
}

#if !WL2_ONVIF_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info*out){if(!out)return 1;out->abi_version=wl2::ModuleAbiVersion;out->name="wl2:onvif";out->version="0.1.0";out->build=WL2_BUILD;out->stable_id="53f57f40-5507-4a48-82d1-122fad7c6d71";out->summary="Secure ONVIF Device, Media1, PTZ, and PullPoint Event bindings.";out->api=OnvifApi;out->unload_safe=0;out->required_wl2_version=WL2_VERSION;return 0;}
#endif
