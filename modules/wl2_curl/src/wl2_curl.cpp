#include "wl2_curl/wl2_curl.h"

#include "wl2/runtime.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif

namespace {

constexpr const char* CurlApi = R"(Exports JavaScript module wl2:curl.

Functions:
  get(url, options)
  post(url, body, options)

Class:
  CurlClient({ timeoutMs, followRedirects, headers, insecureSkipTlsVerify })

Security defaults:
  TLS peer and host verification are enabled.
  Redirects are disabled unless requested.
  Only HTTP and HTTPS protocols are intended for the initial API.
  URL-to-script execution is intentionally not provided.)";

#if WL2_HAVE_QUICKJS
struct CurlRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    long timeoutMs = 30000;
    bool followRedirects = false;
    bool insecureSkipTlsVerify = false;
};

struct CurlResponse {
    long status = 0;
    std::string url;
    std::string body;
    std::map<std::string, std::string> headers;
    double totalMs = 0;
};

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(ptr, size * nmemb);
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return size * nmemb;
    }
    auto key = lowercase(line.substr(0, colon));
    auto value = line.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || std::isspace(static_cast<unsigned char>(value.back())))) {
        value.pop_back();
    }
    (*headers)[std::move(key)] = std::move(value);
    return size * nmemb;
}

bool get_string(JSContext* ctx, JSValueConst obj, const char* name, std::string& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    const char* text = JS_ToCString(ctx, value);
    if (!text) {
        JS_FreeValue(ctx, value);
        return false;
    }
    out = text;
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, value);
    return true;
}

bool get_long(JSContext* ctx, JSValueConst obj, const char* name, long& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    int32_t n = 0;
    const bool ok = JS_ToInt32(ctx, &n, value) == 0;
    JS_FreeValue(ctx, value);
    if (ok) {
        out = n;
    }
    return ok;
}

bool get_bool(JSContext* ctx, JSValueConst obj, const char* name, bool& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    out = JS_ToBool(ctx, value) != 0;
    JS_FreeValue(ctx, value);
    return true;
}

void read_headers(JSContext* ctx, JSValueConst options, CurlRequest& request) {
    JSValue headers = JS_GetPropertyStr(ctx, options, "headers");
    if (!JS_IsObject(headers)) {
        JS_FreeValue(ctx, headers);
        return;
    }

    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &len, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0) {
        JS_FreeValue(ctx, headers);
        return;
    }

    for (uint32_t i = 0; i < len; ++i) {
        const char* key = JS_AtomToCString(ctx, props[i].atom);
        JSValue value = JS_GetProperty(ctx, headers, props[i].atom);
        const char* val = JS_ToCString(ctx, value);
        if (key && val) {
            request.headers.emplace_back(std::string(key) + ": " + val);
        }
        if (val) {
            JS_FreeCString(ctx, val);
        }
        if (key) {
            JS_FreeCString(ctx, key);
        }
        JS_FreeValue(ctx, value);
        JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);
    JS_FreeValue(ctx, headers);
}

void read_options(JSContext* ctx, JSValueConst options, CurlRequest& request) {
    if (!JS_IsObject(options)) {
        return;
    }
    get_long(ctx, options, "timeoutMs", request.timeoutMs);
    get_bool(ctx, options, "followRedirects", request.followRedirects);
    get_bool(ctx, options, "insecureSkipTlsVerify", request.insecureSkipTlsVerify);
    read_headers(ctx, options, request);
}

JSValue make_wl2_buffer(JSContext* ctx, const std::string& bytes) {
    JSValue arrayBuffer = JS_NewArrayBufferCopy(
        ctx,
        reinterpret_cast<const uint8_t*>(bytes.data()),
        bytes.size());
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    JS_FreeValue(ctx, global);
    if (!JS_IsObject(wl2)) {
        JS_FreeValue(ctx, wl2);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2 host API is unavailable");
    }

    JSValue bufferNamespace = JS_GetPropertyStr(ctx, wl2, "buffer");
    JS_FreeValue(ctx, wl2);
    if (!JS_IsObject(bufferNamespace)) {
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer host API is unavailable");
    }

    JSValue fromArrayBuffer = JS_GetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer");
    if (!JS_IsFunction(ctx, fromArrayBuffer)) {
        JS_FreeValue(ctx, fromArrayBuffer);
        JS_FreeValue(ctx, bufferNamespace);
        JS_FreeValue(ctx, arrayBuffer);
        return JS_ThrowInternalError(ctx, "wl2.buffer.fromArrayBuffer is unavailable");
    }

    JSValue args[] = {arrayBuffer};
    JSValue result = JS_Call(ctx, fromArrayBuffer, bufferNamespace, 1, args);
    JS_FreeValue(ctx, fromArrayBuffer);
    JS_FreeValue(ctx, bufferNamespace);
    JS_FreeValue(ctx, arrayBuffer);
    return result;
}

CurlResponse perform_request(const CurlRequest& request) {
    CurlResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    curl_slist* headers = nullptr;
    for (const auto& header : request.headers) {
        headers = curl_slist_append(headers, header.c_str());
    }

    auto started = std::chrono::steady_clock::now();

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request.timeoutMs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, request.insecureSkipTlsVerify ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, request.insecureSkipTlsVerify ? 0L : 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "winglib2/0.1");
#ifdef CURLOPT_PROTOCOLS_STR
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    } else if (request.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
        }
    }

    CURLcode code = curl_easy_perform(curl);
    response.totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    if (code != CURLE_OK) {
        std::string message = curl_easy_strerror(code);
        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);
        throw std::runtime_error(message);
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    char* effectiveUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
    if (effectiveUrl) {
        response.url = effectiveUrl;
    }

    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return response;
}

JSValue make_response(JSContext* ctx, const CurlResponse& response) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, static_cast<int32_t>(response.status)));
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, response.url.c_str()));
    JSValue body = make_wl2_buffer(ctx, response.body);
    if (JS_IsException(body)) {
        JS_FreeValue(ctx, obj);
        return body;
    }
    JS_SetPropertyStr(ctx, obj, "body", body);

    JSValue headers = JS_NewObject(ctx);
    for (const auto& [key, value] : response.headers) {
        JS_SetPropertyStr(ctx, headers, key.c_str(), JS_NewString(ctx, value.c_str()));
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    JSValue timing = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, timing, "totalMs", JS_NewFloat64(ctx, response.totalMs));
    JS_SetPropertyStr(ctx, obj, "timing", timing);
    return obj;
}

JSValue throw_curl_error(JSContext* ctx, const char* operation, const std::exception& e) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "CurlError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_curl"));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, e.what()));
    return JS_Throw(ctx, error);
}

JSValue curl_get(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    CurlRequest request;
    request.method = "GET";
    const char* url = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (!url) {
        return JS_ThrowTypeError(ctx, "get(url, options) requires a URL string");
    }
    request.url = url;
    JS_FreeCString(ctx, url);
    if (argc > 1) {
        read_options(ctx, argv[1], request);
    }
    try {
        return make_response(ctx, perform_request(request));
    } catch (const std::exception& e) {
        return throw_curl_error(ctx, "get", e);
    }
}

JSValue curl_post(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    CurlRequest request;
    request.method = "POST";
    const char* url = argc > 0 ? JS_ToCString(ctx, argv[0]) : nullptr;
    if (!url) {
        return JS_ThrowTypeError(ctx, "post(url, body, options) requires a URL string");
    }
    request.url = url;
    JS_FreeCString(ctx, url);

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        const char* body = JS_ToCString(ctx, argv[1]);
        if (body) {
            request.body = body;
            JS_FreeCString(ctx, body);
        }
    }
    if (argc > 2) {
        read_options(ctx, argv[2], request);
    }
    try {
        return make_response(ctx, perform_request(request));
    } catch (const std::exception& e) {
        return throw_curl_error(ctx, "post", e);
    }
}

JSValue curl_client_request(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "CurlClient.request(request) requires an object");
    }

    CurlRequest request;
    get_string(ctx, argv[0], "method", request.method);
    if (request.method.empty()) {
        request.method = "GET";
    }
    std::transform(request.method.begin(), request.method.end(), request.method.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (!get_string(ctx, argv[0], "url", request.url)) {
        return JS_ThrowTypeError(ctx, "request.url is required");
    }
    get_string(ctx, argv[0], "body", request.body);
    read_options(ctx, argv[0], request);

    try {
        return make_response(ctx, perform_request(request));
    } catch (const std::exception& e) {
        return throw_curl_error(ctx, "request", e);
    }
}

JSValue curl_client_ctor(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    (void)newTarget;
    (void)argc;
    (void)argv;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "request", JS_NewCFunction(ctx, curl_client_request, "request", 1));
    return obj;
}

int init_curl_module(JSContext* ctx, JSModuleDef* module) {
    JS_SetModuleExport(ctx, module, "get", JS_NewCFunction(ctx, curl_get, "get", 2));
    JS_SetModuleExport(ctx, module, "post", JS_NewCFunction(ctx, curl_post, "post", 3));
    JS_SetModuleExport(ctx, module, "CurlClient", JS_NewCFunction2(ctx, curl_client_ctor, "CurlClient", 1, JS_CFUNC_constructor, 0));
    return 0;
}
#endif

} // namespace

wl2::ModuleInfo wl2_curl_register_module(wl2::Runtime& runtime) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:curl", wl2_curl_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:curl",
        .version = LIBCURL_VERSION,
        .build = WL2_BUILD,
        .stableId = "7ef0cc47-6153-45f4-a607-c9b88f7654ef",
        .summary = "Secure-by-default HTTP client module backed by libcurl.",
        .api = CurlApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_curl_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_curl_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "get");
    JS_AddModuleExport(ctx, module, "post");
    JS_AddModuleExport(ctx, module, "CurlClient");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_CURL_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:curl";
    out->version = LIBCURL_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "7ef0cc47-6153-45f4-a607-c9b88f7654ef";
    out->summary = "Secure-by-default HTTP client module backed by libcurl.";
    out->api = CurlApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
