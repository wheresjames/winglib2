#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <variant>

#if WL2_HAVE_QUICKJS

namespace wl2_webrtc {

namespace {

struct SignalMessage {
    std::string type;
    int64_t clientId = 0;
    bool binary = false;
    std::string data;
    std::string detail;
};

struct SignalQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<SignalMessage> events;
    std::atomic<int64_t> dropped{0};
    size_t maxQueued = 4096;

    void push(SignalMessage&& event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (events.size() >= maxQueued) {
                events.pop_front();
                dropped.fetch_add(1);
            }
            events.push_back(std::move(event));
        }
        cv.notify_all();
    }
};

struct WebSocketBox {
    std::shared_ptr<rtc::WebSocket> ws;
    std::shared_ptr<SignalQueue> queue;
};

struct SignalingServerState {
    std::shared_ptr<SignalQueue> queue;
    std::mutex clientsMutex;
    std::map<int64_t, std::shared_ptr<rtc::WebSocket>> clients;
    std::atomic<int64_t> nextClientId{1};
    std::atomic<bool> alive{true};
    std::string path;
};

struct SignalingServerBox {
    std::shared_ptr<rtc::WebSocketServer> server;
    std::shared_ptr<SignalingServerState> state;
};

std::string js_string_bytes(JSContext* ctx, JSValueConst value) {
    size_t len = 0;
    const char* text = JS_ToCStringLen(ctx, &len, value);
    if (!text) return {};
    std::string out(text, len);
    JS_FreeCString(ctx, text);
    return out;
}

bool js_payload(JSContext* ctx, JSValueConst value, std::string& out, bool& binary) {
    if (JS_IsString(value)) {
        out = js_string_bytes(ctx, value);
        binary = false;
        return true;
    }
    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, value);
    if (bytes) {
        out.assign(reinterpret_cast<const char*>(bytes), byteLength);
        binary = true;
        return true;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t byteOffset = 0, viewLength = 0, bytesPerElement = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &viewLength, &bytesPerElement);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return false;
    }
    bytes = JS_GetArrayBuffer(ctx, &byteLength, ab);
    bool ok = false;
    if (bytes && byteOffset + viewLength <= byteLength) {
        out.assign(reinterpret_cast<const char*>(bytes) + byteOffset, viewLength);
        binary = true;
        ok = true;
    }
    JS_FreeValue(ctx, ab);
    return ok;
}

std::string ws_state_name(rtc::WebSocket::State state) {
    switch (state) {
        case rtc::WebSocket::State::Connecting: return "connecting";
        case rtc::WebSocket::State::Open: return "open";
        case rtc::WebSocket::State::Closing: return "closing";
        case rtc::WebSocket::State::Closed: return "closed";
    }
    return "unknown";
}

struct UrlParts {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
};

bool parse_ws_url(const std::string& url, UrlParts& out) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    out.scheme = url.substr(0, schemeEnd);
    if (out.scheme != "ws" && out.scheme != "wss") return false;
    std::string rest = url.substr(schemeEnd + 3);
    if (auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash);
    }
    if (auto at = rest.find('@'); at != std::string::npos) {
        rest = rest.substr(at + 1);
    }
    out.port = out.scheme == "wss" ? 443 : 80;
    if (auto colon = rest.rfind(':'); colon != std::string::npos) {
        out.host = rest.substr(0, colon);
        try {
            int parsed = std::stoi(rest.substr(colon + 1));
            if (parsed < 0 || parsed > 65535) return false;
            out.port = static_cast<uint16_t>(parsed);
        } catch (...) {
            return false;
        }
    } else {
        out.host = rest;
    }
    return !out.host.empty();
}

JSValue make_signal_events(JSContext* ctx, const std::deque<SignalMessage>& events) {
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const auto& e : events) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, e.type.c_str()));
        if (e.clientId) JS_SetPropertyStr(ctx, obj, "clientId", JS_NewInt64(ctx, e.clientId));
        if (!e.detail.empty()) JS_SetPropertyStr(ctx, obj, "detail", JS_NewString(ctx, e.detail.c_str()));
        if (e.type == "message") {
            JS_SetPropertyStr(ctx, obj, "binary", JS_NewBool(ctx, e.binary));
            if (e.binary) {
                JS_SetPropertyStr(ctx, obj, "data", JS_NewArrayBufferCopy(ctx,
                    reinterpret_cast<const uint8_t*>(e.data.data()), e.data.size()));
            } else {
                JS_SetPropertyStr(ctx, obj, "data", JS_NewStringLen(ctx, e.data.data(), e.data.size()));
            }
        }
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

std::deque<SignalMessage> drain_signal_queue(SignalQueue& queue, int64_t timeoutMs, int64_t maxEvents) {
    std::deque<SignalMessage> out;
    std::unique_lock<std::mutex> lock(queue.mutex);
    if (queue.events.empty() && timeoutMs > 0) {
        queue.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !queue.events.empty(); });
    }
    while (!queue.events.empty() && static_cast<int64_t>(out.size()) < std::max<int64_t>(0, maxEvents)) {
        out.push_back(std::move(queue.events.front()));
        queue.events.pop_front();
    }
    return out;
}

void bind_websocket_events(const std::shared_ptr<rtc::WebSocket>& ws, const std::shared_ptr<SignalQueue>& queue,
    int64_t clientId = 0) {
    ws->onOpen([queue, clientId] {
        queue->push({"open", clientId});
    });
    ws->onClosed([queue, clientId] {
        queue->push({clientId ? "client-disconnected" : "closed", clientId});
    });
    ws->onError([queue, clientId](std::string error) {
        SignalMessage e;
        e.type = "error";
        e.clientId = clientId;
        e.detail = std::move(error);
        queue->push(std::move(e));
    });
    ws->onMessage([queue, clientId](rtc::message_variant data) {
        SignalMessage e;
        e.type = "message";
        e.clientId = clientId;
        std::visit([&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, rtc::binary>) {
                e.binary = true;
                e.data.assign(reinterpret_cast<const char*>(arg.data()), arg.size());
            } else {
                e.binary = false;
                e.data.assign(arg.begin(), arg.end());
            }
        }, data);
        queue->push(std::move(e));
    });
}

WebSocketBox* live_ws(JSContext* ctx, JSValueConst thisVal, const char* op) {
    auto* box = static_cast<WebSocketBox*>(JS_GetOpaque2(ctx, thisVal, g_wsClassId));
    if (!box || !box->ws) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", op, "Not a live WebSocket");
        return nullptr;
    }
    return box;
}

SignalingServerBox* live_server(JSContext* ctx, JSValueConst thisVal, const char* op) {
    auto* box = static_cast<SignalingServerBox*>(JS_GetOpaque2(ctx, thisVal, g_signalingServerClassId));
    if (!box || !box->server) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", op, "Not a live SignalingServer");
        return nullptr;
    }
    return box;
}

void ws_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<WebSocketBox*>(JS_GetOpaque(value, g_wsClassId));
    if (box) {
        if (box->ws) {
            try { box->ws->close(); } catch (...) {}
        }
        delete box;
    }
}

void server_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<SignalingServerBox*>(JS_GetOpaque(value, g_signalingServerClassId));
    if (box) {
        if (box->state) {
            box->state->alive.store(false);
            std::lock_guard<std::mutex> lock(box->state->clientsMutex);
            for (auto& [_, client] : box->state->clients) {
                if (client) {
                    try { client->close(); } catch (...) {}
                }
            }
            box->state->clients.clear();
        }
        if (box->server) {
            try { box->server->stop(); } catch (...) {}
        }
        delete box;
    }
}

JSValue new_ws_object(JSContext* ctx, std::shared_ptr<rtc::WebSocket> ws, std::shared_ptr<SignalQueue> queue) {
    JSValue obj = JS_NewObjectClass(ctx, g_wsClassId);
    if (JS_IsException(obj)) return obj;
    auto* box = new WebSocketBox{std::move(ws), std::move(queue)};
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue new_server_object(JSContext* ctx, std::shared_ptr<rtc::WebSocketServer> server,
    std::shared_ptr<SignalingServerState> state) {
    JSValue obj = JS_NewObjectClass(ctx, g_signalingServerClassId);
    if (JS_IsException(obj)) return obj;
    auto* box = new SignalingServerBox{std::move(server), std::move(state)};
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue ws_send(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    WebSocketBox* box = live_ws(ctx, thisVal, "WebSocket.send");
    if (!box) return JS_EXCEPTION;
    if (argc < 1) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "WebSocket.send", "send(data) requires data");
    }
    std::string payload;
    bool binary = false;
    if (!js_payload(ctx, argv[0], payload, binary)) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "WebSocket.send",
            "send(data) requires a string or bytes");
    }
    try {
        if (binary) {
            box->ws->send(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        } else {
            box->ws->send(payload);
        }
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "WebSocket.send", e.what());
    }
    return JS_UNDEFINED;
}

JSValue ws_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    WebSocketBox* box = live_ws(ctx, thisVal, "WebSocket.poll");
    if (!box) return JS_EXCEPTION;
    int64_t timeoutMs = 0;
    int64_t maxEvents = 128;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxEvents);
    }
    return make_signal_events(ctx, drain_signal_queue(*box->queue, timeoutMs, maxEvents));
}

JSValue ws_state(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    WebSocketBox* box = live_ws(ctx, thisVal, "WebSocket.state");
    if (!box) return JS_EXCEPTION;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "readyState", JS_NewString(ctx, ws_state_name(box->ws->readyState()).c_str()));
    JS_SetPropertyStr(ctx, obj, "open", JS_NewBool(ctx, box->ws->isOpen()));
    if (auto addr = box->ws->remoteAddress()) {
        JS_SetPropertyStr(ctx, obj, "remoteAddress", JS_NewString(ctx, addr->c_str()));
    }
    if (auto path = box->ws->path()) {
        JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, path->c_str()));
    }
    return obj;
}

JSValue ws_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<WebSocketBox*>(JS_GetOpaque2(ctx, thisVal, g_wsClassId));
    if (box && box->ws) {
        try { box->ws->close(); } catch (...) {}
    }
    return JS_UNDEFINED;
}

JSValue server_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    SignalingServerBox* box = live_server(ctx, thisVal, "SignalingServer.poll");
    if (!box) return JS_EXCEPTION;
    int64_t timeoutMs = 0;
    int64_t maxEvents = 128;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxEvents);
    }
    return make_signal_events(ctx, drain_signal_queue(*box->state->queue, timeoutMs, maxEvents));
}

JSValue server_send(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    SignalingServerBox* box = live_server(ctx, thisVal, "SignalingServer.send");
    if (!box) return JS_EXCEPTION;
    if (argc < 2) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "SignalingServer.send",
            "send(clientId, data) requires a client id and data");
    }
    int64_t clientId = 0;
    JS_ToInt64(ctx, &clientId, argv[0]);
    std::string payload;
    bool binary = false;
    if (!js_payload(ctx, argv[1], payload, binary)) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "SignalingServer.send",
            "data must be a string or bytes");
    }
    std::shared_ptr<rtc::WebSocket> client;
    {
        std::lock_guard<std::mutex> lock(box->state->clientsMutex);
        auto it = box->state->clients.find(clientId);
        if (it != box->state->clients.end()) {
            client = it->second;
            if (!client || !client->isOpen()) {
                box->state->clients.erase(it);
                client.reset();
            }
        }
    }
    if (!client) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "SignalingServer.send",
            "Unknown signaling client id");
    }
    try {
        if (binary) client->send(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
        else client->send(payload);
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "SignalingServer.send", e.what());
    }
    return JS_UNDEFINED;
}

JSValue server_port(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    SignalingServerBox* box = live_server(ctx, thisVal, "SignalingServer.port");
    if (!box) return JS_EXCEPTION;
    return JS_NewInt32(ctx, box->server->port());
}

JSValue server_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<SignalingServerBox*>(JS_GetOpaque2(ctx, thisVal, g_signalingServerClassId));
    if (box && box->server) {
        if (box->state) {
            box->state->alive.store(false);
            std::lock_guard<std::mutex> lock(box->state->clientsMutex);
            for (auto& [_, client] : box->state->clients) {
                if (client) {
                    try { client->close(); } catch (...) {}
                }
            }
            box->state->clients.clear();
        }
        try { box->server->stop(); } catch (...) {}
    }
    return JS_UNDEFINED;
}

} // namespace

void register_signaling_classes(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (g_wsClassId == 0) JS_NewClassID(&g_wsClassId);
    JSClassDef wsDef{};
    wsDef.class_name = "WebSocket";
    wsDef.finalizer = ws_finalizer;
    JS_NewClass(rt, g_wsClassId, &wsDef);
    JSValue wsProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, wsProto, "send", JS_NewCFunction(ctx, ws_send, "send", 1));
    JS_SetPropertyStr(ctx, wsProto, "poll", JS_NewCFunction(ctx, ws_poll, "poll", 1));
    JS_SetPropertyStr(ctx, wsProto, "state", JS_NewCFunction(ctx, ws_state, "state", 0));
    JS_SetPropertyStr(ctx, wsProto, "close", JS_NewCFunction(ctx, ws_close, "close", 0));
    JS_SetClassProto(ctx, g_wsClassId, wsProto);

    if (g_signalingServerClassId == 0) JS_NewClassID(&g_signalingServerClassId);
    JSClassDef serverDef{};
    serverDef.class_name = "SignalingServer";
    serverDef.finalizer = server_finalizer;
    JS_NewClass(rt, g_signalingServerClassId, &serverDef);
    JSValue serverProto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, serverProto, "poll", JS_NewCFunction(ctx, server_poll, "poll", 1));
    JS_SetPropertyStr(ctx, serverProto, "send", JS_NewCFunction(ctx, server_send, "send", 2));
    JS_SetPropertyStr(ctx, serverProto, "port", JS_NewCFunction(ctx, server_port, "port", 0));
    JS_SetPropertyStr(ctx, serverProto, "close", JS_NewCFunction(ctx, server_close, "close", 0));
    JS_SetClassProto(ctx, g_signalingServerClassId, serverProto);
}

JSValue webrtc_websocket_connect_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "WebSocket.connect",
            "WebSocket.connect(options) requires a url");
    }
    std::string url;
    if (!option_string(ctx, argv[0], "url", url) || url.empty()) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "WebSocket.connect",
            "WebSocket.connect(options) requires a url string");
    }
    UrlParts parts;
    if (!parse_ws_url(url, parts)) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "WebSocket.connect",
            "Invalid WebSocket URL: " + url);
    }
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        return throw_webrtc_error(ctx, "webrtc_permission_denied", "WebSocket.connect",
            "Runtime is unavailable");
    }
    if (auto ok = runtime->authorizeNetworkConnect(parts.host, parts.port); !ok) {
        return throw_webrtc_error(ctx, "webrtc_permission_denied", "WebSocket.connect", ok.error().message());
    }

    rtc::WebSocket::Configuration config;
    int64_t timeoutMs = 0;
    if (option_int(ctx, argv[0], "timeoutMs", timeoutMs) && timeoutMs >= 0) {
        config.connectionTimeout = std::chrono::milliseconds(timeoutMs);
    }
    bool disableTlsVerification = option_bool(ctx, argv[0], "disableTlsVerification", false);
    config.disableTlsVerification = disableTlsVerification;

    auto queue = std::make_shared<SignalQueue>();
    std::shared_ptr<rtc::WebSocket> ws;
    try {
        ws = std::make_shared<rtc::WebSocket>(config);
        bind_websocket_events(ws, queue);
        ws->open(url);
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "WebSocket.connect", e.what());
    }
    return new_ws_object(ctx, std::move(ws), std::move(queue));
}

JSValue webrtc_signaling_server_listen_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "SignalingServer.listen",
            "SignalingServer.listen(options) requires host and port");
    }
    std::string host = "127.0.0.1";
    std::string path = "/ws";
    int64_t port = 8080;
    option_string(ctx, argv[0], "host", host);
    option_string(ctx, argv[0], "path", path);
    option_int(ctx, argv[0], "port", port);
    if (port < 0 || port > 65535) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "SignalingServer.listen",
            "port must be 0..65535");
    }
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        return throw_webrtc_error(ctx, "webrtc_permission_denied", "SignalingServer.listen",
            "Runtime is unavailable");
    }
    if (auto ok = runtime->authorizeNetworkListen(host, static_cast<uint16_t>(port)); !ok) {
        return throw_webrtc_error(ctx, "webrtc_permission_denied", "SignalingServer.listen", ok.error().message());
    }

    rtc::WebSocketServer::Configuration config;
    config.bindAddress = host;
    config.port = static_cast<uint16_t>(port);
    int64_t timeoutMs = 0;
    if (option_int(ctx, argv[0], "timeoutMs", timeoutMs) && timeoutMs >= 0) {
        config.connectionTimeout = std::chrono::milliseconds(timeoutMs);
    }
    auto state = std::make_shared<SignalingServerState>();
    state->queue = std::make_shared<SignalQueue>();
    state->path = path;
    std::shared_ptr<rtc::WebSocketServer> server;
    try {
        server = std::make_shared<rtc::WebSocketServer>(config);
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "SignalingServer.listen", e.what());
    }
    JSValue obj = new_server_object(ctx, server, state);
    if (JS_IsException(obj)) return obj;
    std::weak_ptr<SignalingServerState> weakState = state;
    server->onClient([weakState](std::shared_ptr<rtc::WebSocket> client) {
        auto state = weakState.lock();
        if (!state || !state->alive.load() || !client) return;
        const int64_t id = state->nextClientId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(state->clientsMutex);
            if (!state->alive.load()) return;
            state->clients[id] = client;
        }
        bind_websocket_events(client, state->queue, id);
        SignalMessage e;
        e.type = "client-connected";
        e.clientId = id;
        if (auto p = client->path()) e.detail = *p;
        state->queue->push(std::move(e));
    });
    return obj;
}

} // namespace wl2_webrtc

#endif // WL2_HAVE_QUICKJS
