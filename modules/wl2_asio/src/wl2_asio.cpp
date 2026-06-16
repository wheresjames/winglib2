// wl2:asio — minimal asynchronous TCP networking module (Phase 1: client only).
//
// All Asio socket work runs on a single module-owned worker thread driving one
// io_context. JavaScript-facing functions (connect/read/write/close) marshal the
// request onto that thread with asio::post, and completions are marshalled back
// onto the JavaScript thread through Runtime::async().post() where it is safe to
// settle the QuickJS promise. Network access is denied by default and gated by
// Runtime::authorizeNetworkConnect().
#include "wl2_asio/wl2_asio.h"

#include "wl2/runtime.h"

#include <asio.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

constexpr const char* AsioApi = R"(Exports JavaScript module wl2:asio.

Functions:
  connect({ host, port, timeoutMs }) -> Promise<TcpSocket>
  listen({ host, port }) -> Promise<TcpServer>

TcpSocket:
  read({ maxBytes, timeoutMs }) -> Promise<wl2.Buffer>   (0 bytes signals EOF)
  write(data, { timeoutMs }) -> Promise<{ bytesWritten }> (data: string|ArrayBuffer|TypedArray)
  remoteAddress() -> { host, port }
  localAddress() -> { host, port }
  close()                                                  (idempotent)

TcpServer:
  accept({ timeoutMs }) -> Promise<TcpSocket>
  address() -> { host, port }
  close()                                                  (idempotent)

Security defaults:
  Network access is denied by default. The host runtime must grant it
  (allowNetwork + networkAllowList) before connect() can succeed; denials
  surface as AsioError code "asio_permission_denied".)";

// --- Module-owned Asio runtime -------------------------------------------------

// One io_context driven by a single worker thread, created lazily on first use.
// The io_context object itself is never destroyed (it outlives every socket
// handle so finalizers can safely close fds); only its run loop is started and
// stopped. start() is restartable so sequential runtimes (e.g. `wl2 run --watch`)
// each get a live worker.
struct AsioRuntimeState {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread worker;
    bool running = false;

    void start() {
        if (running) {
            return;
        }
        io.restart();
        work.emplace(asio::make_work_guard(io));
        worker = std::thread([this] { io.run(); });
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        work.reset();
        io.stop();
        if (worker.joinable()) {
            worker.join();
        }
        running = false;
    }
};

std::mutex g_state_mutex;
std::unique_ptr<AsioRuntimeState> g_state;

asio::io_context& ensure_io() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state) {
        g_state = std::make_unique<AsioRuntimeState>();
    }
    g_state->start();
    return g_state->io;
}

void shutdown_asio_state() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state) {
        g_state->stop();
    }
}

// Native handle owned (shared) by the JavaScript TcpSocket object and by any
// in-flight operation. The socket is bound to the module io_context.
struct TcpSocketHandle {
    asio::ip::tcp::socket socket;
    // Written on the JS thread (close()) and read on the io_context thread
    // (completion handlers), so it must be atomic.
    std::atomic<bool> closed{false};
    bool reading = false;  // single in-flight read, touched only on the JS thread
    bool writing = false;  // single in-flight write, touched only on the JS thread

    explicit TcpSocketHandle(asio::io_context& io) : socket(io) {}
};

// Native handle for a listening server, shared by the JavaScript TcpServer
// object and any in-flight accept. The acceptor is bound to the module io_context.
struct TcpServerHandle {
    asio::ip::tcp::acceptor acceptor;
    // Written on the JS thread (close()) and read on the io_context thread
    // (the accept handler), so it must be atomic.
    std::atomic<bool> closed{false};
    bool accepting = false;  // single in-flight accept, touched only on the JS thread

    explicit TcpServerHandle(asio::io_context& io) : acceptor(io) {}
};

constexpr std::size_t kDefaultReadBytes = 64 * 1024;
constexpr std::size_t kMaxReadBytes = 1024 * 1024;

#if WL2_HAVE_QUICKJS

JSClassID tcp_socket_class_id = 0;

struct SocketBox {
    std::shared_ptr<TcpSocketHandle> handle;
};

void tcp_socket_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<SocketBox*>(JS_GetOpaque(val, tcp_socket_class_id));
}

std::shared_ptr<TcpSocketHandle> get_handle(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<SocketBox*>(JS_GetOpaque2(ctx, value, tcp_socket_class_id));
    return box ? box->handle : nullptr;
}

JSValue new_socket_object(JSContext* ctx, std::shared_ptr<TcpSocketHandle> handle) {
    JSValue obj = JS_NewObjectClass(ctx, tcp_socket_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new SocketBox{std::move(handle)});
    return obj;
}

JSClassID tcp_server_class_id = 0;

struct ServerBox {
    std::shared_ptr<TcpServerHandle> handle;
};

void tcp_server_finalizer(JSRuntime* rt, JSValue val) {
    (void)rt;
    delete static_cast<ServerBox*>(JS_GetOpaque(val, tcp_server_class_id));
}

std::shared_ptr<TcpServerHandle> get_server(JSContext* ctx, JSValueConst value) {
    auto* box = static_cast<ServerBox*>(JS_GetOpaque2(ctx, value, tcp_server_class_id));
    return box ? box->handle : nullptr;
}

JSValue new_server_object(JSContext* ctx, std::shared_ptr<TcpServerHandle> handle) {
    JSValue obj = JS_NewObjectClass(ctx, tcp_server_class_id);
    if (JS_IsException(obj)) {
        return obj;
    }
    JS_SetOpaque(obj, new ServerBox{std::move(handle)});
    return obj;
}

// --- Error contract ------------------------------------------------------------

enum class AsioErr {
    PermissionDenied,
    InvalidArgument,
    ResolveFailed,
    ConnectFailed,
    ListenFailed,
    AcceptFailed,
    ReadFailed,
    WriteFailed,
    Timeout,
    Closed,
};

const char* code_string(AsioErr code) {
    switch (code) {
        case AsioErr::PermissionDenied: return "asio_permission_denied";
        case AsioErr::InvalidArgument: return "asio_invalid_argument";
        case AsioErr::ResolveFailed: return "asio_resolve_failed";
        case AsioErr::ConnectFailed: return "asio_connect_failed";
        case AsioErr::ListenFailed: return "asio_listen_failed";
        case AsioErr::AcceptFailed: return "asio_accept_failed";
        case AsioErr::ReadFailed: return "asio_read_failed";
        case AsioErr::WriteFailed: return "asio_write_failed";
        case AsioErr::Timeout: return "asio_timeout";
        case AsioErr::Closed: return "asio_closed";
    }
    return "asio_error";
}

// Builds an AsioError following the shared module error shape. Runs on the JS
// thread. host/port/cause are optional (empty host / port 0 / empty cause omit).
JSValue make_error(JSContext* ctx, AsioErr code, const char* operation,
    const std::string& host, uint16_t port, const std::string& cause) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "AsioError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_asio"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code_string(code)));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    const std::string message = cause.empty()
        ? std::string(operation) + " failed (" + code_string(code) + ")"
        : std::string(operation) + " failed: " + cause;
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    if (!host.empty()) {
        JS_SetPropertyStr(ctx, error, "host", JS_NewString(ctx, host.c_str()));
    }
    if (port != 0) {
        JS_SetPropertyStr(ctx, error, "port", JS_NewInt32(ctx, port));
    }
    if (!cause.empty()) {
        JS_SetPropertyStr(ctx, error, "cause", JS_NewString(ctx, cause.c_str()));
    }
    return error;
}

// --- Option / value helpers ----------------------------------------------------

bool get_string_prop(JSContext* ctx, JSValueConst obj, const char* name, std::string& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    bool ok = false;
    if (!JS_IsUndefined(value) && !JS_IsNull(value)) {
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, value);
        if (text) {
            out.assign(text, len);
            JS_FreeCString(ctx, text);
            ok = true;
        }
    }
    JS_FreeValue(ctx, value);
    return ok;
}

bool get_int_prop(JSContext* ctx, JSValueConst obj, const char* name, int32_t& out) {
    JSValue value = JS_GetPropertyStr(ctx, obj, name);
    bool ok = false;
    if (JS_IsNumber(value)) {
        ok = JS_ToInt32(ctx, &out, value) == 0;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

// Extracts raw bytes from a string (UTF-8), ArrayBuffer, or TypedArray.
bool extract_bytes(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsString(value)) {
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, value);
        if (!text) {
            return false;
        }
        out.assign(text, len);
        JS_FreeCString(ctx, text);
        return true;
    }

    size_t size = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &size, value);
    if (bytes) {
        out.assign(reinterpret_cast<const char*>(bytes), size);
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));  // not an ArrayBuffer; clear and retry

    size_t offset = 0;
    size_t length = 0;
    size_t bytesPerElement = 0;
    JSValue arrayBuffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &length, &bytesPerElement);
    if (!JS_IsException(arrayBuffer)) {
        uint8_t* base = JS_GetArrayBuffer(ctx, &size, arrayBuffer);
        bool ok = false;
        if (base) {
            out.assign(reinterpret_cast<const char*>(base) + offset, length);
            ok = true;
        }
        JS_FreeValue(ctx, arrayBuffer);
        return ok;
    }
    JS_FreeValue(ctx, arrayBuffer);
    JS_FreeValue(ctx, JS_GetException(ctx));
    return false;
}

// Wraps bytes in a wl2.Buffer via the host wl2.buffer.fromArrayBuffer helper, so
// reads match the buffer convention used by wl2:fs and wl2:curl.
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

// A pending promise: resolve/reject are owned here and freed exactly once when
// the operation settles on the JS thread.
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

// Settle helpers. settle_value takes ownership of `value`; both free resolve and
// reject. Always invoked on the JS thread (directly for synchronous rejections,
// or inside Runtime::async().post() for completions arriving from the worker).
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

// Returns an already-rejected promise (used for synchronous failures such as a
// denied capability or invalid argument), so callers always get a promise back.
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

// --- connect() -----------------------------------------------------------------

struct ConnectOp {
    std::shared_ptr<Promise> promise;
    std::string host;
    uint16_t port = 0;
    long timeoutMs = 0;
    std::shared_ptr<TcpSocketHandle> handle;
    asio::ip::tcp::resolver resolver;
    asio::steady_timer timer;
    bool done = false;  // claimed only on the io_context thread

    ConnectOp(asio::io_context& io) : resolver(io), timer(io) {}
};

void connect_finish_socket(const std::shared_ptr<ConnectOp>& op) {
    auto promise = op->promise;
    auto handle = op->handle;
    promise->runtime->async().post([promise, handle] {
        settle_value(promise, new_socket_object(promise->ctx, handle));
        promise->runtime->async().endOperation();
    });
}

void connect_finish_error(const std::shared_ptr<ConnectOp>& op, AsioErr code, std::string cause) {
    auto promise = op->promise;
    std::string host = op->host;
    uint16_t port = op->port;
    promise->runtime->async().post([promise, code, cause, host, port] {
        settle_error(promise, make_error(promise->ctx, code, "connect", host, port, cause));
        promise->runtime->async().endOperation();
    });
}

JSValue asio_connect(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "runtime unavailable");
    }
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "connect(options) requires an options object");
    }

    std::string host;
    int32_t port = 0;
    if (!get_string_prop(ctx, argv[0], "host", host) || host.empty()) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "connect", "", 0, "host is required"));
    }
    if (!get_int_prop(ctx, argv[0], "port", port) || port <= 0 || port > 65535) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "connect", host, 0, "port must be 1-65535"));
    }
    int32_t timeoutMs = 0;
    get_int_prop(ctx, argv[0], "timeoutMs", timeoutMs);

    // The capability gate is applied after DNS resolution (every resolved
    // endpoint must be authorized); see the resolve handler below.
    asio::io_context& io = ensure_io();
    auto op = std::make_shared<ConnectOp>(io);
    op->host = host;
    op->port = static_cast<uint16_t>(port);
    op->timeoutMs = timeoutMs;
    op->handle = std::make_shared<TcpSocketHandle>(io);

    JSValue jsPromise = make_promise(ctx, runtime, op->promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }

    runtime->async().beginOperation();
    asio::post(io, [op] {
        op->resolver.async_resolve(op->host, std::to_string(op->port),
            [op](const asio::error_code& ec, asio::ip::tcp::resolver::results_type results) {
                if (op->done) {
                    return;
                }
                if (ec) {
                    op->done = true;
                    op->timer.cancel();
                    connect_finish_error(op, AsioErr::ResolveFailed, ec.message());
                    return;
                }
                // Resolve-then-authorize: every resolved endpoint must satisfy
                // outbound policy, denying if any fails. The originally requested
                // host is preserved in diagnostics (connect_finish_error).
                for (const auto& entry : results) {
                    const std::string address = entry.endpoint().address().to_string();
                    if (auto ok = op->promise->runtime->authorizeNetworkConnect(address, op->port); !ok) {
                        op->done = true;
                        op->timer.cancel();
                        connect_finish_error(op, AsioErr::PermissionDenied,
                            ok.error().code() + ": " + ok.error().message());
                        return;
                    }
                }
                asio::async_connect(op->handle->socket, results,
                    [op](const asio::error_code& ce, const asio::ip::tcp::endpoint&) {
                        if (op->done) {
                            return;
                        }
                        op->done = true;
                        op->timer.cancel();
                        if (ce) {
                            connect_finish_error(op, AsioErr::ConnectFailed, ce.message());
                        } else {
                            connect_finish_socket(op);
                        }
                    });
            });

        if (op->timeoutMs > 0) {
            op->timer.expires_after(std::chrono::milliseconds(op->timeoutMs));
            op->timer.async_wait([op](const asio::error_code& ec) {
                if (ec || op->done) {
                    return;
                }
                op->done = true;
                asio::error_code ignored;
                op->resolver.cancel();
                op->handle->socket.close(ignored);
                connect_finish_error(op, AsioErr::Timeout, "connect timed out");
            });
        }
    });

    return jsPromise;
}

// --- TcpSocket.read() ----------------------------------------------------------

struct ReadOp {
    std::shared_ptr<Promise> promise;
    std::shared_ptr<TcpSocketHandle> handle;
    std::vector<char> buffer;
    asio::steady_timer timer;
    long timeoutMs = 0;
    bool done = false;

    ReadOp(asio::io_context& io) : timer(io) {}
};

void read_finish_bytes(const std::shared_ptr<ReadOp>& op, std::size_t n) {
    auto promise = op->promise;
    auto handle = op->handle;
    auto buffer = std::make_shared<std::vector<char>>(std::move(op->buffer));
    promise->runtime->async().post([promise, handle, buffer, n] {
        handle->reading = false;
        settle_value(promise, make_wl2_buffer(promise->ctx, buffer->data(), n));
        promise->runtime->async().endOperation();
    });
}

void read_finish_error(const std::shared_ptr<ReadOp>& op, AsioErr code, std::string cause) {
    auto promise = op->promise;
    auto handle = op->handle;
    promise->runtime->async().post([promise, handle, code, cause] {
        handle->reading = false;
        settle_error(promise, make_error(promise->ctx, code, "read", "", 0, cause));
        promise->runtime->async().endOperation();
    });
}

JSValue tcp_socket_read(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto handle = get_handle(ctx, thisVal);
    if (!runtime || !handle) {
        return JS_ThrowTypeError(ctx, "read() called on a non-socket");
    }
    if (handle->closed) {
        return rejected_promise(ctx, runtime, make_error(ctx, AsioErr::Closed, "read", "", 0, "socket is closed"));
    }
    if (handle->reading) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "read", "", 0, "a read is already in progress"));
    }

    std::size_t maxBytes = kDefaultReadBytes;
    if (argc > 0 && JS_IsObject(argv[0])) {
        int32_t requested = 0;
        if (get_int_prop(ctx, argv[0], "maxBytes", requested) && requested > 0) {
            maxBytes = std::min(static_cast<std::size_t>(requested), kMaxReadBytes);
        }
    }
    int32_t timeoutMs = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        get_int_prop(ctx, argv[0], "timeoutMs", timeoutMs);
    }

    asio::io_context& io = ensure_io();
    auto op = std::make_shared<ReadOp>(io);
    op->handle = handle;
    op->buffer.resize(maxBytes);
    op->timeoutMs = timeoutMs;

    JSValue jsPromise = make_promise(ctx, runtime, op->promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }

    handle->reading = true;
    runtime->async().beginOperation();
    asio::post(io, [op] {
        op->handle->socket.async_read_some(asio::buffer(op->buffer),
            [op](const asio::error_code& ec, std::size_t n) {
                if (op->done) {
                    return;
                }
                op->done = true;
                op->timer.cancel();
                if (!ec) {
                    read_finish_bytes(op, n);
                } else if (ec == asio::error::eof) {
                    read_finish_bytes(op, 0);  // EOF reported as a zero-length read
                } else if (ec == asio::error::operation_aborted) {
                    read_finish_error(op, op->handle->closed ? AsioErr::Closed : AsioErr::Timeout, ec.message());
                } else {
                    read_finish_error(op, AsioErr::ReadFailed, ec.message());
                }
            });

        if (op->timeoutMs > 0) {
            op->timer.expires_after(std::chrono::milliseconds(op->timeoutMs));
            op->timer.async_wait([op](const asio::error_code& ec) {
                if (ec || op->done) {
                    return;
                }
                op->done = true;
                asio::error_code ignored;
                op->handle->socket.cancel(ignored);
                read_finish_error(op, AsioErr::Timeout, "read timed out");
            });
        }
    });

    return jsPromise;
}

// --- TcpSocket.write() ---------------------------------------------------------

struct WriteOp {
    std::shared_ptr<Promise> promise;
    std::shared_ptr<TcpSocketHandle> handle;
    std::shared_ptr<std::string> data;
    asio::steady_timer timer;
    long timeoutMs = 0;
    bool done = false;

    WriteOp(asio::io_context& io) : timer(io) {}
};

void write_finish_bytes(const std::shared_ptr<WriteOp>& op, std::size_t n) {
    auto promise = op->promise;
    auto handle = op->handle;
    promise->runtime->async().post([promise, handle, n] {
        handle->writing = false;
        JSValue result = JS_NewObject(promise->ctx);
        JS_SetPropertyStr(promise->ctx, result, "bytesWritten", JS_NewInt64(promise->ctx, static_cast<int64_t>(n)));
        settle_value(promise, result);
        promise->runtime->async().endOperation();
    });
}

void write_finish_error(const std::shared_ptr<WriteOp>& op, AsioErr code, std::string cause) {
    auto promise = op->promise;
    auto handle = op->handle;
    promise->runtime->async().post([promise, handle, code, cause] {
        handle->writing = false;
        settle_error(promise, make_error(promise->ctx, code, "write", "", 0, cause));
        promise->runtime->async().endOperation();
    });
}

JSValue tcp_socket_write(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto handle = get_handle(ctx, thisVal);
    if (!runtime || !handle) {
        return JS_ThrowTypeError(ctx, "write() called on a non-socket");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "write(data) requires data");
    }
    if (handle->closed) {
        return rejected_promise(ctx, runtime, make_error(ctx, AsioErr::Closed, "write", "", 0, "socket is closed"));
    }
    if (handle->writing) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "write", "", 0, "a write is already in progress"));
    }

    auto data = std::make_shared<std::string>();
    if (!extract_bytes(ctx, argv[0], *data)) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "write", "", 0, "data must be a string, ArrayBuffer, or TypedArray"));
    }
    int32_t timeoutMs = 0;
    if (argc > 1 && JS_IsObject(argv[1])) {
        get_int_prop(ctx, argv[1], "timeoutMs", timeoutMs);
    }

    asio::io_context& io = ensure_io();
    auto op = std::make_shared<WriteOp>(io);
    op->handle = handle;
    op->data = data;
    op->timeoutMs = timeoutMs;

    JSValue jsPromise = make_promise(ctx, runtime, op->promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }

    handle->writing = true;
    runtime->async().beginOperation();
    asio::post(io, [op] {
        asio::async_write(op->handle->socket, asio::buffer(*op->data),
            [op](const asio::error_code& ec, std::size_t n) {
                if (op->done) {
                    return;
                }
                op->done = true;
                op->timer.cancel();
                if (!ec) {
                    write_finish_bytes(op, n);
                } else if (ec == asio::error::operation_aborted) {
                    write_finish_error(op, op->handle->closed ? AsioErr::Closed : AsioErr::Timeout, ec.message());
                } else {
                    write_finish_error(op, AsioErr::WriteFailed, ec.message());
                }
            });

        if (op->timeoutMs > 0) {
            op->timer.expires_after(std::chrono::milliseconds(op->timeoutMs));
            op->timer.async_wait([op](const asio::error_code& ec) {
                if (ec || op->done) {
                    return;
                }
                op->done = true;
                asio::error_code ignored;
                op->handle->socket.cancel(ignored);
                write_finish_error(op, AsioErr::Timeout, "write timed out");
            });
        }
    });

    return jsPromise;
}

// --- TcpSocket address getters -------------------------------------------------

JSValue endpoint_to_object(JSContext* ctx, const asio::ip::tcp::endpoint& endpoint) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "host", JS_NewString(ctx, endpoint.address().to_string().c_str()));
    JS_SetPropertyStr(ctx, obj, "port", JS_NewInt32(ctx, endpoint.port()));
    return obj;
}

// Synchronous endpoint queries (getpeername / getsockname); safe to read while
// async work is outstanding because they only inspect the underlying fd.
JSValue tcp_socket_remote_address(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto handle = get_handle(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "remoteAddress() called on a non-socket");
    }
    asio::error_code ec;
    auto endpoint = handle->socket.remote_endpoint(ec);
    if (ec) {
        return JS_Throw(ctx, make_error(ctx, AsioErr::Closed, "remoteAddress", "", 0, ec.message()));
    }
    return endpoint_to_object(ctx, endpoint);
}

JSValue tcp_socket_local_address(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto handle = get_handle(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "localAddress() called on a non-socket");
    }
    asio::error_code ec;
    auto endpoint = handle->socket.local_endpoint(ec);
    if (ec) {
        return JS_Throw(ctx, make_error(ctx, AsioErr::Closed, "localAddress", "", 0, ec.message()));
    }
    return endpoint_to_object(ctx, endpoint);
}

// --- TcpSocket.close() ---------------------------------------------------------

JSValue tcp_socket_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto handle = get_handle(ctx, thisVal);
    if (!handle) {
        return JS_ThrowTypeError(ctx, "close() called on a non-socket");
    }
    if (!handle->closed) {
        handle->closed = true;
        // The socket is owned by the io_context thread; close there so any
        // in-flight read/write is cancelled and settles with asio_closed.
        asio::post(handle->socket.get_executor(), [handle] {
            asio::error_code ignored;
            handle->socket.cancel(ignored);
            handle->socket.close(ignored);
        });
    }
    return JS_UNDEFINED;
}

// --- listen() ------------------------------------------------------------------

// Resolves a bind address: accepts an IP literal directly, otherwise performs a
// (brief, synchronous) resolve so hostnames like "localhost" work for binding.
bool resolve_bind_endpoint(asio::io_context& io, const std::string& host, uint16_t port,
    asio::ip::tcp::endpoint& out, std::string& error) {
    asio::error_code ec;
    asio::ip::address address = asio::ip::make_address(host, ec);
    if (!ec) {
        out = asio::ip::tcp::endpoint(address, port);
        return true;
    }
    asio::ip::tcp::resolver resolver(io);
    auto results = resolver.resolve(host, std::to_string(port), ec);
    if (ec || results.empty()) {
        error = ec ? ec.message() : "no addresses for host";
        return false;
    }
    out = results.begin()->endpoint();
    return true;
}

JSValue asio_listen(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)thisVal;
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    if (!runtime) {
        return JS_ThrowInternalError(ctx, "runtime unavailable");
    }

    std::string host = "127.0.0.1";
    int32_t port = 0;  // 0 selects an ephemeral port
    if (argc > 0 && JS_IsObject(argv[0])) {
        std::string requestedHost;
        if (get_string_prop(ctx, argv[0], "host", requestedHost) && !requestedHost.empty()) {
            host = requestedHost;
        }
        int32_t requestedPort = 0;
        if (get_int_prop(ctx, argv[0], "port", requestedPort)) {
            if (requestedPort < 0 || requestedPort > 65535) {
                return rejected_promise(ctx, runtime,
                    make_error(ctx, AsioErr::InvalidArgument, "listen", host, 0, "port must be 0-65535"));
            }
            port = requestedPort;
        }
    }

    // Capability gate: listening is denied by default.
    if (auto ok = runtime->authorizeNetworkListen(host, static_cast<uint16_t>(port)); !ok) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::PermissionDenied, "listen", host, static_cast<uint16_t>(port),
                ok.error().code() + ": " + ok.error().message()));
    }

    asio::io_context& io = ensure_io();
    asio::ip::tcp::endpoint endpoint;
    std::string resolveError;
    if (!resolve_bind_endpoint(io, host, static_cast<uint16_t>(port), endpoint, resolveError)) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::ResolveFailed, "listen", host, static_cast<uint16_t>(port), resolveError));
    }

    // bind()/listen() do not block, so the acceptor is opened synchronously here
    // (before the io_context thread touches it) and the server is returned ready.
    auto handle = std::make_shared<TcpServerHandle>(io);
    asio::error_code ec;
    handle->acceptor.open(endpoint.protocol(), ec);
    if (!ec) {
        handle->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    }
    if (!ec) {
        handle->acceptor.bind(endpoint, ec);
    }
    if (!ec) {
        handle->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::ListenFailed, "listen", host, static_cast<uint16_t>(port), ec.message()));
    }

    std::shared_ptr<Promise> p;
    JSValue jsPromise = make_promise(ctx, runtime, p);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }
    settle_value(p, new_server_object(ctx, handle));
    return jsPromise;
}

// --- TcpServer.accept() --------------------------------------------------------

struct AcceptOp {
    std::shared_ptr<Promise> promise;
    std::shared_ptr<TcpServerHandle> server;
    std::shared_ptr<TcpSocketHandle> client;
    asio::steady_timer timer;
    long timeoutMs = 0;
    bool done = false;

    AcceptOp(asio::io_context& io) : timer(io) {}
};

void accept_finish_socket(const std::shared_ptr<AcceptOp>& op) {
    auto promise = op->promise;
    auto server = op->server;
    auto client = op->client;
    promise->runtime->async().post([promise, server, client] {
        server->accepting = false;
        settle_value(promise, new_socket_object(promise->ctx, client));
        promise->runtime->async().endOperation();
    });
}

void accept_finish_error(const std::shared_ptr<AcceptOp>& op, AsioErr code, std::string cause) {
    auto promise = op->promise;
    auto server = op->server;
    promise->runtime->async().post([promise, server, code, cause] {
        server->accepting = false;
        settle_error(promise, make_error(promise->ctx, code, "accept", "", 0, cause));
        promise->runtime->async().endOperation();
    });
}

JSValue tcp_server_accept(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
    auto server = get_server(ctx, thisVal);
    if (!runtime || !server) {
        return JS_ThrowTypeError(ctx, "accept() called on a non-server");
    }
    if (server->closed) {
        return rejected_promise(ctx, runtime, make_error(ctx, AsioErr::Closed, "accept", "", 0, "server is closed"));
    }
    if (server->accepting) {
        return rejected_promise(ctx, runtime,
            make_error(ctx, AsioErr::InvalidArgument, "accept", "", 0, "an accept is already in progress"));
    }

    int32_t timeoutMs = 0;
    if (argc > 0 && JS_IsObject(argv[0])) {
        get_int_prop(ctx, argv[0], "timeoutMs", timeoutMs);
    }

    asio::io_context& io = ensure_io();
    auto op = std::make_shared<AcceptOp>(io);
    op->server = server;
    op->client = std::make_shared<TcpSocketHandle>(io);
    op->timeoutMs = timeoutMs;

    JSValue jsPromise = make_promise(ctx, runtime, op->promise);
    if (JS_IsException(jsPromise)) {
        return jsPromise;
    }

    server->accepting = true;
    runtime->async().beginOperation();
    asio::post(io, [op] {
        op->server->acceptor.async_accept(op->client->socket,
            [op](const asio::error_code& ec) {
                if (op->done) {
                    return;
                }
                op->done = true;
                op->timer.cancel();
                if (!ec) {
                    accept_finish_socket(op);
                } else if (ec == asio::error::operation_aborted) {
                    accept_finish_error(op, op->server->closed ? AsioErr::Closed : AsioErr::Timeout, ec.message());
                } else {
                    accept_finish_error(op, AsioErr::AcceptFailed, ec.message());
                }
            });

        if (op->timeoutMs > 0) {
            op->timer.expires_after(std::chrono::milliseconds(op->timeoutMs));
            op->timer.async_wait([op](const asio::error_code& ec) {
                if (ec || op->done) {
                    return;
                }
                op->done = true;
                asio::error_code ignored;
                op->server->acceptor.cancel(ignored);
                accept_finish_error(op, AsioErr::Timeout, "accept timed out");
            });
        }
    });

    return jsPromise;
}

JSValue tcp_server_address(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto server = get_server(ctx, thisVal);
    if (!server) {
        return JS_ThrowTypeError(ctx, "address() called on a non-server");
    }
    asio::error_code ec;
    auto endpoint = server->acceptor.local_endpoint(ec);
    if (ec) {
        return JS_Throw(ctx, make_error(ctx, AsioErr::ListenFailed, "address", "", 0, ec.message()));
    }
    return endpoint_to_object(ctx, endpoint);
}

JSValue tcp_server_close(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    (void)argc;
    (void)argv;
    auto server = get_server(ctx, thisVal);
    if (!server) {
        return JS_ThrowTypeError(ctx, "close() called on a non-server");
    }
    if (!server->closed) {
        server->closed = true;
        asio::post(server->acceptor.get_executor(), [server] {
            asio::error_code ignored;
            server->acceptor.cancel(ignored);
            server->acceptor.close(ignored);
        });
    }
    return JS_UNDEFINED;
}

// --- Module wiring -------------------------------------------------------------

void register_socket_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (tcp_socket_class_id == 0) {
        JS_NewClassID(&tcp_socket_class_id);
    }
    JSClassDef def{};
    def.class_name = "TcpSocket";
    def.finalizer = tcp_socket_finalizer;
    JS_NewClass(rt, tcp_socket_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "read", JS_NewCFunction(ctx, tcp_socket_read, "read", 1));
    JS_SetPropertyStr(ctx, proto, "write", JS_NewCFunction(ctx, tcp_socket_write, "write", 2));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, tcp_socket_close, "close", 0));
    JS_SetPropertyStr(ctx, proto, "remoteAddress", JS_NewCFunction(ctx, tcp_socket_remote_address, "remoteAddress", 0));
    JS_SetPropertyStr(ctx, proto, "localAddress", JS_NewCFunction(ctx, tcp_socket_local_address, "localAddress", 0));
    JS_SetClassProto(ctx, tcp_socket_class_id, proto);
}

void register_server_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (tcp_server_class_id == 0) {
        JS_NewClassID(&tcp_server_class_id);
    }
    JSClassDef def{};
    def.class_name = "TcpServer";
    def.finalizer = tcp_server_finalizer;
    JS_NewClass(rt, tcp_server_class_id, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "accept", JS_NewCFunction(ctx, tcp_server_accept, "accept", 1));
    JS_SetPropertyStr(ctx, proto, "address", JS_NewCFunction(ctx, tcp_server_address, "address", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, tcp_server_close, "close", 0));
    JS_SetClassProto(ctx, tcp_server_class_id, proto);
}

int init_asio_module(JSContext* ctx, JSModuleDef* module) {
    register_socket_class(ctx);
    register_server_class(ctx);
    JS_SetModuleExport(ctx, module, "connect", JS_NewCFunction(ctx, asio_connect, "connect", 1));
    JS_SetModuleExport(ctx, module, "listen", JS_NewCFunction(ctx, asio_listen, "listen", 1));
    return 0;
}

#endif  // WL2_HAVE_QUICKJS

}  // namespace

wl2::ModuleInfo wl2_asio_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:asio", wl2_asio_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:asio",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "25de54b1-78cb-4109-9a31-b9aed98807a1",
        .summary = "Asynchronous TCP networking module backed by standalone Asio.",
        .api = AsioApi,
        .unloadSafe = true,
    };
}

extern "C" void* wl2_asio_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    if (auto* runtime = static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx))) {
        runtime->async().registerShutdownHook([] { shutdown_asio_state(); });
    }
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, init_asio_module);
    if (!module) {
        return nullptr;
    }
    JS_AddModuleExport(ctx, module, "connect");
    JS_AddModuleExport(ctx, module, "listen");
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_ASIO_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) {
        return 1;
    }
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:asio";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "25de54b1-78cb-4109-9a31-b9aed98807a1";
    out->summary = "Asynchronous TCP networking module backed by standalone Asio.";
    out->api = AsioApi;
    out->unload_safe = 1;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
