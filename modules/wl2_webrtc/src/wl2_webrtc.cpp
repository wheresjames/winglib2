#include "webrtc/internal.h"

namespace wl2_webrtc {

const char* const WebRtcApi = R"(Exports JavaScript module wl2:webrtc.

WebRTC client/server backed by libdatachannel.

Functions:
  version()                       -> { module, libdatachannel: { version } }
  capabilities()                  -> { provider, backend, tlsBackend, media, websocket, dataChannel }
  PeerConnection.create(options?) -> PeerConnection
  WebSocket.connect(options)      -> WebSocket
  SignalingServer.listen(options) -> SignalingServer

  PeerConnection methods:
  createDataChannel(label)        -> DataChannel (triggers negotiation)
  addTrack(options)               -> Track (RTP from PacketBuffer)
  setRemoteDescription({ type, sdp }) -> { ok }
  addIceCandidate({ candidate, sdpMid? }) -> { ok }
  localDescription()              -> { type, sdp } | null
  state()                         -> { connection, ice, gathering, signaling }
  poll({ timeoutMs?, max? })      -> [ event, ... ]   (drained off the lib threads)
  stats()                         -> { pendingEvents, droppedEvents, bytesSent, bytesReceived, rttMs, selectedCandidatePair, closed }
  close()

poll() events:
  { type: "local-description", description: { type, sdp } }
  { type: "local-candidate", candidate: { candidate, sdpMid } }
  { type: "state-change", connection, ice, gathering, signaling }
  { type: "data-channel", label, channel: DataChannel }
  { type: "track", media, packetBufferName, track: Track }
  { type: "error", message }

DataChannel methods:
  send(data)                      -> { ok, buffered }   (string or bytes)
  poll({ timeoutMs?, max? })      -> [ { data, binary } ]
  label() / isOpen() / bufferedAmount() / close()

Track methods:
  pump({ timeoutMs?, max? })       -> { sent, bytes, lastSequence }
  stats()                         -> { sentPackets, receivedPackets, droppedPackets, ... }
  packetBufferName() / mid() / isOpen() / close()

Media tracks move RTP packets only. addTrack({ media, codec, payloadType,
sendPacketBufferName }) opens an existing PacketBuffer and pump() copies RTP
records into libdatachannel. Inbound tracks create a receive PacketBuffer and
emit its name in the track event.

WebSocket methods:
  send(data) / poll({ timeoutMs?, max? }) / state() / close()

SignalingServer methods:
  poll({ timeoutMs?, max? }) / send(clientId, data) / port() / close()

Security model:
  ICE also reaches peer-advertised addresses; configured STUN/TURN endpoints are
  authorized via the runtime network policy before the peer is built. Use
  loopbackOnly:true for host-candidate, no-server operation. See docs/security.md.

Stable error codes:
  webrtc_invalid_argument, webrtc_permission_denied, webrtc_closed, webrtc_failed.)";

#if WL2_HAVE_QUICKJS

JSValue throw_webrtc_error(JSContext* ctx, const char* code, const char* operation,
    const std::string& message) {
    JSValue error = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, "WebRtcError"));
    JS_SetPropertyStr(ctx, error, "module", JS_NewString(ctx, "wl2_webrtc"));
    JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, code));
    JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx, operation));
    JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message.c_str()));
    return JS_Throw(ctx, error);
}

wl2::Runtime* current_runtime(JSContext* ctx) {
    return static_cast<wl2::Runtime*>(JS_GetContextOpaque(ctx));
}

bool option_string(JSContext* ctx, JSValueConst options, const char* key, std::string& out) {
    if (!JS_IsObject(options)) return false;
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (!JS_IsString(value)) { JS_FreeValue(ctx, value); return false; }
    const char* text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!text) return false;
    out = text;
    JS_FreeCString(ctx, text);
    return true;
}

bool option_int(JSContext* ctx, JSValueConst options, const char* key, int64_t& out) {
    if (!JS_IsObject(options)) return false;
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) { JS_FreeValue(ctx, value); return false; }
    int64_t parsed = 0;
    int rc = JS_ToInt64(ctx, &parsed, value);
    JS_FreeValue(ctx, value);
    if (rc < 0) return false;
    out = parsed;
    return true;
}

bool option_bool(JSContext* ctx, JSValueConst options, const char* key, bool fallback) {
    if (!JS_IsObject(options)) return fallback;
    JSValue value = JS_GetPropertyStr(ctx, options, key);
    if (JS_IsUndefined(value) || JS_IsNull(value)) { JS_FreeValue(ctx, value); return fallback; }
    bool result = JS_ToBool(ctx, value) == 1;
    JS_FreeValue(ctx, value);
    return result;
}

JSValue webrtc_version_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JSValue module = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module, "version", JS_NewString(ctx, WL2_VERSION));
    JS_SetPropertyStr(ctx, module, "build", JS_NewString(ctx, WL2_BUILD));
    JS_SetPropertyStr(ctx, obj, "module", module);
    JSValue lib = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, lib, "version", JS_NewString(ctx, WL2_WEBRTC_LIBVERSION));
    JS_SetPropertyStr(ctx, obj, "libdatachannel", lib);
    return obj;
}

JSValue webrtc_capabilities_fn(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "provider", JS_NewString(ctx, WL2_WEBRTC_PROVIDER_USED));
    JS_SetPropertyStr(ctx, obj, "backend", JS_NewString(ctx, "libdatachannel"));
    JS_SetPropertyStr(ctx, obj, "tlsBackend", JS_NewString(ctx, WL2_WEBRTC_TLS_BACKEND));
    JS_SetPropertyStr(ctx, obj, "dataChannel", JS_NewBool(ctx, true));
#if RTC_ENABLE_MEDIA
    JS_SetPropertyStr(ctx, obj, "media", JS_NewBool(ctx, true));
#else
    JS_SetPropertyStr(ctx, obj, "media", JS_NewBool(ctx, false));
#endif
#if RTC_ENABLE_WEBSOCKET
    JS_SetPropertyStr(ctx, obj, "websocket", JS_NewBool(ctx, true));
#else
    JS_SetPropertyStr(ctx, obj, "websocket", JS_NewBool(ctx, false));
#endif
    return obj;
}

int init_webrtc_module(JSContext* ctx, JSModuleDef* module) {
    register_webrtc_classes(ctx);
    JS_SetModuleExport(ctx, module, "version", JS_NewCFunction(ctx, webrtc_version_fn, "version", 0));
    JS_SetModuleExport(ctx, module, "capabilities", JS_NewCFunction(ctx, webrtc_capabilities_fn, "capabilities", 0));
    JSValue peerConnection = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, peerConnection, "create",
        JS_NewCFunction(ctx, webrtc_peerconnection_create_fn, "create", 1));
    JS_SetModuleExport(ctx, module, "PeerConnection", peerConnection);
    JSValue webSocket = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, webSocket, "connect",
        JS_NewCFunction(ctx, webrtc_websocket_connect_fn, "connect", 1));
    JS_SetModuleExport(ctx, module, "WebSocket", webSocket);
    JSValue signalingServer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, signalingServer, "listen",
        JS_NewCFunction(ctx, webrtc_signaling_server_listen_fn, "listen", 1));
    JS_SetModuleExport(ctx, module, "SignalingServer", signalingServer);
    return 0;
}

constexpr const char* kExportNames[] = {"version", "capabilities", "PeerConnection", "WebSocket", "SignalingServer"};

#endif // WL2_HAVE_QUICKJS

} // namespace wl2_webrtc

wl2::ModuleInfo wl2_webrtc_register_module(wl2::Runtime& runtime) {
#if WL2_HAVE_QUICKJS
    runtime.registerQuickJsModule("wl2:webrtc", wl2_webrtc_quickjs_module_factory);
#else
    (void)runtime;
#endif
    return wl2::ModuleInfo{
        .abiVersion = wl2::ModuleAbiVersion,
        .name = "wl2:webrtc",
        .version = WL2_VERSION,
        .build = WL2_BUILD,
        .stableId = "4d8f2a17-6c3e-4b90-9e21-0f5a7c9d1b44",
        .summary = "WebRTC client/server (libdatachannel): peer connections, data channels, signaling.",
        .api = wl2_webrtc::WebRtcApi,
        .unloadSafe = false,
    };
}

extern "C" void* wl2_webrtc_quickjs_module_factory(void* context, const char* moduleName) {
#if WL2_HAVE_QUICKJS
    auto* ctx = static_cast<JSContext*>(context);
    JSModuleDef* module = JS_NewCModule(ctx, moduleName, wl2_webrtc::init_webrtc_module);
    if (!module) return nullptr;
    for (const char* name : wl2_webrtc::kExportNames) {
        JS_AddModuleExport(ctx, module, name);
    }
    return module;
#else
    (void)context;
    (void)moduleName;
    return nullptr;
#endif
}

#if !WL2_WEBRTC_STATIC_MODULE
extern "C" int wl2_module_get_info(wl2_module_info* out) {
    if (!out) return 1;
    out->abi_version = wl2::ModuleAbiVersion;
    out->name = "wl2:webrtc";
    out->version = WL2_VERSION;
    out->build = WL2_BUILD;
    out->stable_id = "4d8f2a17-6c3e-4b90-9e21-0f5a7c9d1b44";
    out->summary = "WebRTC client/server (libdatachannel): peer connections, data channels, signaling.";
    out->api = wl2_webrtc::WebRtcApi;
    out->unload_safe = 0;
    out->required_wl2_version = WL2_VERSION;
    return 0;
}
#endif
