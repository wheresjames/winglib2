// wl2:http — embeddable HTTP/1.1 server on RESTinio + standalone Asio.
//
// RESTinio runs on a module-owned io_context driven by one worker thread. A JS
// route handler runs on the JavaScript thread: an inbound request is copied into
// plain data on the io thread and posted to the JS thread through
// Runtime::async().post(); the handler's return value (or resolved Promise) is
// turned into a response and sent back on the io thread. This is the "Model B"
// bridge — the runtime's AsyncHost is a completion queue, not a shared loop, so
// the server owns its own loop and marshals both directions.

#include "wl2_restinio/wl2_restinio.h"

#include "wl2/runtime.h"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <restinio/core.hpp>
#include <restinio/helpers/file_upload.hpp>
#include <restinio/helpers/multipart_body.hpp>
#include <restinio/null_logger.hpp>
#include <restinio/router/express.hpp>
#include <restinio/websocket/websocket.hpp>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif
#ifndef WL2_HTTP_TLS
#define WL2_HTTP_TLS 0
#endif

#if WL2_HTTP_TLS
#include <restinio/tls.hpp>
#endif

#include <zlib.h>

namespace {

namespace asio_ns = restinio::asio_ns;

constexpr const char* HttpApi = R"(Exports JavaScript module wl2:http.

Embeddable HTTP/1.1 server built on RESTinio. Each server is an independent
object (no process-global state); requests are dispatched to JS handlers that
return a response object or a Promise of one.

  new HttpServer({ host?, port?, maxBodyBytes? })
    .route(method, path, handler)   handler(req) -> response | Promise<response>
                                    path supports :params and * wildcards
    .listen() -> Promise<{ host, port }>   (gated by listen permission)
    .close()  -> Promise<void>

  req      = { method, url, path, query, params, headers, body, remoteAddr }
  response = { status?, headers?, body? } | string
             body may be a string or ArrayBuffer/TypedArray

Errors use the shared HttpError shape with stable http_* codes.)";

constexpr std::size_t kDefaultMaxBodyBytes = 1u << 20;

// --- Module io_context + worker thread ------------------------------------

// RESTinio traits: quiet logger, express router as the request handler.
struct http_traits_t : public restinio::default_traits_t {
    using logger_t = restinio::null_logger_t;
    using request_handler_t = restinio::router::express_router_t<>;
};
using http_server_t = restinio::http_server_t<http_traits_t>;
using router_t = restinio::router::express_router_t<>;
namespace ws_ns = restinio::websocket::basic;

#if WL2_HTTP_TLS
// TLS variant: same express router, TLS socket. request_handler_t is the same
// express_router_t<>, so one router type serves both plaintext and HTTPS.
using https_traits_t = restinio::tls_traits_t<restinio::asio_timer_manager_t, restinio::null_logger_t, router_t>;
using https_server_t = restinio::http_server_t<https_traits_t>;
#endif

struct HttpState {
    asio_ns::io_context io;
    std::optional<asio_ns::executor_work_guard<asio_ns::io_context::executor_type>> work;
    std::thread worker;
    bool started = false;
    std::mutex mutex;
};

HttpState& state() {
    static HttpState s;
    return s;
}

asio_ns::io_context& ensure_io() {
    HttpState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.started) {
        s.work.emplace(asio_ns::make_work_guard(s.io));
        s.worker = std::thread([&s] { s.io.run(); });
        s.started = true;
    }
    return s.io;
}

#if WL2_HAVE_QUICKJS

// --- Error contract -------------------------------------------------------

JSValue throw_http_error(JSContext* ctx, const char* code, const char* operation, const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "HttpError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_restinio"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return error;
}

// --- Promise plumbing (mirrors wl2:asio) ----------------------------------

struct Promise {
    JSContext* ctx = nullptr;
    wl2::Runtime* runtime = nullptr;
    JSValue resolve = JS_UNDEFINED;
    JSValue reject = JS_UNDEFINED;
};

JSValue make_promise(JSContext* ctx, wl2::Runtime* runtime, std::shared_ptr<Promise>& promise) {
    JSValue funcs[2];
    JSValue jsPromise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }
    promise = std::make_shared<Promise>();
    promise->ctx = ctx;
    promise->runtime = runtime;
    promise->resolve = funcs[0];
    promise->reject = funcs[1];
    return jsPromise;
}

void settle_value(const std::shared_ptr<Promise>& p, JSValue value) {
    JSValue r = JS_Call(p->ctx, p->resolve, JS_UNDEFINED, 1, &value);
    JS_FreeValue(p->ctx, r);
    JS_FreeValue(p->ctx, value);
    JS_FreeValue(p->ctx, p->resolve);
    JS_FreeValue(p->ctx, p->reject);
    p->resolve = JS_UNDEFINED;
    p->reject = JS_UNDEFINED;
}

void settle_error(const std::shared_ptr<Promise>& p, JSValue error) {
    JSValue r = JS_Call(p->ctx, p->reject, JS_UNDEFINED, 1, &error);
    JS_FreeValue(p->ctx, r);
    JS_FreeValue(p->ctx, error);
    JS_FreeValue(p->ctx, p->resolve);
    JS_FreeValue(p->ctx, p->reject);
    p->resolve = JS_UNDEFINED;
    p->reject = JS_UNDEFINED;
}

JSValue rejected_promise(JSContext* ctx, wl2::Runtime* runtime, JSValue error) {
    std::shared_ptr<Promise> p;
    JSValue jsPromise = make_promise(ctx, runtime, p);
    if (JS_IsException(jsPromise)) {
        JS_FreeValue(ctx, error);
        return jsPromise;
    }
    settle_error(p, error);
    return jsPromise;
}

// --- Value helpers --------------------------------------------------------

bool get_string_prop(JSContext* ctx, JSValueConst obj, const char* name, std::string& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return false;
    }
    const char* s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) {
        return false;
    }
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

bool get_int_prop(JSContext* ctx, JSValueConst obj, const char* name, int64_t& out) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return false;
    }
    int64_t n = 0;
    int rc = JS_ToInt64(ctx, &n, v);
    JS_FreeValue(ctx, v);
    if (rc < 0) {
        return false;
    }
    out = n;
    return true;
}

// Extract bytes from a string, ArrayBuffer, or TypedArray into out.
bool extract_bytes(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsString(value)) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, value);
        if (!s) {
            return false;
        }
        out.assign(s, len);
        JS_FreeCString(ctx, s);
        return true;
    }
    size_t size = 0;
    uint8_t* ptr = JS_GetArrayBuffer(ctx, &size, value);
    if (ptr) {
        out.assign(reinterpret_cast<char*>(ptr), size);
        return true;
    }
    // TypedArray / DataView -> underlying ArrayBuffer slice.
    size_t byteOffset = 0;
    size_t byteLength = 0;
    size_t bytesPerElement = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElement);
    if (!JS_IsException(buffer)) {
        size_t bufSize = 0;
        uint8_t* base = JS_GetArrayBuffer(ctx, &bufSize, buffer);
        bool ok = false;
        if (base && byteOffset + byteLength <= bufSize) {
            out.assign(reinterpret_cast<char*>(base + byteOffset), byteLength);
            ok = true;
        }
        JS_FreeValue(ctx, buffer);
        return ok;
    }
    return false;
}

JSValue make_wl2_buffer(JSContext* ctx, const char* data, std::size_t size) {
    JSValue arrayBuffer = JS_NewArrayBufferCopy(ctx, reinterpret_cast<const uint8_t*>(data), size);
    if (JS_IsException(arrayBuffer)) {
        return arrayBuffer;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue wl2 = JS_GetPropertyStr(ctx, global, "wl2");
    JS_FreeValue(ctx, global);
    JSValue bufferNamespace = JS_GetPropertyStr(ctx, wl2, "buffer");
    JS_FreeValue(ctx, wl2);
    JSValue fromArrayBuffer = JS_GetPropertyStr(ctx, bufferNamespace, "fromArrayBuffer");
    if (!JS_IsFunction(ctx, fromArrayBuffer)) {
        JS_FreeValue(ctx, fromArrayBuffer);
        JS_FreeValue(ctx, bufferNamespace);
        return arrayBuffer; // fall back to the raw ArrayBuffer.
    }
    JSValue args[] = {arrayBuffer};
    JSValue result = JS_Call(ctx, fromArrayBuffer, bufferNamespace, 1, args);
    JS_FreeValue(ctx, fromArrayBuffer);
    JS_FreeValue(ctx, bufferNamespace);
    JS_FreeValue(ctx, arrayBuffer);
    return result;
}

// --- Server model ---------------------------------------------------------

struct RouteDef {
    std::string method;
    std::string path;
    std::vector<std::string> paramNames; // :name tokens, for named-param lookup.
    JSValue handler = JS_UNDEFINED;       // owned (dup'd); freed on close/finalize.
    bool isStatic = false;                // static file mount rather than a JS handler.
    bool isWebSocket = false;
    std::string staticRoot;               // filesystem root for a static mount.
    std::string staticMount;              // URL prefix for a static mount.
    JSValue wsOnOpen = JS_UNDEFINED;
    JSValue wsOnMessage = JS_UNDEFINED;
    JSValue wsOnClose = JS_UNDEFINED;
    std::size_t wsMaxMessageBytes = kDefaultMaxBodyBytes;
    std::size_t wsMaxBufferedBytes = kDefaultMaxBodyBytes * 4u;
};

struct WsConnection;

struct ServerHandle {
    JSContext* ctx = nullptr;
    wl2::Runtime* runtime = nullptr;
    std::string host = "127.0.0.1";
    uint16_t port = 0;
    std::size_t maxBodyBytes = kDefaultMaxBodyBytes;
    std::vector<RouteDef> routes;
    std::unique_ptr<http_server_t> server;
    bool listening = false;
    bool closed = false;
    bool aliveHeld = false; // an outstanding async op keeping the loop alive while listening.
    // HTTPS: set when the `https` option is provided.
    bool tls = false;
    std::string certPath;
    std::string keyPath;
    std::string keyPassword;
    std::mutex wsMutex;
    std::map<restinio::connection_id_t, std::shared_ptr<WsConnection>> websockets;
#if WL2_HTTP_TLS
    std::unique_ptr<https_server_t> tlsServer;
#endif
};

struct WsConnection {
    std::weak_ptr<ServerHandle> server;
    ws_ns::ws_handle_t ws;
    restinio::connection_id_t id = 0;
    std::string remote;
    std::size_t maxBufferedBytes = kDefaultMaxBodyBytes * 4u;
    std::size_t bufferedBytes = 0;
    bool closed = false;
    std::mutex mutex;
};

// Open/close whichever concrete server (plaintext or TLS) a handle owns. These
// run on the io thread. open_sync throws on failure (e.g. bind error).
void open_server(ServerHandle& h) {
#if WL2_HTTP_TLS
    if (h.tlsServer) {
        h.tlsServer->open_sync();
        return;
    }
#endif
    h.server->open_sync();
}

void close_server(ServerHandle& h) {
    std::vector<std::shared_ptr<WsConnection>> conns;
    {
        std::lock_guard<std::mutex> lock(h.wsMutex);
        for (auto& [id, conn] : h.websockets) {
            (void)id;
            conns.push_back(conn);
        }
        h.websockets.clear();
    }
    for (auto& conn : conns) {
        try {
            ws_ns::ws_handle_t ws;
            {
                std::lock_guard<std::mutex> lock(conn->mutex);
                conn->closed = true;
                ws = conn->ws;
            }
            if (ws) {
                ws->shutdown();
            }
        } catch (...) {
        }
    }
#if WL2_HTTP_TLS
    if (h.tlsServer) {
        h.tlsServer->close_sync();
        return;
    }
#endif
    if (h.server) {
        h.server->close_sync();
    }
}

// Data copied off a request on the io thread, consumed on the JS thread.
struct Incoming {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::string remote;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> params;
    JSValue handler = JS_UNDEFINED; // borrowed from ServerHandle.
    bool isStatic = false;
    bool acceptsGzip = false;
    std::string staticRoot;
    std::string staticMount;
    // Parsed multipart/form-data parts (name, filename, contentType, data).
    struct FilePart {
        std::string name;
        std::string filename;
        std::string contentType;
        std::string data;
    };
    std::vector<FilePart> files;
};

// One in-flight request awaiting its JS-produced response.
struct Pending {
    restinio::request_handle_t req;
    std::shared_ptr<ServerHandle> handle;
    bool acceptsGzip = false;
};

JSClassID http_server_class_id = 0;
JSClassID ws_connection_class_id = 0;

struct ServerBox {
    std::shared_ptr<ServerHandle> handle;
};

struct WsBox {
    std::shared_ptr<WsConnection> conn;
};

void free_routes(JSContext* ctx, ServerHandle& h) {
    for (RouteDef& r : h.routes) {
        JS_FreeValue(ctx, r.handler);
        r.handler = JS_UNDEFINED;
        JS_FreeValue(ctx, r.wsOnOpen);
        JS_FreeValue(ctx, r.wsOnMessage);
        JS_FreeValue(ctx, r.wsOnClose);
        r.wsOnOpen = JS_UNDEFINED;
        r.wsOnMessage = JS_UNDEFINED;
        r.wsOnClose = JS_UNDEFINED;
    }
    h.routes.clear();
}

void http_server_finalizer(JSRuntime*, JSValue val) {
    auto* box = static_cast<ServerBox*>(JS_GetOpaque(val, http_server_class_id));
    delete box;
}

std::shared_ptr<ServerHandle> get_server(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<ServerBox*>(JS_GetOpaque2(ctx, value, http_server_class_id));
    return box ? box->handle : nullptr;
}

void ws_connection_finalizer(JSRuntime*, JSValue val) {
    auto* box = static_cast<WsBox*>(JS_GetOpaque(val, ws_connection_class_id));
    delete box;
}

std::shared_ptr<WsConnection> get_ws_connection(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<WsBox*>(JS_GetOpaque2(ctx, value, ws_connection_class_id));
    return box ? box->conn : nullptr;
}

JSValue make_ws_connection_object(JSContext* ctx, const std::shared_ptr<WsConnection>& conn);

// --- Response extraction / sending ----------------------------------------

struct Outgoing {
    uint16_t status = 200;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool clientAcceptsGzip = false; // whether gzip may be applied (client opted in).
};

// gzip-compress into out. Returns true on success (out holds gzip-framed data).
bool gzip_compress(const std::string& in, std::string& out) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    char buffer[16384];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buffer);
        zs.avail_out = sizeof(buffer);
        ret = deflate(&zs, Z_FINISH);
        out.append(buffer, sizeof(buffer) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);
    return ret == Z_STREAM_END;
}

bool is_compressible_type(const std::string& contentType) {
    return contentType.rfind("text/", 0) == 0 || contentType.find("json") != std::string::npos
        || contentType.find("javascript") != std::string::npos || contentType.find("svg") != std::string::npos
        || contentType.find("xml") != std::string::npos;
}

const char* default_reason(uint16_t status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "";
    }
}

// Interpret a JS handler return value as a response.
void extract_response(JSContext* ctx, JSValueConst value, Outgoing& out) {
    if (JS_IsString(value)) {
        extract_bytes(ctx, value, out.body);
        return;
    }
    if (!JS_IsObject(value)) {
        // Coerce anything else to a string body.
        const char* s = JS_ToCString(ctx, value);
        if (s) {
            out.body = s;
            JS_FreeCString(ctx, s);
        }
        return;
    }
    int64_t status = 200;
    if (get_int_prop(ctx, value, "status", status) && status >= 100 && status <= 599) {
        out.status = static_cast<uint16_t>(status);
    }
    JSValue headers = JS_GetPropertyStr(ctx, value, "headers");
    if (JS_IsObject(headers)) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &count, headers, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < count; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                JSValue hv = JS_GetProperty(ctx, headers, props[i].atom);
                const char* val = JS_ToCString(ctx, hv);
                if (key && val) {
                    out.headers.emplace_back(key, val);
                }
                if (key) {
                    JS_FreeCString(ctx, key);
                }
                if (val) {
                    JS_FreeCString(ctx, val);
                }
                JS_FreeValue(ctx, hv);
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }
    JS_FreeValue(ctx, headers);

    JSValue body = JS_GetPropertyStr(ctx, value, "body");
    if (!JS_IsUndefined(body) && !JS_IsNull(body)) {
        extract_bytes(ctx, body, out.body);
    }
    JS_FreeValue(ctx, body);
}

bool iequals(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char ca = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i];
        char cb = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

// Runs on the io thread: build and send the RESTinio response.
void send_response(const restinio::request_handle_t& req, const Outgoing& out) {
    try {
        // Decide gzip based on the effective content-type before building.
        std::string contentType;
        for (const auto& [k, v] : out.headers) {
            if (iequals(k, "content-type")) {
                contentType = v;
            }
        }
        if (contentType.empty()) {
            contentType = "text/plain; charset=utf-8";
        }
        std::string body = out.body;
        bool gzipped = false;
        if (out.clientAcceptsGzip && body.size() >= 64 && is_compressible_type(contentType)) {
            std::string compressed;
            if (gzip_compress(body, compressed) && compressed.size() < body.size()) {
                body.swap(compressed);
                gzipped = true;
            }
        }

        std::string reason = out.reason.empty() ? default_reason(out.status) : out.reason;
        auto resp = req->create_response(
            restinio::http_status_line_t{restinio::http_status_code_t{out.status}, std::move(reason)});
        bool haveContentType = false;
        for (const auto& [k, v] : out.headers) {
            resp.append_header(k, v);
            if (iequals(k, "content-type")) {
                haveContentType = true;
            }
        }
        if (!haveContentType) {
            resp.append_header(restinio::http_field::content_type, "text/plain; charset=utf-8");
        }
        if (gzipped) {
            resp.append_header(restinio::http_field::content_encoding, "gzip");
        }
        resp.set_body(body);
        resp.done();
    } catch (...) {
        // Connection already gone; nothing to do.
    }
}

void deliver(const std::shared_ptr<ServerHandle>& handle, const restinio::request_handle_t& req, Outgoing out) {
    asio_ns::post(ensure_io(), [req, out = std::move(out)] { send_response(req, out); });
    handle->runtime->async().endOperation();
}

ws_ns::opcode_t ws_opcode_from_string(const std::string& opcode, ws_ns::opcode_t fallback) {
    if (opcode == "text") return ws_ns::opcode_t::text_frame;
    if (opcode == "binary") return ws_ns::opcode_t::binary_frame;
    if (opcode == "ping") return ws_ns::opcode_t::ping_frame;
    if (opcode == "pong") return ws_ns::opcode_t::pong_frame;
    if (opcode == "close") return ws_ns::opcode_t::connection_close_frame;
    return fallback;
}

std::string ws_opcode_name(ws_ns::opcode_t opcode) {
    switch (opcode) {
        case ws_ns::opcode_t::text_frame: return "text";
        case ws_ns::opcode_t::binary_frame: return "binary";
        case ws_ns::opcode_t::ping_frame: return "ping";
        case ws_ns::opcode_t::pong_frame: return "pong";
        case ws_ns::opcode_t::connection_close_frame: return "close";
        case ws_ns::opcode_t::continuation_frame: return "continuation";
        default: return "unknown";
    }
}

std::string ws_close_payload(uint16_t code, const std::string& reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xff));
    payload.push_back(static_cast<char>(code & 0xff));
    payload += reason;
    return payload;
}

void ws_remove_connection(const std::shared_ptr<ServerHandle>& handle, restinio::connection_id_t id) {
    std::lock_guard<std::mutex> lock(handle->wsMutex);
    handle->websockets.erase(id);
}

void ws_send_on_io(const std::shared_ptr<WsConnection>& conn, std::string payload, ws_ns::opcode_t opcode) {
    std::size_t size = payload.size();
    try {
        conn->ws->send_message(ws_ns::final_frame, opcode, restinio::writable_item_t{std::move(payload)},
            [conn, size](const asio_ns::error_code&) {
                std::lock_guard<std::mutex> lock(conn->mutex);
                conn->bufferedBytes = conn->bufferedBytes > size ? conn->bufferedBytes - size : 0;
            });
    } catch (...) {
        std::lock_guard<std::mutex> lock(conn->mutex);
        conn->bufferedBytes = conn->bufferedBytes > size ? conn->bufferedBytes - size : 0;
        conn->closed = true;
    }
}

JSValue ws_connection_send(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto conn = get_ws_connection(ctx, thisVal);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "send() called on a non-WebSocket connection");
    }
    if (argc < 1) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "ws.send", "send(data, opcode?) requires data"));
    }
    std::string payload;
    if (!extract_bytes(ctx, argv[0], payload)) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "ws.send", "data must be a string or buffer"));
    }
    ws_ns::opcode_t opcode = JS_IsString(argv[0]) ? ws_ns::opcode_t::text_frame : ws_ns::opcode_t::binary_frame;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) {
            opcode = ws_opcode_from_string(s, opcode);
            JS_FreeCString(ctx, s);
        }
    }
    {
        std::lock_guard<std::mutex> lock(conn->mutex);
        if (conn->closed || !conn->ws) {
            return JS_Throw(ctx, throw_http_error(ctx, "http_closed", "ws.send", "websocket is closed"));
        }
        if (conn->bufferedBytes + payload.size() > conn->maxBufferedBytes) {
            conn->closed = true;
            auto closePayload = ws_close_payload(1009, "buffer limit exceeded");
            asio_ns::post(ensure_io(), [conn, closePayload = std::move(closePayload)]() mutable {
                try {
                    conn->ws->send_message(ws_ns::final_frame, ws_ns::opcode_t::connection_close_frame,
                        restinio::writable_item_t{std::move(closePayload)});
                } catch (...) {
                }
            });
            return JS_NewBool(ctx, false);
        }
        conn->bufferedBytes += payload.size();
    }
    asio_ns::post(ensure_io(), [conn, payload = std::move(payload), opcode]() mutable {
        ws_send_on_io(conn, std::move(payload), opcode);
    });
    return JS_NewBool(ctx, true);
}

JSValue ws_connection_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto conn = get_ws_connection(ctx, thisVal);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "close() called on a non-WebSocket connection");
    }
    int64_t code = 1000;
    if (argc > 0) {
        JS_ToInt64(ctx, &code, argv[0]);
    }
    if (code < 1000 || code > 4999) {
        code = 1000;
    }
    std::string reason;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) {
            reason = s;
            JS_FreeCString(ctx, s);
        }
    }
    {
        std::lock_guard<std::mutex> lock(conn->mutex);
        if (conn->closed || !conn->ws) {
            return JS_UNDEFINED;
        }
        conn->closed = true;
    }
    std::string payload = ws_close_payload(static_cast<uint16_t>(code), reason);
    asio_ns::post(ensure_io(), [conn, payload = std::move(payload)]() mutable {
        try {
            conn->ws->send_message(ws_ns::final_frame, ws_ns::opcode_t::connection_close_frame,
                restinio::writable_item_t{std::move(payload)});
        } catch (...) {
        }
    });
    return JS_UNDEFINED;
}

JSValue make_ws_connection_object(JSContext* ctx, const std::shared_ptr<WsConnection>& conn) {
    JSValue obj = JS_NewObjectClass(ctx, ws_connection_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new WsBox{conn});
    JS_SetPropertyStr(ctx, obj, "remoteAddr", JS_NewString(ctx, conn->remote.c_str()));
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt64(ctx, static_cast<int64_t>(conn->id)));
    std::size_t buffered = 0;
    {
        std::lock_guard<std::mutex> lock(conn->mutex);
        buffered = conn->bufferedBytes;
    }
    JS_SetPropertyStr(ctx, obj, "bufferedAmount", JS_NewInt64(ctx, static_cast<int64_t>(buffered)));
    return obj;
}

JSValue ws_message_text_cb(JSContext* ctx, JSValueConst, int argc, JSValueConst*, int, JSValue* data) {
    (void)argc;
    return JS_DupValue(ctx, data[0]);
}

// --- Static file serving --------------------------------------------------

// Percent-decode a URL path segment ('+' is left as-is; it is a space only in
// query strings, not in paths).
std::string percent_decode(const std::string& s) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(c | 0x20);
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex(s[i + 1]);
            int l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>(h * 16 + l));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

const char* mime_type(const std::string& path) {
    std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "json" || ext == "map") return "application/json; charset=utf-8";
    if (ext == "txt") return "text/plain; charset=utf-8";
    if (ext == "xml") return "application/xml; charset=utf-8";
    if (ext == "csv") return "text/csv; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "wasm") return "application/wasm";
    if (ext == "pdf") return "application/pdf";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf") return "font/ttf";
    return "application/octet-stream";
}

// Serve a file for a static mount. Runs on the JS thread so the runtime's
// filesystem-read policy can be consulted; small assets only (read into memory).
void serve_static(const std::shared_ptr<ServerHandle>& handle, const restinio::request_handle_t& req,
    const std::shared_ptr<Incoming>& in) {
    Outgoing out;
    out.clientAcceptsGzip = in->acceptsGzip;
    std::string sub = in->path.size() >= in->staticMount.size() ? in->path.substr(in->staticMount.size()) : "";
    if (!sub.empty() && sub.front() == '/') {
        sub.erase(0, 1);
    }
    sub = percent_decode(sub);

    // Traversal guard: reject any parent-dir segment or an absolute escape before
    // touching the filesystem.
    bool unsafe = sub.find("..") != std::string::npos || (!sub.empty() && sub.front() == '/');
    if (unsafe) {
        out.status = 403;
        out.body = "forbidden";
        deliver(handle, req, out);
        return;
    }
    if (sub.empty()) {
        sub = "index.html"; // directory index.
    }

    std::filesystem::path candidate = std::filesystem::path(in->staticRoot) / sub;
    auto resolved = handle->runtime->resolveFilesystemReadPath(candidate.string());
    if (!resolved) {
        out.status = 403;
        out.body = "forbidden";
        deliver(handle, req, out);
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*resolved, ec)) {
        out.status = 404;
        out.body = "not found";
        deliver(handle, req, out);
        return;
    }
    std::ifstream file(*resolved, std::ios::binary);
    if (!file) {
        out.status = 404;
        out.body = "not found";
        deliver(handle, req, out);
        return;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    out.status = 200;
    out.body = contents.str();
    out.headers.emplace_back("content-type", mime_type(resolved->string()));
    deliver(handle, req, out);
}

// --- JS-thread request dispatch -------------------------------------------

JSValue build_request_object(JSContext* ctx, const Incoming& in) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "method", JS_NewString(ctx, in.method.c_str()));
    JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, in.path.c_str()));
    std::string url = in.query.empty() ? in.path : in.path + "?" + in.query;
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url.c_str()));
    JS_SetPropertyStr(ctx, obj, "query", JS_NewString(ctx, in.query.c_str()));
    JS_SetPropertyStr(ctx, obj, "remoteAddr", JS_NewString(ctx, in.remote.c_str()));

    JSValue headers = JS_NewObject(ctx);
    for (const auto& [k, v] : in.headers) {
        JS_SetPropertyStr(ctx, headers, k.c_str(), JS_NewString(ctx, v.c_str()));
    }
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    JSValue params = JS_NewObject(ctx);
    for (const auto& [k, v] : in.params) {
        JS_SetPropertyStr(ctx, params, k.c_str(), JS_NewString(ctx, v.c_str()));
    }
    JS_SetPropertyStr(ctx, obj, "params", params);

    // Cookies: parse the Cookie header ("a=1; b=2") into an object.
    JSValue cookies = JS_NewObject(ctx);
    for (const auto& [k, v] : in.headers) {
        if (!iequals(k, "cookie")) {
            continue;
        }
        std::size_t start = 0;
        while (start < v.size()) {
            std::size_t semi = v.find(';', start);
            std::string pair = v.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
            std::size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::size_t ks = pair.find_first_not_of(" \t");
                std::string name = ks == std::string::npos ? std::string{} : pair.substr(ks, eq - ks);
                std::string value = pair.substr(eq + 1);
                if (!name.empty()) {
                    JS_SetPropertyStr(ctx, cookies, name.c_str(), JS_NewString(ctx, value.c_str()));
                }
            }
            if (semi == std::string::npos) {
                break;
            }
            start = semi + 1;
        }
    }
    JS_SetPropertyStr(ctx, obj, "cookies", cookies);

    // Multipart files (populated when the body is multipart/form-data).
    JSValue files = JS_NewArray(ctx);
    uint32_t fi = 0;
    for (const auto& part : in.files) {
        JSValue f = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, f, "name", JS_NewString(ctx, part.name.c_str()));
        JS_SetPropertyStr(ctx, f, "filename", JS_NewString(ctx, part.filename.c_str()));
        JS_SetPropertyStr(ctx, f, "contentType", JS_NewString(ctx, part.contentType.c_str()));
        JS_SetPropertyStr(ctx, f, "data", make_wl2_buffer(ctx, part.data.data(), part.data.size()));
        JS_SetPropertyUint32(ctx, files, fi++, f);
    }
    JS_SetPropertyStr(ctx, obj, "files", files);

    JS_SetPropertyStr(ctx, obj, "body", make_wl2_buffer(ctx, in.body.data(), in.body.size()));
    return obj;
}

void respond_from_value(JSContext* ctx, Pending* pr, JSValueConst value) {
    Outgoing out;
    extract_response(ctx, value, out);
    out.clientAcceptsGzip = pr->acceptsGzip;
    auto handle = pr->handle;
    auto req = pr->req;
    delete pr;
    deliver(handle, req, std::move(out));
}

void respond_with_server_error(JSContext* ctx, Pending* pr, JSValueConst error) {
    Outgoing out;
    out.status = 500;
    std::string message = "handler failed";
    if (JS_IsObject(error)) {
        JSValue m = JS_GetPropertyStr(ctx, error, "message");
        const char* s = JS_ToCString(ctx, m);
        if (s) {
            message = s;
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, m);
    }
    out.body = message;
    auto handle = pr->handle;
    auto req = pr->req;
    delete pr;
    deliver(handle, req, std::move(out));
}

// then()/catch() callback: data[0] holds the Pending*, magic selects done/fail.
JSValue settlement_cb(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic, JSValue* data) {
    int64_t ptr = 0;
    JS_ToInt64(ctx, &ptr, data[0]);
    auto* pr = reinterpret_cast<Pending*>(static_cast<intptr_t>(ptr));
    JSValue v = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (magic == 0) {
        respond_from_value(ctx, pr, v);
    } else {
        respond_with_server_error(ctx, pr, v);
    }
    return JS_UNDEFINED;
}

// Wrap a handler return value in Promise.resolve(v).then(done, fail), so sync
// values and Promises are handled uniformly. Consumes ownership of `pr`.
void attach_settlement(JSContext* ctx, Pending* pr, JSValue result) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue promiseCtor = JS_GetPropertyStr(ctx, global, "Promise");
    JSValue resolveFn = JS_GetPropertyStr(ctx, promiseCtor, "resolve");
    JSValue args[] = {result};
    JSValue promise = JS_Call(ctx, resolveFn, promiseCtor, 1, args);
    JS_FreeValue(ctx, resolveFn);

    JSValue data = JS_NewInt64(ctx, static_cast<int64_t>(reinterpret_cast<intptr_t>(pr)));
    JSValue onDone = JS_NewCFunctionData(ctx, settlement_cb, 1, 0, 1, &data);
    JSValue onFail = JS_NewCFunctionData(ctx, settlement_cb, 1, 1, 1, &data);
    JS_FreeValue(ctx, data);

    JSValue thenFn = JS_GetPropertyStr(ctx, promise, "then");
    JSValue thenArgs[] = {onDone, onFail};
    JSValue chained = JS_Call(ctx, thenFn, promise, 2, thenArgs);
    JS_FreeValue(ctx, chained);
    JS_FreeValue(ctx, thenFn);
    JS_FreeValue(ctx, onDone);
    JS_FreeValue(ctx, onFail);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, promiseCtor);
    JS_FreeValue(ctx, global);
}

void dispatch_on_js(const std::shared_ptr<ServerHandle>& handle, const restinio::request_handle_t& req, const std::shared_ptr<Incoming>& in) {
    JSContext* ctx = handle->ctx;
    if (handle->closed) {
        handle->runtime->async().endOperation();
        return;
    }
    if (in->isStatic) {
        serve_static(handle, req, in);
        return;
    }
    JSValue reqObj = build_request_object(ctx, *in);
    JSValue result = JS_Call(ctx, in->handler, JS_UNDEFINED, 1, &reqObj);
    JS_FreeValue(ctx, reqObj);

    auto* pr = new Pending{req, handle, in->acceptsGzip};
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        JSValue exc = JS_GetException(ctx);
        respond_with_server_error(ctx, pr, exc);
        JS_FreeValue(ctx, exc);
        return;
    }
    attach_settlement(ctx, pr, result);
    JS_FreeValue(ctx, result);
}

// --- Router construction --------------------------------------------------

std::vector<std::string> parse_param_names(const std::string& path) {
    std::vector<std::string> names;
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] == ':') {
            std::size_t j = i + 1;
            while (j < path.size() && (std::isalnum(static_cast<unsigned char>(path[j])) || path[j] == '_')) {
                ++j;
            }
            if (j > i + 1) {
                names.push_back(path.substr(i + 1, j - i - 1));
            }
            i = j;
        } else {
            ++i;
        }
    }
    return names;
}

using express_handler_t = restinio::router::express_request_handler_t;

void dispatch_ws_open(const std::shared_ptr<ServerHandle>& handle, JSValueConst callback,
    const std::shared_ptr<WsConnection>& conn) {
    if (handle->closed || !JS_IsFunction(handle->ctx, callback)) {
        handle->runtime->async().endOperation();
        return;
    }
    JSValue jsConn = make_ws_connection_object(handle->ctx, conn);
    JSValue result = JS_Call(handle->ctx, callback, JS_UNDEFINED, 1, &jsConn);
    JS_FreeValue(handle->ctx, jsConn);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(handle->ctx);
        JS_FreeValue(handle->ctx, exc);
    }
    JS_FreeValue(handle->ctx, result);
    handle->runtime->async().endOperation();
}

void dispatch_ws_close(const std::shared_ptr<ServerHandle>& handle, JSValueConst callback,
    const std::shared_ptr<WsConnection>& conn, uint16_t code, const std::string& reason) {
    if (!handle->closed && JS_IsFunction(handle->ctx, callback)) {
        JSValue jsConn = make_ws_connection_object(handle->ctx, conn);
        JSValue args[] = {
            jsConn,
            JS_NewInt32(handle->ctx, code),
            JS_NewString(handle->ctx, reason.c_str()),
        };
        JSValue result = JS_Call(handle->ctx, callback, JS_UNDEFINED, 3, args);
        JS_FreeValue(handle->ctx, args[2]);
        JS_FreeValue(handle->ctx, args[1]);
        JS_FreeValue(handle->ctx, jsConn);
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(handle->ctx);
            JS_FreeValue(handle->ctx, exc);
        }
        JS_FreeValue(handle->ctx, result);
    }
    handle->runtime->async().endOperation();
}

void dispatch_ws_message(const std::shared_ptr<ServerHandle>& handle, JSValueConst callback,
    const std::shared_ptr<WsConnection>& conn, ws_ns::opcode_t opcode, std::string payload) {
    if (handle->closed || !JS_IsFunction(handle->ctx, callback)) {
        handle->runtime->async().endOperation();
        return;
    }
    JSContext* ctx = handle->ctx;
    JSValue jsConn = make_ws_connection_object(ctx, conn);
    JSValue msg = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, msg, "opcode", JS_NewString(ctx, ws_opcode_name(opcode).c_str()));
    JS_SetPropertyStr(ctx, msg, "data", make_wl2_buffer(ctx, payload.data(), payload.size()));
    JSValue text = JS_NewStringLen(ctx, payload.data(), payload.size());
    JSValue fnData = JS_DupValue(ctx, text);
    JSValue textFn = JS_NewCFunctionData(ctx, ws_message_text_cb, 0, 0, 1, &fnData);
    JS_FreeValue(ctx, fnData);
    JS_SetPropertyStr(ctx, msg, "text", textFn);
    JS_FreeValue(ctx, text);
    JSValue args[] = {jsConn, msg};
    JSValue result = JS_Call(ctx, callback, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, msg);
    JS_FreeValue(ctx, jsConn);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);
    handle->runtime->async().endOperation();
}

void handle_ws_frame(const std::shared_ptr<ServerHandle>& handle, std::size_t routeIndex,
    const std::shared_ptr<WsConnection>& conn, ws_ns::message_handle_t msg) {
    const RouteDef& def = handle->routes[routeIndex];
    const auto opcode = msg->opcode();
    std::string payload = msg->payload();
    if (opcode == ws_ns::opcode_t::ping_frame) {
        auto pong = *msg;
        pong.set_opcode(ws_ns::opcode_t::pong_frame);
        try {
            conn->ws->send_message(pong);
        } catch (...) {
        }
        return;
    }
    if (opcode == ws_ns::opcode_t::connection_close_frame) {
        uint16_t code = 1000;
        std::string reason;
        if (payload.size() >= 2) {
            code = (static_cast<uint16_t>(static_cast<unsigned char>(payload[0])) << 8)
                | static_cast<uint16_t>(static_cast<unsigned char>(payload[1]));
            reason = payload.substr(2);
        }
        {
            std::lock_guard<std::mutex> lock(conn->mutex);
            conn->closed = true;
        }
        try {
            conn->ws->send_message(*msg);
        } catch (...) {
        }
        ws_remove_connection(handle, conn->id);
        if (JS_IsFunction(handle->ctx, def.wsOnClose)) {
            handle->runtime->async().beginOperation();
            handle->runtime->async().post([handle, cb = def.wsOnClose, conn, code, reason] {
                dispatch_ws_close(handle, cb, conn, code, reason);
            });
        }
        return;
    }
    if (opcode != ws_ns::opcode_t::text_frame && opcode != ws_ns::opcode_t::binary_frame
        && opcode != ws_ns::opcode_t::continuation_frame) {
        return;
    }
    if (payload.size() > def.wsMaxMessageBytes) {
        std::string closePayload = ws_close_payload(1009, "message too big");
        try {
            conn->ws->send_message(ws_ns::final_frame, ws_ns::opcode_t::connection_close_frame,
                restinio::writable_item_t{std::move(closePayload)});
        } catch (...) {
        }
        return;
    }
    handle->runtime->async().beginOperation();
    handle->runtime->async().post([handle, cb = def.wsOnMessage, conn, opcode, payload = std::move(payload)]() mutable {
        dispatch_ws_message(handle, cb, conn, opcode, std::move(payload));
    });
}

template <typename Traits>
restinio::request_handling_status_t upgrade_websocket(const std::shared_ptr<ServerHandle>& handle,
    std::size_t routeIndex, restinio::request_handle_t req) {
    const RouteDef& def = handle->routes[routeIndex];
    if (restinio::http_connection_header_t::upgrade != req->header().connection()) {
        return restinio::request_rejected();
    }
    auto conn = std::make_shared<WsConnection>();
    conn->server = handle;
    conn->maxBufferedBytes = def.wsMaxBufferedBytes;
    try {
        conn->remote = req->remote_endpoint().address().to_string();
    } catch (...) {
    }
    auto wsh = ws_ns::upgrade<Traits>(*req, ws_ns::activation_t::immediate,
        [handle, routeIndex, conn](auto, auto msg) {
            handle_ws_frame(handle, routeIndex, conn, std::move(msg));
        });
    conn->ws = wsh;
    conn->id = wsh->connection_id();
    {
        std::lock_guard<std::mutex> lock(handle->wsMutex);
        handle->websockets.emplace(conn->id, conn);
    }
    if (JS_IsFunction(handle->ctx, def.wsOnOpen)) {
        handle->runtime->async().beginOperation();
        handle->runtime->async().post([handle, cb = def.wsOnOpen, conn] {
            dispatch_ws_open(handle, cb, conn);
        });
    }
    return restinio::request_accepted();
}

express_handler_t make_route_handler(const std::shared_ptr<ServerHandle>& handle, std::size_t routeIndex) {
    return [handle, routeIndex](restinio::request_handle_t req, restinio::router::route_params_t params)
               -> restinio::request_handling_status_t {
        const RouteDef& def = handle->routes[routeIndex];

        if (def.isWebSocket) {
#if WL2_HTTP_TLS
            if (handle->tls) {
                return upgrade_websocket<https_traits_t>(handle, routeIndex, req);
            }
#endif
            return upgrade_websocket<http_traits_t>(handle, routeIndex, req);
        }

        // Body cap: reject oversize requests without invoking JS.
        if (req->body().size() > handle->maxBodyBytes) {
            Outgoing out;
            out.status = 413;
            out.body = "payload too large";
            send_response(req, out);
            return restinio::request_accepted();
        }

        auto in = std::make_shared<Incoming>();
        in->method = def.method;
        in->handler = def.handler;
        in->isStatic = def.isStatic;
        in->staticRoot = def.staticRoot;
        in->staticMount = def.staticMount;

        std::string target{req->header().request_target()};
        std::size_t q = target.find('?');
        if (q == std::string::npos) {
            in->path = std::move(target);
        } else {
            in->path = target.substr(0, q);
            in->query = target.substr(q + 1);
        }
        in->body = req->body();
        try {
            in->remote = req->remote_endpoint().address().to_string();
        } catch (...) {
        }
        req->header().for_each_field([&](const auto& field) {
            in->headers.emplace_back(std::string(field.name()), std::string(field.value()));
        });
        for (const auto& [k, v] : in->headers) {
            if (iequals(k, "accept-encoding") && v.find("gzip") != std::string::npos) {
                in->acceptsGzip = true;
                break;
            }
        }
        for (const std::string& name : def.paramNames) {
            in->params.emplace_back(name, std::string(params[name]));
        }

        // Parse multipart/form-data into file parts (best-effort, io thread).
        for (const auto& [k, v] : in->headers) {
            if (!iequals(k, "content-type") || v.find("multipart/form-data") == std::string::npos) {
                continue;
            }
            namespace mp = restinio::multipart_body;
            namespace fu = restinio::file_upload;
            (void)mp::enumerate_parts(*req, [&in](mp::parsed_part_t part) {
                auto analyzed = fu::analyze_part(std::move(part));
                if (analyzed) {
                    Incoming::FilePart fp;
                    fp.name = analyzed->name;
                    if (analyzed->filename) {
                        fp.filename = *analyzed->filename;
                    } else if (analyzed->filename_star) {
                        fp.filename = *analyzed->filename_star;
                    }
                    if (auto ct = analyzed->fields.opt_value_of(restinio::http_field::content_type)) {
                        fp.contentType.assign(ct->data(), ct->size());
                    }
                    fp.data.assign(analyzed->body.data(), analyzed->body.size());
                    in->files.push_back(std::move(fp));
                }
                return mp::handling_result_t::continue_enumeration;
            });
            break;
        }

        handle->runtime->async().beginOperation();
        handle->runtime->async().post([handle, req, in] { dispatch_on_js(handle, req, in); });
        return restinio::request_accepted();
    };
}

void register_route(router_t& router, const std::string& method, const std::string& path, express_handler_t handler) {
    restinio::http_method_id_t id = restinio::http_method_get();
    if (method == "POST") {
        id = restinio::http_method_post();
    } else if (method == "PUT") {
        id = restinio::http_method_put();
    } else if (method == "DELETE") {
        id = restinio::http_method_delete();
    } else if (method == "PATCH") {
        id = restinio::http_method_patch();
    } else if (method == "HEAD") {
        id = restinio::http_method_head();
    } else if (method == "OPTIONS") {
        id = restinio::http_method_options();
    }
    router.add_handler(id, path, std::move(handler));
}

// --- JS methods -----------------------------------------------------------

JSValue http_server_route(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto handle = get_server(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "route() called on a non-server");
    }
    if (handle->listening) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "route", "routes must be added before listen()"));
    }
    if (argc < 3 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "route", "route(method, path, handler) requires a method, path, and function"));
    }
    std::string method;
    std::string path;
    {
        const char* m = JS_ToCString(ctx, argv[0]);
        const char* p = JS_ToCString(ctx, argv[1]);
        if (m) {
            method = m;
            JS_FreeCString(ctx, m);
        }
        if (p) {
            path = p;
            JS_FreeCString(ctx, p);
        }
    }
    for (char& c : method) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    RouteDef def;
    def.method = method;
    def.path = path;
    def.paramNames = parse_param_names(path);
    def.handler = JS_DupValue(ctx, argv[2]);
    handle->routes.push_back(std::move(def));
    return JS_DupValue(ctx, thisVal); // chainable
}

JSValue http_server_static(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto handle = get_server(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "static() called on a non-server");
    }
    if (handle->listening) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "static", "static mounts must be added before listen()"));
    }
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "static", "static(mount, root) requires a mount path and a root directory"));
    }
    std::string mount;
    std::string root;
    {
        const char* m = JS_ToCString(ctx, argv[0]);
        const char* r = JS_ToCString(ctx, argv[1]);
        if (m) {
            mount = m;
            JS_FreeCString(ctx, m);
        }
        if (r) {
            root = r;
            JS_FreeCString(ctx, r);
        }
    }
    if (mount.empty() || mount.front() != '/') {
        mount.insert(mount.begin(), '/');
    }
    while (mount.size() > 1 && mount.back() == '/') {
        mount.pop_back();
    }
    RouteDef def;
    def.method = "GET";
    def.isStatic = true;
    def.staticMount = mount;
    def.staticRoot = root;
    // RESTinio express wildcard is a named regex group, not a bare '*'.
    def.path = mount + "/:wl2path(.*)"; // matches everything under the mount.
    handle->routes.push_back(std::move(def));
    return JS_DupValue(ctx, thisVal); // chainable
}

JSValue http_server_ws(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto handle = get_server(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "ws() called on a non-server");
    }
    if (handle->listening) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "ws", "websocket routes must be added before listen()"));
    }
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1])) {
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "ws", "ws(path, handlers) requires a path and handler object"));
    }
    std::string path;
    const char* p = JS_ToCString(ctx, argv[0]);
    if (p) {
        path = p;
        JS_FreeCString(ctx, p);
    }
    if (path.empty() || path.front() != '/') {
        path.insert(path.begin(), '/');
    }

    JSValue onOpen = JS_GetPropertyStr(ctx, argv[1], "onOpen");
    JSValue onMessage = JS_GetPropertyStr(ctx, argv[1], "onMessage");
    JSValue onClose = JS_GetPropertyStr(ctx, argv[1], "onClose");
    if (!JS_IsFunction(ctx, onMessage)) {
        JS_FreeValue(ctx, onOpen);
        JS_FreeValue(ctx, onMessage);
        JS_FreeValue(ctx, onClose);
        return JS_Throw(ctx, throw_http_error(ctx, "http_invalid_argument", "ws", "handlers.onMessage must be a function"));
    }

    RouteDef def;
    def.method = "GET";
    def.path = path;
    def.isWebSocket = true;
    def.wsOnOpen = JS_IsFunction(ctx, onOpen) ? JS_DupValue(ctx, onOpen) : JS_UNDEFINED;
    def.wsOnMessage = JS_DupValue(ctx, onMessage);
    def.wsOnClose = JS_IsFunction(ctx, onClose) ? JS_DupValue(ctx, onClose) : JS_UNDEFINED;
    int64_t maxMessage = 0;
    if (get_int_prop(ctx, argv[1], "maxMessageBytes", maxMessage) && maxMessage > 0) {
        def.wsMaxMessageBytes = static_cast<std::size_t>(maxMessage);
    }
    int64_t maxBuffered = 0;
    if (get_int_prop(ctx, argv[1], "maxBufferedBytes", maxBuffered) && maxBuffered > 0) {
        def.wsMaxBufferedBytes = static_cast<std::size_t>(maxBuffered);
    }

    JS_FreeValue(ctx, onOpen);
    JS_FreeValue(ctx, onMessage);
    JS_FreeValue(ctx, onClose);
    handle->routes.push_back(std::move(def));
    return JS_DupValue(ctx, thisVal);
}

JSValue http_server_listen(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto handle = get_server(ctx, thisVal);
    if (!runtime || !handle) {
        return JS_ThrowTypeError(ctx, "listen() called on a non-server");
    }
    if (handle->closed) {
        return rejected_promise(ctx, runtime, throw_http_error(ctx, "http_closed", "listen", "server is closed"));
    }
    if (handle->listening) {
        return rejected_promise(ctx, runtime, throw_http_error(ctx, "http_already_listening", "listen", "server is already listening"));
    }
    if (auto ok = runtime->authorizeNetworkListen(handle->host, handle->port); !ok) {
        return rejected_promise(ctx, runtime, throw_http_error(ctx, "http_permission_denied", "listen", ok.error().message()));
    }

    // Build the router from the registered routes.
    auto router = std::make_unique<router_t>();
    for (std::size_t i = 0; i < handle->routes.size(); ++i) {
        register_route(*router, handle->routes[i].method, handle->routes[i].path, make_route_handler(handle, i));
    }
    // Unmatched routes get a 404 (RESTinio's default for a rejected request is a
    // 501). This runs on the io thread; no JS handler is involved.
    router->non_matched_request_handler([](restinio::request_handle_t req) {
        Outgoing out;
        out.status = 404;
        out.body = "not found";
        send_response(req, out);
        return restinio::request_accepted();
    });

    asio_ns::io_context& io = ensure_io();
    try {
        if (handle->tls) {
#if WL2_HTTP_TLS
            namespace ssl = asio_ns::ssl;
            ssl::context tls_ctx{ssl::context::tls_server};
            tls_ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2
                | ssl::context::no_sslv3 | ssl::context::single_dh_use);
            if (!handle->keyPassword.empty()) {
                std::string pw = handle->keyPassword;
                tls_ctx.set_password_callback(
                    [pw](std::size_t, ssl::context::password_purpose) { return pw; });
            }
            tls_ctx.use_certificate_chain_file(handle->certPath);
            tls_ctx.use_private_key_file(handle->keyPath, ssl::context::pem);
            handle->tlsServer = std::make_unique<https_server_t>(
                restinio::external_io_context(io),
                restinio::server_settings_t<https_traits_t>{}
                    .port(handle->port)
                    .address(handle->host)
                    .request_handler(std::move(router))
                    .tls_context(std::move(tls_ctx)));
#else
            return rejected_promise(ctx, runtime,
                throw_http_error(ctx, "http_tls_failed", "listen", "wl2:http was built without TLS support"));
#endif
        } else {
            handle->server = std::make_unique<http_server_t>(
                restinio::external_io_context(io),
                restinio::server_settings_t<http_traits_t>{}
                    .port(handle->port)
                    .address(handle->host)
                    .request_handler(std::move(router)));
        }
    } catch (const std::exception& e) {
        const char* code = handle->tls ? "http_tls_failed" : "http_listen_failed";
        return rejected_promise(ctx, runtime, throw_http_error(ctx, code, "listen", e.what()));
    }

    std::shared_ptr<Promise> promise;
    JSValue jsPromise = make_promise(ctx, runtime, promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }
    std::string host = handle->host;
    uint16_t port = handle->port;

    runtime->async().beginOperation();
    asio_ns::post(io, [handle, promise, host, port] {
        try {
            open_server(*handle);
            handle->listening = true;
            promise->runtime->async().post([promise, handle, host, port] {
                // Hold one outstanding op for the server's lifetime so a listening
                // server keeps the event loop (and process) alive until close().
                promise->runtime->async().beginOperation();
                handle->aliveHeld = true;
                JSValue obj = JS_NewObject(promise->ctx);
                JS_SetPropertyStr(promise->ctx, obj, "host", JS_NewString(promise->ctx, host.c_str()));
                JS_SetPropertyStr(promise->ctx, obj, "port", JS_NewInt32(promise->ctx, port));
                settle_value(promise, obj);
                promise->runtime->async().endOperation(); // release the listen op.
            });
        } catch (const std::exception& e) {
            std::string message = e.what();
            promise->runtime->async().post([promise, message] {
                settle_error(promise, throw_http_error(promise->ctx, "http_listen_failed", "listen", message));
                promise->runtime->async().endOperation();
            });
        }
    });
    return jsPromise;
}

JSValue http_server_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto handle = get_server(ctx, thisVal);
    if (!runtime || !handle) {
        return JS_ThrowTypeError(ctx, "close() called on a non-server");
    }

    std::shared_ptr<Promise> promise;
    JSValue jsPromise = make_promise(ctx, runtime, promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }

    if (handle->closed || !handle->listening) {
        handle->closed = true;
        free_routes(ctx, *handle);
        settle_value(promise, JS_UNDEFINED);
        return jsPromise;
    }

    runtime->async().beginOperation();
    asio_ns::post(ensure_io(), [handle, promise] {
        try {
            close_server(*handle);
        } catch (...) {
        }
        handle->listening = false;
        handle->closed = true;
        promise->runtime->async().post([promise, handle] {
            if (handle->aliveHeld) {
                handle->aliveHeld = false;
                promise->runtime->async().endOperation(); // release the lifetime op.
            }
            free_routes(promise->ctx, *handle);
            settle_value(promise, JS_UNDEFINED);
            promise->runtime->async().endOperation(); // release the close op.
        });
    });
    return jsPromise;
}

// HttpServer(options?) — a constructor (used as `new HttpServer(...)`). It
// returns a class object carrying the server handle, which overrides the default
// `this` and provides the route/listen/close methods from the class prototype.
JSValue http_server_ctor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "wl2:http requires a runtime context");
    }
    auto handle = std::make_shared<ServerHandle>();
    handle->ctx = ctx;
    handle->runtime = runtime;
    if (argc > 0 && JS_IsObject(argv[0])) {
        std::string host;
        if (get_string_prop(ctx, argv[0], "host", host) && !host.empty()) {
            handle->host = host;
        }
        int64_t port = 0;
        if (get_int_prop(ctx, argv[0], "port", port) && port >= 0 && port <= 65535) {
            handle->port = static_cast<uint16_t>(port);
        }
        int64_t maxBody = 0;
        if (get_int_prop(ctx, argv[0], "maxBodyBytes", maxBody) && maxBody > 0) {
            handle->maxBodyBytes = static_cast<std::size_t>(maxBody);
        }
        JSValue https = JS_GetPropertyStr(ctx, argv[0], "https");
        if (JS_IsObject(https)) {
            handle->tls = true;
            get_string_prop(ctx, https, "cert", handle->certPath);
            get_string_prop(ctx, https, "key", handle->keyPath);
            get_string_prop(ctx, https, "keyPassword", handle->keyPassword);
        }
        JS_FreeValue(ctx, https);
    }

    JSValue obj = JS_NewObjectClass(ctx, http_server_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new ServerBox{handle});
    return obj;
}

// --- Module registration --------------------------------------------------

void register_server_class(JSContext* ctx) {
    if (http_server_class_id == 0) {
        JS_NewClassID(&http_server_class_id);
    }
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!JS_IsRegisteredClass(rt, http_server_class_id)) {
        JSClassDef def{};
        def.class_name = "HttpServer";
        def.finalizer = http_server_finalizer;
        JS_NewClass(rt, http_server_class_id, &def);
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "route", JS_NewCFunction(ctx, http_server_route, "route", 3));
    JS_SetPropertyStr(ctx, proto, "static", JS_NewCFunction(ctx, http_server_static, "static", 2));
    JS_SetPropertyStr(ctx, proto, "ws", JS_NewCFunction(ctx, http_server_ws, "ws", 2));
    JS_SetPropertyStr(ctx, proto, "listen", JS_NewCFunction(ctx, http_server_listen, "listen", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, http_server_close, "close", 0));
    JS_SetClassProto(ctx, http_server_class_id, proto);

    if (ws_connection_class_id == 0) {
        JS_NewClassID(&ws_connection_class_id);
    }
    if (!JS_IsRegisteredClass(rt, ws_connection_class_id)) {
        JSClassDef def{};
        def.class_name = "WsConnection";
        def.finalizer = ws_connection_finalizer;
        JS_NewClass(rt, ws_connection_class_id, &def);
    }
    JSValue wsProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wsProto, "send", JS_NewCFunction(ctx, ws_connection_send, "send", 2));
    JS_SetPropertyStr(ctx, wsProto, "close", JS_NewCFunction(ctx, ws_connection_close, "close", 2));
    JS_SetClassProto(ctx, ws_connection_class_id, wsProto);
}

int init_http_module(JSContext* ctx, JSModuleDef* module) {
    register_server_class(ctx);
    JS_SetModuleExport(ctx, module, "HttpServer", JS_NewCFunction2(ctx, http_server_ctor, "HttpServer", 1, JS_CFUNC_constructor, 0));
    return 0;
}

void shutdown_http_state() {
    HttpState& s = state();
    bool started;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        started = s.started;
    }
    if (!started) {
        return;
    }
    s.io.stop();
    s.work.reset();
    if (s.worker.joinable()) {
        s.worker.join();
    }
}

#endif // WL2_HAVE_QUICKJS

} // namespace

wl2::ModuleInfo wl2_restinio_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:http", wl2_restinio_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:http",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "8c49bf47-227b-487d-9e93-5f4ec3e8c054",
        .summary = "Embeddable HTTP/1.1 server backed by RESTinio and standalone Asio.",
        .api = HttpApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_restinio_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    if (auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx))) {
        runtime->async().registerShutdownHook([] { shutdown_http_state(); });
    }
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_http_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "HttpServer");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_RESTINIO_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:http";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "8c49bf47-227b-487d-9e93-5f4ec3e8c054";
    out->summary = "Embeddable HTTP/1.1 server backed by RESTinio and standalone Asio.";
    out->api = HttpApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
