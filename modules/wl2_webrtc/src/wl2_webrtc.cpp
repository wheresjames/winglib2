#include "webrtc/internal.h"

#include <cstdio>
#include <cstring>

namespace wl2_webrtc {

const char* const WebRtcApi = R"(Exports JavaScript module wl2:webrtc.

WebRTC client/server backed by libdatachannel.

Functions:
  version()                       -> { module, libdatachannel: { version } }
  capabilities()                  -> { provider, backend, tlsBackend, media, websocket, dataChannel }
  PeerConnection.create(options?) -> PeerConnection
  WebSocket.connect(options)      -> WebSocket
  SignalingServer.listen(options) -> SignalingServer

High-level helpers (JavaScript, see docs/api.md):
  new MediaSession(options)       -> one sender-offers peer (addTrack, start, handleSignal, pump)
  new SignalingHub(options)       -> multiplex MediaSessions over a signaling socket with auth

  PeerConnection methods:
  createDataChannel(label)        -> DataChannel (triggers negotiation)
  addTrack(options)               -> Track (RTP from PacketBuffer, autoOffer defaults true)
  setRemoteDescription({ type, sdp }) -> { ok }
  setLocalDescription("offer"|"answer") -> { ok }
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

// High-level JavaScript helpers layered on top of the native PeerConnection.
// Authored in JS (it is signaling glue, not a hot path) and evaluated at module
// init as a factory that closes over the native exports. Kept here rather than a
// separate file because the module loader only resolves named modules -- a loose
// ./helpers.js could not be imported. See docs/api.md for the public shape.
const char* const kWebRtcHelpersJs = R"WLRTC(
(function (native) {
  "use strict";
  var PeerConnection = native.PeerConnection;

  function nowMs() {
    try { return wl2.runtime.now(); } catch (e) { return Date.now(); }
  }

  // One peer connection streaming media to (or exchanging media with) a single
  // remote. Sender-offers role: add tracks, then start() emits an SDP offer via
  // the `signal` callback; feed the remote's answer/candidates back through
  // handleSignal(). Media is pumped on demand -- drive pump() from the remote's
  // periodic "pump" ticks (there is no server-side timer).
  class MediaSession {
    constructor(options) {
      var opts = options || {};
      this.signal = typeof opts.signal === "function" ? opts.signal : function () {};
      this._onState = typeof opts.onState === "function" ? opts.onState : null;
      this._onStatus = typeof opts.onStatus === "function" ? opts.onStatus : null;
      this._onPump = [];
      this._onClose = [];
      this.tracks = [];
      this.sentPackets = 0;
      this.sentBytes = 0;
      this.closed = false;
      this.started = false;
      this._lastStateAt = 0;
      this.data = {}; // free-form slot for the app (e.g. a GStreamer pipeline)
      // Drive negotiation explicitly (one offer per start()); disabling
      // auto-negotiation keeps the signaling state deterministic.
      var create = { loopbackOnly: opts.loopbackOnly === true, disableAutoNegotiation: true };
      if (opts.receivePacketBufferNamePrefix) {
        create.receivePacketBufferNamePrefix = opts.receivePacketBufferNamePrefix;
      }
      var ice = opts.iceServers || {};
      if (ice.stunServer) create.stunServer = ice.stunServer;
      if (Array.isArray(ice.turnServers)) create.turnServers = ice.turnServers;
      if (ice.iceTransportPolicy) create.iceTransportPolicy = ice.iceTransportPolicy;
      this.pc = PeerConnection.create(create);
    }

    onClose(cb) { if (typeof cb === "function") this._onClose.push(cb); return this; }
    onState(cb) { if (typeof cb === "function") this._onState = cb; return this; }
    onStatus(cb) { if (typeof cb === "function") this._onStatus = cb; return this; }
    // Invoked at the start of every pump(); handy for polling a media pipeline's
    // bus alongside the WebRTC event loop.
    onPump(cb) { if (typeof cb === "function") this._onPump.push(cb); return this; }

    _status(level, message) {
      if (this._onStatus) { try { this._onStatus({ level: level, message: message }); } catch (e) {} }
      this.signal({ type: "status", level: level, message: message });
    }

    // Emit a status message to the remote (and any onStatus listener).
    notify(level, message) { this._status(level, message); return this; }

    addTrack(spec) {
      if (this.closed) throw new Error("MediaSession is closed");
      var s = spec || {};
      var track = this.pc.addTrack({
        media: s.media || "video",
        codec: s.codec || "VP8",
        payloadType: s.payloadType || 96,
        clockRate: s.clockRate || (s.media === "audio" ? 48000 : 90000),
        track: s.track || 0,
        sendPacketBufferName: s.sendPacketBufferName,
        mid: s.mid,          // undefined -> native default ("video"/"audio")
        autoOffer: false,    // exactly one explicit offer is emitted by start()
      });
      this.tracks.push(track);
      return track;
    }

    // Emit a single offer covering every added track.
    start() {
      if (this.started || this.closed) return this;
      if (this.tracks.length === 0) throw new Error("MediaSession.start() needs at least one track");
      this.started = true;
      this.pc.setLocalDescription("offer");
      this.pump();
      return this;
    }

    handleSignal(message) {
      if (this.closed || !message) return;
      var type = message.type;
      if (type === "answer") {
        if (message.description && message.description.sdp) this.pc.setRemoteDescription(message.description);
      } else if (type === "candidate") {
        if (message.candidate && message.candidate.candidate) this.pc.addIceCandidate(message.candidate);
      } else if (type === "stop") {
        this.close();
        return;
      }
      this.pump();
    }

    pump(options) {
      if (this.closed) return;
      var opts = options || {};
      var timeoutMs = opts.timeoutMs || 0;
      var maxEvents = opts.max || 64;
      for (var hook of this._onPump) { try { hook(this); } catch (e) {} }
      if (this.closed) return;
      for (var ev of this.pc.poll({ timeoutMs: timeoutMs, max: maxEvents })) {
        if (ev.type === "local-description") {
          if (ev.description && ev.description.type === "offer") {
            this.signal({ type: "offer", description: ev.description });
          }
        } else if (ev.type === "local-candidate") {
          if (ev.candidate && ev.candidate.candidate) this.signal({ type: "candidate", candidate: ev.candidate });
        } else if (ev.type === "state-change") {
          if (this._onState) { try { this._onState(ev); } catch (e) {} }
          this._sendState();
        } else if (ev.type === "error") {
          this._status("error", ev.message || "WebRTC error");
        }
      }
      var anyOpen = false;
      for (var t of this.tracks) {
        if (t.isOpen()) {
          anyOpen = true;
          var p = t.pump({ timeoutMs: 0, max: 128 });
          if (p && p.sent) { this.sentPackets += p.sent; this.sentBytes += p.bytes; }
        }
      }
      var when = nowMs();
      if (anyOpen && when - this._lastStateAt > 1000) { this._lastStateAt = when; this._sendState(); }
    }

    _sendState() {
      var state, stats;
      try { state = this.pc.state(); } catch (e) { return; }
      try { stats = this.pc.stats(); } catch (e) { stats = {}; }
      this.signal({
        type: "state",
        connection: state.connection, ice: state.ice,
        gathering: state.gathering, signaling: state.signaling,
        sentPackets: this.sentPackets, sentBytes: this.sentBytes,
        trackOpen: this.tracks.some(function (tr) { return tr.isOpen(); }),
        selectedCandidatePair: stats.selectedCandidatePair || null,
      });
    }

    stats() {
      var s = {};
      try { s = this.pc.stats(); } catch (e) {}
      return {
        sentPackets: this.sentPackets, sentBytes: this.sentBytes,
        trackOpen: this.tracks.some(function (tr) { return tr.isOpen(); }),
        selectedCandidatePair: s.selectedCandidatePair || null,
      };
    }

    close() {
      if (this.closed) return;
      this.closed = true;
      for (var t of this.tracks) { try { t.close(); } catch (e) {} }
      try { this.pc.close(); } catch (e) {}
      var cbs = this._onClose; this._onClose = [];
      for (var cb of cbs) { try { cb(this); } catch (e) {} }
    }
  }

  // Multiplexes many MediaSessions over one signaling transport (e.g. a wl2:http
  // WebSocket). Wire onMessage/onClose into the server's ws handlers. The client
  // sends {type:"hello", token} first; on success the hub replies {type:"welcome",
  // iceServers}. On {type:"start"} it builds a MediaSession and invokes onSession
  // so the app can attach tracks (and its pipeline) before the offer is sent.
  class SignalingHub {
    constructor(options) {
      var opts = options || {};
      this._authenticate = typeof opts.authenticate === "function" ? opts.authenticate : null;
      this._onSession = typeof opts.onSession === "function" ? opts.onSession : function () {};
      this._onError = typeof opts.onError === "function" ? opts.onError : null;
      this._iceServers = opts.iceServers || {};                 // native side (stunServer/turnServers)
      this._clientIceServers = opts.clientIceServers || [];     // browser RTCIceServer[]
      this._loopbackOnly = opts.loopbackOnly === true;
      this._receivePrefix = opts.receivePacketBufferNamePrefix;
      this._clients = new Map(); // conn.id -> { conn, authed, userId, session }
    }

    _record(conn) {
      var rec = this._clients.get(conn.id);
      if (!rec) { rec = { id: conn.id, conn: conn, authed: false, userId: null, session: null }; this._clients.set(conn.id, rec); }
      else rec.conn = conn; // refresh the per-callback conn object for later sends
      return rec;
    }

    _send(id, msg) {
      var rec = this._clients.get(id);
      if (!rec || !rec.conn) return;
      try { rec.conn.send(JSON.stringify(msg)); } catch (e) {}
    }

    onMessage(conn, text) {
      var rec = this._record(conn);
      var message;
      try { message = JSON.parse(text); } catch (e) { return; }
      var type = message && message.type;
      if (type === "hello") { this._authenticateClient(rec, message); return; }
      if (!rec.authed) { this._send(rec.id, { type: "error", message: "Send hello before signaling." }); return; }
      if (type === "start") { this._startClient(rec, message); }
      else if (rec.session) { try { rec.session.handleSignal(message); } catch (e) { this._send(rec.id, { type: "status", level: "error", message: (e && e.message) || String(e) }); } }
    }

    _authenticateClient(rec, message) {
      var userId = true;
      if (this._authenticate) {
        try { userId = this._authenticate(message, rec); } catch (e) { userId = false; }
      }
      if (userId) {
        rec.authed = true;
        rec.userId = userId === true ? null : userId;
        this._send(rec.id, { type: "welcome", userId: rec.userId, iceServers: this._clientIceServers });
      } else {
        rec.authed = false;
        this._send(rec.id, { type: "error", message: "Authentication failed." });
        try { rec.conn.close(); } catch (e) {}
      }
    }

    _startClient(rec, message) {
      if (rec.session) { try { rec.session.close(); } catch (e) {} rec.session = null; }
      var session = new MediaSession({
        iceServers: this._iceServers,
        loopbackOnly: this._loopbackOnly,
        receivePacketBufferNamePrefix: this._receivePrefix,
        signal: (function (hub, id) { return function (m) { hub._send(id, m); }; })(this, rec.id),
      });
      rec.session = session;
      try {
        this._onSession(session, { conn: rec.conn, userId: rec.userId, request: (message && message.request) || {} });
        session.start();
      } catch (e) {
        this._send(rec.id, { type: "status", level: "error", message: (e && e.message) || String(e) });
        try { session.close(); } catch (e2) {}
        rec.session = null;
        if (this._onError) { try { this._onError(e, rec); } catch (e2) {} }
      }
    }

    onClose(conn) {
      var rec = this._clients.get(conn.id);
      if (!rec) return;
      this._clients.delete(conn.id);
      if (rec.session) { try { rec.session.close(); } catch (e) {} }
    }

    close() {
      for (var rec of this._clients.values()) { if (rec.session) { try { rec.session.close(); } catch (e) {} } }
      this._clients.clear();
    }
  }

  return { MediaSession: MediaSession, SignalingHub: SignalingHub };
})
)WLRTC";

// Evaluate the JS helper factory and install its classes as module exports.
void install_webrtc_helpers(JSContext* ctx, JSModuleDef* module, JSValueConst nativeExports) {
    JSValue factory = JS_Eval(ctx, kWebRtcHelpersJs, std::strlen(kWebRtcHelpersJs),
        "wl2:webrtc/helpers.js", JS_EVAL_TYPE_GLOBAL);
    JSValue mediaSession = JS_UNDEFINED;
    JSValue signalingHub = JS_UNDEFINED;
    if (!JS_IsException(factory)) {
        JSValue helpers = JS_Call(ctx, factory, JS_UNDEFINED, 1, const_cast<JSValueConst*>(&nativeExports));
        if (!JS_IsException(helpers)) {
            mediaSession = JS_GetPropertyStr(ctx, helpers, "MediaSession");
            signalingHub = JS_GetPropertyStr(ctx, helpers, "SignalingHub");
        } else {
            JSValue exc = JS_GetException(ctx);
            const char* text = JS_ToCString(ctx, exc);
            fprintf(stderr, "wl2:webrtc helper init failed: %s\n", text ? text : "unknown");
            if (text) JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, exc);
        }
        JS_FreeValue(ctx, helpers);
    } else {
        JSValue exc = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, exc);
        fprintf(stderr, "wl2:webrtc helper compile failed: %s\n", text ? text : "unknown");
        if (text) JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, factory);
    JS_SetModuleExport(ctx, module, "MediaSession", mediaSession);
    JS_SetModuleExport(ctx, module, "SignalingHub", signalingHub);
}

int init_webrtc_module(JSContext* ctx, JSModuleDef* module) {
    register_webrtc_classes(ctx);
    JS_SetModuleExport(ctx, module, "version", JS_NewCFunction(ctx, webrtc_version_fn, "version", 0));
    JS_SetModuleExport(ctx, module, "capabilities", JS_NewCFunction(ctx, webrtc_capabilities_fn, "capabilities", 0));
    JSValue peerConnection = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, peerConnection, "create",
        JS_NewCFunction(ctx, webrtc_peerconnection_create_fn, "create", 1));
    JSValue webSocket = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, webSocket, "connect",
        JS_NewCFunction(ctx, webrtc_websocket_connect_fn, "connect", 1));
    JSValue signalingServer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, signalingServer, "listen",
        JS_NewCFunction(ctx, webrtc_signaling_server_listen_fn, "listen", 1));

    // Build the object handed to the JS helper factory before the values are
    // consumed by JS_SetModuleExport.
    JSValue nativeExports = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, nativeExports, "PeerConnection", JS_DupValue(ctx, peerConnection));
    JS_SetPropertyStr(ctx, nativeExports, "WebSocket", JS_DupValue(ctx, webSocket));
    JS_SetPropertyStr(ctx, nativeExports, "SignalingServer", JS_DupValue(ctx, signalingServer));

    JS_SetModuleExport(ctx, module, "PeerConnection", peerConnection);
    JS_SetModuleExport(ctx, module, "WebSocket", webSocket);
    JS_SetModuleExport(ctx, module, "SignalingServer", signalingServer);

    install_webrtc_helpers(ctx, module, nativeExports);
    JS_FreeValue(ctx, nativeExports);
    return 0;
}

constexpr const char* kExportNames[] = {"version", "capabilities", "PeerConnection", "WebSocket",
    "SignalingServer", "MediaSession", "SignalingHub"};

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
