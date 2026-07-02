#include "internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <variant>

#if WL2_HAVE_QUICKJS

namespace wl2_webrtc {

JSClassID g_pcClassId = 0;
JSClassID g_dcClassId = 0;
JSClassID g_trackClassId = 0;
JSClassID g_wsClassId = 0;
JSClassID g_signalingServerClassId = 0;

namespace {

std::atomic<int64_t> g_receiveBufferCounter{1};

// --- Boxes ------------------------------------------------------------------

struct PcBox {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<SessionState> state;
    std::string receivePacketBufferNamePrefix = "/wl2_webrtc_recv";
    std::vector<std::shared_ptr<TrackState>> tracks;
    bool closed = false;
};

struct DcBox {
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<DataChannelState> state;
};

struct TrackBox {
    std::shared_ptr<TrackState> state;
};

// --- Enum -> string ---------------------------------------------------------

const char* connection_state_name(rtc::PeerConnection::State s) {
    switch (s) {
        case rtc::PeerConnection::State::New: return "new";
        case rtc::PeerConnection::State::Connecting: return "connecting";
        case rtc::PeerConnection::State::Connected: return "connected";
        case rtc::PeerConnection::State::Disconnected: return "disconnected";
        case rtc::PeerConnection::State::Failed: return "failed";
        case rtc::PeerConnection::State::Closed: return "closed";
    }
    return "unknown";
}

const char* ice_state_name(rtc::PeerConnection::IceState s) {
    switch (s) {
        case rtc::PeerConnection::IceState::New: return "new";
        case rtc::PeerConnection::IceState::Checking: return "checking";
        case rtc::PeerConnection::IceState::Connected: return "connected";
        case rtc::PeerConnection::IceState::Completed: return "completed";
        case rtc::PeerConnection::IceState::Failed: return "failed";
        case rtc::PeerConnection::IceState::Disconnected: return "disconnected";
        case rtc::PeerConnection::IceState::Closed: return "closed";
    }
    return "unknown";
}

const char* gathering_state_name(rtc::PeerConnection::GatheringState s) {
    switch (s) {
        case rtc::PeerConnection::GatheringState::New: return "new";
        case rtc::PeerConnection::GatheringState::InProgress: return "in-progress";
        case rtc::PeerConnection::GatheringState::Complete: return "complete";
    }
    return "unknown";
}

const char* signaling_state_name(rtc::PeerConnection::SignalingState s) {
    switch (s) {
        case rtc::PeerConnection::SignalingState::Stable: return "stable";
        case rtc::PeerConnection::SignalingState::HaveLocalOffer: return "have-local-offer";
        case rtc::PeerConnection::SignalingState::HaveRemoteOffer: return "have-remote-offer";
        case rtc::PeerConnection::SignalingState::HaveLocalPranswer: return "have-local-pranswer";
        case rtc::PeerConnection::SignalingState::HaveRemotePranswer: return "have-remote-pranswer";
    }
    return "unknown";
}

// Snapshot every current state string into a StateChange event.
SessionEvent make_state_event(rtc::PeerConnection& pc) {
    SessionEvent e;
    e.kind = SessionEvent::Kind::StateChange;
    e.a = connection_state_name(pc.state());
    e.b = ice_state_name(pc.iceState());
    e.c = gathering_state_name(pc.gatheringState());
    e.d = signaling_state_name(pc.signalingState());
    return e;
}

// Extract raw bytes from a string, ArrayBuffer, or typed array view.
bool js_bytes(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsString(value)) {
        size_t len = 0;
        const char* text = JS_ToCStringLen(ctx, &len, value);
        if (!text) return false;
        out.assign(text, len);
        JS_FreeCString(ctx, text);
        return true;
    }
    size_t byteLength = 0;
    uint8_t* bytes = JS_GetArrayBuffer(ctx, &byteLength, value);
    if (bytes) {
        out.assign(reinterpret_cast<const char*>(bytes), byteLength);
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
        ok = true;
    }
    JS_FreeValue(ctx, ab);
    return ok;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0xf]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0xf]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string packet_metadata_json(const TrackState& state, int64_t pts) {
    std::ostringstream out;
    out << "{\"schema\":1"
        << ",\"codec\":\"" << json_escape(lower_ascii(state.codec)) << "\""
        << ",\"mediaType\":\"" << json_escape(state.media) << "\""
        << ",\"caps\":\"application/x-rtp,media=(string)" << json_escape(state.media)
        << ",encoding-name=(string)" << json_escape(state.codec)
        << ",payload=(int)" << state.payloadType
        << ",clock-rate=(int)" << state.clockRate << "\""
        << ",\"streamFormat\":\"rtp\""
        << ",\"alignment\":\"packet\""
        << ",\"track\":" << state.trackId
        << ",\"pts\":" << pts
        << ",\"dts\":0"
        << ",\"duration\":0"
        << ",\"timeBase\":\"1/" << state.clockRate << "\""
        << ",\"flags\":0"
        << ",\"discontinuity\":false"
        << ",\"sideData\":\"\"}";
    return out.str();
}

wl2::PacketKind packet_kind_for_media(const std::string& media) {
    if (media == "video") return wl2::PacketKind::Video;
    if (media == "audio") return wl2::PacketKind::Audio;
    return wl2::PacketKind::Data;
}

JSValue throw_policy_error(JSContext* ctx, const char* operation, const wl2::Error& error) {
    return throw_webrtc_error(ctx, "webrtc_permission_denied", operation, error.message());
}

bool authorize_shared_memory_name(JSContext* ctx, const char* operation, const std::string& name) {
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_webrtc_error(ctx, "webrtc_permission_denied", operation,
            "Shared-memory access is not permitted without a runtime policy");
        return false;
    }
    if (auto ok = runtime->authorizeSharedMemory(name); !ok) {
        throw_policy_error(ctx, operation, ok.error());
        return false;
    }
    return true;
}

rtc::Description::Media make_media_description(const std::string& media, const std::string& codec,
    int payloadType, int clockRate) {
    const std::string c = lower_ascii(codec);
    if (media == "audio") {
        rtc::Description::Audio desc("audio", rtc::Description::Direction::SendOnly);
        if (c == "opus") desc.addOpusCodec(payloadType);
        else if (c == "pcma") desc.addPCMACodec(payloadType);
        else if (c == "pcmu") desc.addPCMUCodec(payloadType);
        else desc.addAudioCodec(payloadType, codec);
        (void)clockRate;
        return desc;
    }

    rtc::Description::Video desc("video", rtc::Description::Direction::SendOnly);
    if (c == "h264") desc.addH264Codec(payloadType);
    else if (c == "h265" || c == "hevc") desc.addH265Codec(payloadType);
    else if (c == "vp8") desc.addVP8Codec(payloadType);
    else if (c == "vp9") desc.addVP9Codec(payloadType);
    else if (c == "av1") desc.addAV1Codec(payloadType);
    else desc.addVideoCodec(payloadType, codec);
    (void)clockRate;
    return desc;
}

// Parse a STUN/TURN URL enough to authorize its host/port before use.
bool parse_ice_endpoint(const std::string& url, std::string& host, uint16_t& port) {
    std::string s = url;
    for (const char* scheme : {"stuns:", "stun:", "turns:", "turn:"}) {
        if (s.rfind(scheme, 0) == 0) { s = s.substr(std::strlen(scheme)); break; }
    }
    if (s.rfind("//", 0) == 0) s = s.substr(2);
    if (auto at = s.find('@'); at != std::string::npos) s = s.substr(at + 1); // drop userinfo
    if (auto slash = s.find('/'); slash != std::string::npos) s = s.substr(0, slash);
    port = 3478;
    if (auto colon = s.rfind(':'); colon != std::string::npos) {
        host = s.substr(0, colon);
        try { port = static_cast<uint16_t>(std::stoi(s.substr(colon + 1))); } catch (...) {}
    } else {
        host = s;
    }
    return !host.empty();
}

bool authorize_ice(JSContext* ctx, const std::string& url) {
    std::string host;
    uint16_t port = 0;
    if (!parse_ice_endpoint(url, host, port)) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", "PeerConnection.create",
            "Could not parse ICE server URL: " + url);
        return false;
    }
    wl2::Runtime* runtime = current_runtime(ctx);
    if (!runtime) {
        throw_webrtc_error(ctx, "webrtc_permission_denied", "PeerConnection.create", "Runtime is unavailable");
        return false;
    }
    if (auto ok = runtime->authorizeNetworkConnect(host, port); !ok) {
        throw_webrtc_error(ctx, "webrtc_permission_denied", "PeerConnection.create",
            "Network access denied for ICE server " + host + ":" + std::to_string(port));
        return false;
    }
    return true;
}

// --- DataChannel object -----------------------------------------------------

DcBox* live_dc(JSContext* ctx, JSValueConst thisVal) {
    auto* box = static_cast<DcBox*>(JS_GetOpaque2(ctx, thisVal, g_dcClassId));
    if (!box || !box->dc) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", "DataChannel", "Not a live DataChannel");
        return nullptr;
    }
    return box;
}

void dc_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<DcBox*>(JS_GetOpaque(value, g_dcClassId));
    if (box) {
        if (box->dc) {
            try { box->dc->close(); } catch (...) {}
        }
        delete box;
    }
}

JSValue new_data_channel_object(JSContext* ctx, std::shared_ptr<rtc::DataChannel> dc,
    std::shared_ptr<DataChannelState> state) {
    JSValue obj = JS_NewObjectClass(ctx, g_dcClassId);
    if (JS_IsException(obj)) return obj;
    auto* box = new DcBox{std::move(dc), std::move(state)};
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue dc_send(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    DcBox* box = live_dc(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    if (argc < 1) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "DataChannel.send", "send(data) requires data");
    }
    std::string bytes;
    if (!js_bytes(ctx, argv[0], bytes)) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "DataChannel.send",
            "send(data) requires a string or bytes");
    }
    bool queued = false;
    try {
        if (JS_IsString(argv[0])) {
            queued = box->dc->send(bytes);
        } else {
            queued = box->dc->send(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        }
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "DataChannel.send", e.what());
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
    JS_SetPropertyStr(ctx, obj, "buffered", JS_NewBool(ctx, !queued));
    return obj;
}

JSValue dc_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    DcBox* box = live_dc(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    int64_t timeoutMs = 0;
    int64_t maxMessages = 256;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxMessages);
    }
    auto& st = *box->state;
    std::unique_lock<std::mutex> lock(st.mutex);
    if (st.messages.empty() && timeoutMs > 0) {
        st.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !st.messages.empty(); });
    }
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    while (!st.messages.empty() && index < static_cast<uint32_t>(std::max<int64_t>(0, maxMessages))) {
        DataChannelState::Message m = std::move(st.messages.front());
        st.messages.pop_front();
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "binary", JS_NewBool(ctx, m.binary));
        if (m.binary) {
            JS_SetPropertyStr(ctx, obj, "data", JS_NewArrayBufferCopy(ctx,
                reinterpret_cast<const uint8_t*>(m.data.data()), m.data.size()));
        } else {
            JS_SetPropertyStr(ctx, obj, "data", JS_NewStringLen(ctx, m.data.data(), m.data.size()));
        }
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

JSValue dc_label(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    DcBox* box = live_dc(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    return JS_NewString(ctx, box->dc->label().c_str());
}

JSValue dc_is_open(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    DcBox* box = live_dc(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    return JS_NewBool(ctx, box->dc->isOpen());
}

JSValue dc_buffered_amount(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    DcBox* box = live_dc(ctx, thisVal);
    if (!box) return JS_EXCEPTION;
    return JS_NewInt64(ctx, static_cast<int64_t>(box->dc->bufferedAmount()));
}

JSValue dc_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<DcBox*>(JS_GetOpaque2(ctx, thisVal, g_dcClassId));
    if (box && box->dc) {
        try { box->dc->close(); } catch (...) {}
    }
    return JS_UNDEFINED;
}

void register_dc_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (g_dcClassId == 0) JS_NewClassID(&g_dcClassId);
    JSClassDef def{};
    def.class_name = "DataChannel";
    def.finalizer = dc_finalizer;
    JS_NewClass(rt, g_dcClassId, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "send", JS_NewCFunction(ctx, dc_send, "send", 1));
    JS_SetPropertyStr(ctx, proto, "poll", JS_NewCFunction(ctx, dc_poll, "poll", 1));
    JS_SetPropertyStr(ctx, proto, "label", JS_NewCFunction(ctx, dc_label, "label", 0));
    JS_SetPropertyStr(ctx, proto, "isOpen", JS_NewCFunction(ctx, dc_is_open, "isOpen", 0));
    JS_SetPropertyStr(ctx, proto, "bufferedAmount", JS_NewCFunction(ctx, dc_buffered_amount, "bufferedAmount", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, dc_close, "close", 0));
    JS_SetClassProto(ctx, g_dcClassId, proto);
}

// --- PeerConnection object --------------------------------------------------

PcBox* live_pc(JSContext* ctx, JSValueConst thisVal, const char* op) {
    auto* box = static_cast<PcBox*>(JS_GetOpaque2(ctx, thisVal, g_pcClassId));
    if (!box) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", op, "Not a PeerConnection");
        return nullptr;
    }
    if (box->closed || !box->pc) {
        throw_webrtc_error(ctx, "webrtc_closed", op, "PeerConnection is closed");
        return nullptr;
    }
    return box;
}

void reset_pc_callbacks(rtc::PeerConnection& pc) {
    pc.onLocalDescription(nullptr);
    pc.onLocalCandidate(nullptr);
    pc.onStateChange(nullptr);
    pc.onIceStateChange(nullptr);
    pc.onGatheringStateChange(nullptr);
    pc.onSignalingStateChange(nullptr);
    pc.onDataChannel(nullptr);
    pc.onTrack(nullptr);
}

void pc_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<PcBox*>(JS_GetOpaque(value, g_pcClassId));
    if (box) {
        if (box->pc) {
            try { reset_pc_callbacks(*box->pc); box->pc->close(); } catch (...) {}
        }
        for (auto& track : box->tracks) {
            if (track && track->track) {
                try { track->track->resetCallbacks(); track->track->close(); } catch (...) {}
            }
        }
        delete box;
    }
}

JSValue pc_create_data_channel(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PcBox* box = live_pc(ctx, thisVal, "createDataChannel");
    if (!box) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "createDataChannel",
            "createDataChannel(label) requires a label string");
    }
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    std::string label = text;
    JS_FreeCString(ctx, text);
    std::shared_ptr<rtc::DataChannel> dc;
    try {
        dc = box->pc->createDataChannel(label);
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "createDataChannel", e.what());
    }
    auto dcState = std::make_shared<DataChannelState>();
    bind_data_channel(dc, dcState);
    return new_data_channel_object(ctx, std::move(dc), std::move(dcState));
}

// --- Track object -----------------------------------------------------------

TrackBox* live_track(JSContext* ctx, JSValueConst thisVal, const char* op) {
    auto* box = static_cast<TrackBox*>(JS_GetOpaque2(ctx, thisVal, g_trackClassId));
    if (!box || !box->state || !box->state->track) {
        throw_webrtc_error(ctx, "webrtc_invalid_argument", op, "Not a live Track");
        return nullptr;
    }
    return box;
}

void track_finalizer(JSRuntime*, JSValue value) {
    auto* box = static_cast<TrackBox*>(JS_GetOpaque(value, g_trackClassId));
    if (box) {
        if (box->state && box->state->track) {
            try { box->state->track->resetCallbacks(); box->state->track->close(); } catch (...) {}
        }
        delete box;
    }
}

JSValue new_track_object(JSContext* ctx, std::shared_ptr<TrackState> state) {
    JSValue obj = JS_NewObjectClass(ctx, g_trackClassId);
    if (JS_IsException(obj)) return obj;
    auto* box = new TrackBox{std::move(state)};
    JS_SetOpaque(obj, box);
    return obj;
}

JSValue track_pump(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    TrackBox* box = live_track(ctx, thisVal, "Track.pump");
    if (!box) return JS_EXCEPTION;
    auto& st = *box->state;
    int64_t timeoutMs = 0;
    int64_t maxPackets = 64;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxPackets);
    }
    if (!st.sendBuffer) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "Track.pump",
            "Track has no sendPacketBufferName");
    }
    if (timeoutMs > 0 && !st.sendBuffer->waitForPacket(std::chrono::milliseconds(timeoutMs), st.lastSendSequence)) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "sent", JS_NewInt64(ctx, 0));
        JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, 0));
        return obj;
    }

    std::vector<wl2::PacketRecord> records;
    const int64_t buffers = st.sendBuffer->buffers();
    for (int64_t i = 0; i < buffers; ++i) {
        auto rec = st.sendBuffer->record(i);
        if (rec && rec.value().sequence > st.lastSendSequence) {
            records.push_back(std::move(rec.value()));
        }
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return a.sequence < b.sequence;
    });

    int64_t sent = 0;
    int64_t bytes = 0;
    for (const auto& rec : records) {
        if (sent >= std::max<int64_t>(0, maxPackets)) break;
        try {
            st.track->send(reinterpret_cast<const std::byte*>(rec.payload.data()), rec.payload.size());
            st.lastSendSequence = std::max(st.lastSendSequence, rec.sequence);
            ++sent;
            bytes += static_cast<int64_t>(rec.payload.size());
        } catch (...) {
            st.sendErrors.fetch_add(1);
        }
    }
    st.sentPackets.fetch_add(sent);
    st.sentBytes.fetch_add(bytes);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "sent", JS_NewInt64(ctx, sent));
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, bytes));
    JS_SetPropertyStr(ctx, obj, "lastSequence", JS_NewInt64(ctx, st.lastSendSequence));
    return obj;
}

JSValue track_stats(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    TrackBox* box = live_track(ctx, thisVal, "Track.stats");
    if (!box) return JS_EXCEPTION;
    auto& st = *box->state;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "sentPackets", JS_NewInt64(ctx, st.sentPackets.load()));
    JS_SetPropertyStr(ctx, obj, "sentBytes", JS_NewInt64(ctx, st.sentBytes.load()));
    JS_SetPropertyStr(ctx, obj, "receivedPackets", JS_NewInt64(ctx, st.receivedPackets.load()));
    JS_SetPropertyStr(ctx, obj, "receivedBytes", JS_NewInt64(ctx, st.receivedBytes.load()));
    JS_SetPropertyStr(ctx, obj, "droppedPackets", JS_NewInt64(ctx, st.droppedPackets.load()));
    JS_SetPropertyStr(ctx, obj, "sendErrors", JS_NewInt64(ctx, st.sendErrors.load()));
    JS_SetPropertyStr(ctx, obj, "closed", JS_NewBool(ctx, st.closed.load()));
    return obj;
}

JSValue track_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<TrackBox*>(JS_GetOpaque2(ctx, thisVal, g_trackClassId));
    if (box && box->state && box->state->track && !box->state->closed.exchange(true)) {
        try { box->state->track->resetCallbacks(); box->state->track->close(); } catch (...) {}
    }
    return JS_UNDEFINED;
}

JSValue track_is_open(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    TrackBox* box = live_track(ctx, thisVal, "Track.isOpen");
    if (!box) return JS_EXCEPTION;
    return JS_NewBool(ctx, box->state->track->isOpen());
}

JSValue track_mid(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    TrackBox* box = live_track(ctx, thisVal, "Track.mid");
    if (!box) return JS_EXCEPTION;
    return JS_NewString(ctx, box->state->track->mid().c_str());
}

JSValue track_packet_buffer_name(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    TrackBox* box = live_track(ctx, thisVal, "Track.packetBufferName");
    if (!box) return JS_EXCEPTION;
    const std::string& name = box->state->recvPacketBufferName.empty()
        ? box->state->sendPacketBufferName
        : box->state->recvPacketBufferName;
    return JS_NewString(ctx, name.c_str());
}

void register_track_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (g_trackClassId == 0) JS_NewClassID(&g_trackClassId);
    JSClassDef def{};
    def.class_name = "Track";
    def.finalizer = track_finalizer;
    JS_NewClass(rt, g_trackClassId, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "pump", JS_NewCFunction(ctx, track_pump, "pump", 1));
    JS_SetPropertyStr(ctx, proto, "stats", JS_NewCFunction(ctx, track_stats, "stats", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, track_close, "close", 0));
    JS_SetPropertyStr(ctx, proto, "isOpen", JS_NewCFunction(ctx, track_is_open, "isOpen", 0));
    JS_SetPropertyStr(ctx, proto, "mid", JS_NewCFunction(ctx, track_mid, "mid", 0));
    JS_SetPropertyStr(ctx, proto, "packetBufferName", JS_NewCFunction(ctx, track_packet_buffer_name, "packetBufferName", 0));
    JS_SetClassProto(ctx, g_trackClassId, proto);
}

JSValue pc_add_track(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PcBox* box = live_pc(ctx, thisVal, "addTrack");
    if (!box) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "addTrack",
            "addTrack(options) requires media, codec, payloadType, and sendPacketBufferName");
    }
    std::string media, codec, bufferName;
    int64_t payloadType = 96;
    int64_t clockRate = 90000;
    int64_t trackId = 0;
    if (!option_string(ctx, argv[0], "media", media) || !option_string(ctx, argv[0], "codec", codec)
        || !option_string(ctx, argv[0], "sendPacketBufferName", bufferName)) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "addTrack",
            "addTrack(options) requires media, codec, and sendPacketBufferName strings");
    }
    media = lower_ascii(media);
    if (media != "video" && media != "audio") {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "addTrack",
            "media must be 'video' or 'audio'");
    }
    option_int(ctx, argv[0], "payloadType", payloadType);
    option_int(ctx, argv[0], "clockRate", clockRate);
    option_int(ctx, argv[0], "track", trackId);
    if (payloadType < 0 || payloadType > 127) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "addTrack", "payloadType must be 0..127");
    }
    if (clockRate <= 0) {
        clockRate = media == "audio" ? 48000 : 90000;
    }
    if (!authorize_shared_memory_name(ctx, "addTrack", bufferName)) return JS_EXCEPTION;
    auto opened = wl2::PacketBuffer::openExisting(bufferName);
    if (!opened) {
        return throw_webrtc_error(ctx, "webrtc_failed", "addTrack", opened.error().message());
    }

    std::shared_ptr<rtc::Track> track;
    try {
        auto desc = make_media_description(media, codec, static_cast<int>(payloadType), static_cast<int>(clockRate));
        track = box->pc->addTrack(std::move(desc));
        if (box->pc->signalingState() == rtc::PeerConnection::SignalingState::Stable) {
            box->pc->setLocalDescription(rtc::Description::Type::Offer);
        }
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "addTrack", e.what());
    }
    auto st = std::make_shared<TrackState>();
    st->track = track;
    st->sendBuffer = std::move(opened.value());
    st->sendPacketBufferName = bufferName;
    st->media = media;
    st->codec = codec;
    st->payloadType = payloadType;
    st->clockRate = clockRate;
    st->trackId = trackId;
    box->tracks.push_back(st);
    return new_track_object(ctx, st);
}

JSValue pc_set_remote_description(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PcBox* box = live_pc(ctx, thisVal, "setRemoteDescription");
    if (!box) return JS_EXCEPTION;
    std::string type, sdp;
    if (argc < 1 || !option_string(ctx, argv[0], "sdp", sdp) || sdp.empty()) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "setRemoteDescription",
            "setRemoteDescription({ type, sdp }) requires an sdp string");
    }
    option_string(ctx, argv[0], "type", type);
    try {
        box->pc->setRemoteDescription(type.empty() ? rtc::Description(sdp) : rtc::Description(sdp, type));
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "setRemoteDescription", e.what());
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
    return obj;
}

JSValue pc_add_ice_candidate(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PcBox* box = live_pc(ctx, thisVal, "addIceCandidate");
    if (!box) return JS_EXCEPTION;
    std::string candidate, mid;
    if (argc < 1 || !option_string(ctx, argv[0], "candidate", candidate) || candidate.empty()) {
        return throw_webrtc_error(ctx, "webrtc_invalid_argument", "addIceCandidate",
            "addIceCandidate({ candidate, sdpMid? }) requires a candidate string");
    }
    option_string(ctx, argv[0], "sdpMid", mid);
    try {
        box->pc->addRemoteCandidate(mid.empty() ? rtc::Candidate(candidate) : rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "addIceCandidate", e.what());
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, true));
    return obj;
}

JSValue pc_local_description(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PcBox* box = live_pc(ctx, thisVal, "localDescription");
    if (!box) return JS_EXCEPTION;
    auto desc = box->pc->localDescription();
    if (!desc) return JS_NULL;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, desc->typeString().c_str()));
    JS_SetPropertyStr(ctx, obj, "sdp", JS_NewString(ctx, std::string(*desc).c_str()));
    return obj;
}

JSValue pc_state(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    PcBox* box = live_pc(ctx, thisVal, "state");
    if (!box) return JS_EXCEPTION;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "connection", JS_NewString(ctx, connection_state_name(box->pc->state())));
    JS_SetPropertyStr(ctx, obj, "ice", JS_NewString(ctx, ice_state_name(box->pc->iceState())));
    JS_SetPropertyStr(ctx, obj, "gathering", JS_NewString(ctx, gathering_state_name(box->pc->gatheringState())));
    JS_SetPropertyStr(ctx, obj, "signaling", JS_NewString(ctx, signaling_state_name(box->pc->signalingState())));
    return obj;
}

JSValue pc_poll(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv) {
    PcBox* box = live_pc(ctx, thisVal, "poll");
    if (!box) return JS_EXCEPTION;
    int64_t timeoutMs = 0;
    int64_t maxEvents = 128;
    if (argc > 0) {
        option_int(ctx, argv[0], "timeoutMs", timeoutMs);
        option_int(ctx, argv[0], "max", maxEvents);
    }
    auto& st = *box->state;
    std::deque<SessionEvent> drained;
    {
        std::unique_lock<std::mutex> lock(st.mutex);
        if (st.events.empty() && timeoutMs > 0) {
            st.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return !st.events.empty(); });
        }
        while (!st.events.empty() && static_cast<int64_t>(drained.size()) < std::max<int64_t>(0, maxEvents)) {
            drained.push_back(std::move(st.events.front()));
            st.events.pop_front();
        }
    }
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (auto& e : drained) {
        JSValue obj = JS_NewObject(ctx);
        switch (e.kind) {
            case SessionEvent::Kind::LocalDescription: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "local-description"));
                JSValue desc = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, desc, "type", JS_NewString(ctx, e.a.c_str()));
                JS_SetPropertyStr(ctx, desc, "sdp", JS_NewString(ctx, e.b.c_str()));
                JS_SetPropertyStr(ctx, obj, "description", desc);
                break;
            }
            case SessionEvent::Kind::LocalCandidate: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "local-candidate"));
                JSValue cand = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, cand, "candidate", JS_NewString(ctx, e.a.c_str()));
                JS_SetPropertyStr(ctx, cand, "sdpMid", JS_NewString(ctx, e.b.c_str()));
                JS_SetPropertyStr(ctx, obj, "candidate", cand);
                break;
            }
            case SessionEvent::Kind::StateChange: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "state-change"));
                JS_SetPropertyStr(ctx, obj, "connection", JS_NewString(ctx, e.a.c_str()));
                JS_SetPropertyStr(ctx, obj, "ice", JS_NewString(ctx, e.b.c_str()));
                JS_SetPropertyStr(ctx, obj, "gathering", JS_NewString(ctx, e.c.c_str()));
                JS_SetPropertyStr(ctx, obj, "signaling", JS_NewString(ctx, e.d.c_str()));
                break;
            }
            case SessionEvent::Kind::DataChannel: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "data-channel"));
                JS_SetPropertyStr(ctx, obj, "label", JS_NewString(ctx, e.a.c_str()));
                JS_SetPropertyStr(ctx, obj, "channel",
                    new_data_channel_object(ctx, e.channel, e.channelState));
                break;
            }
            case SessionEvent::Kind::Track: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "track"));
                JS_SetPropertyStr(ctx, obj, "media", JS_NewString(ctx, e.a.c_str()));
                JS_SetPropertyStr(ctx, obj, "packetBufferName", JS_NewString(ctx, e.b.c_str()));
                JS_SetPropertyStr(ctx, obj, "track",
                    new_track_object(ctx, e.trackState));
                break;
            }
            case SessionEvent::Kind::Error: {
                JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, "error"));
                JS_SetPropertyStr(ctx, obj, "message", JS_NewString(ctx, e.a.c_str()));
                break;
            }
        }
        JS_SetPropertyUint32(ctx, array, index++, obj);
    }
    return array;
}

JSValue pc_stats(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<PcBox*>(JS_GetOpaque2(ctx, thisVal, g_pcClassId));
    JSValue obj = JS_NewObject(ctx);
    if (box && box->state) {
        std::lock_guard<std::mutex> lock(box->state->mutex);
        JS_SetPropertyStr(ctx, obj, "pendingEvents", JS_NewInt64(ctx, static_cast<int64_t>(box->state->events.size())));
        JS_SetPropertyStr(ctx, obj, "droppedEvents", JS_NewInt64(ctx, box->state->dropped.load()));
    }
    if (box && box->pc) {
        try {
            JS_SetPropertyStr(ctx, obj, "bytesSent", JS_NewInt64(ctx, static_cast<int64_t>(box->pc->bytesSent())));
            JS_SetPropertyStr(ctx, obj, "bytesReceived", JS_NewInt64(ctx, static_cast<int64_t>(box->pc->bytesReceived())));
            if (auto rtt = box->pc->rtt()) {
                JS_SetPropertyStr(ctx, obj, "rttMs", JS_NewInt64(ctx, rtt->count()));
            } else {
                JS_SetPropertyStr(ctx, obj, "rttMs", JS_NULL);
            }
            rtc::Candidate local;
            rtc::Candidate remote;
            if (box->pc->getSelectedCandidatePair(&local, &remote)) {
                JSValue pair = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, pair, "local", JS_NewString(ctx, local.candidate().c_str()));
                JS_SetPropertyStr(ctx, pair, "remote", JS_NewString(ctx, remote.candidate().c_str()));
                if (auto address = local.address()) JS_SetPropertyStr(ctx, pair, "localAddress", JS_NewString(ctx, address->c_str()));
                if (auto port = local.port()) JS_SetPropertyStr(ctx, pair, "localPort", JS_NewInt32(ctx, *port));
                if (auto address = remote.address()) JS_SetPropertyStr(ctx, pair, "remoteAddress", JS_NewString(ctx, address->c_str()));
                if (auto port = remote.port()) JS_SetPropertyStr(ctx, pair, "remotePort", JS_NewInt32(ctx, *port));
                JS_SetPropertyStr(ctx, obj, "selectedCandidatePair", pair);
            } else {
                JS_SetPropertyStr(ctx, obj, "selectedCandidatePair", JS_NULL);
            }
        } catch (...) {
            JS_SetPropertyStr(ctx, obj, "bytesSent", JS_NULL);
            JS_SetPropertyStr(ctx, obj, "bytesReceived", JS_NULL);
            JS_SetPropertyStr(ctx, obj, "rttMs", JS_NULL);
            JS_SetPropertyStr(ctx, obj, "selectedCandidatePair", JS_NULL);
        }
    }
    JS_SetPropertyStr(ctx, obj, "closed", JS_NewBool(ctx, !box || box->closed));
    return obj;
}

JSValue pc_close(JSContext* ctx, JSValueConst thisVal, int, JSValueConst*) {
    auto* box = static_cast<PcBox*>(JS_GetOpaque2(ctx, thisVal, g_pcClassId));
    if (box && box->pc && !box->closed) {
        try { reset_pc_callbacks(*box->pc); box->pc->close(); } catch (...) {}
        for (auto& track : box->tracks) {
            if (track && track->track) {
                try { track->track->resetCallbacks(); track->track->close(); } catch (...) {}
                track->closed = true;
            }
        }
        box->closed = true;
    }
    return JS_UNDEFINED;
}

void register_pc_class(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (g_pcClassId == 0) JS_NewClassID(&g_pcClassId);
    JSClassDef def{};
    def.class_name = "PeerConnection";
    def.finalizer = pc_finalizer;
    JS_NewClass(rt, g_pcClassId, &def);
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "createDataChannel", JS_NewCFunction(ctx, pc_create_data_channel, "createDataChannel", 2));
    JS_SetPropertyStr(ctx, proto, "addTrack", JS_NewCFunction(ctx, pc_add_track, "addTrack", 1));
    JS_SetPropertyStr(ctx, proto, "setRemoteDescription", JS_NewCFunction(ctx, pc_set_remote_description, "setRemoteDescription", 1));
    JS_SetPropertyStr(ctx, proto, "addIceCandidate", JS_NewCFunction(ctx, pc_add_ice_candidate, "addIceCandidate", 1));
    JS_SetPropertyStr(ctx, proto, "localDescription", JS_NewCFunction(ctx, pc_local_description, "localDescription", 0));
    JS_SetPropertyStr(ctx, proto, "state", JS_NewCFunction(ctx, pc_state, "state", 0));
    JS_SetPropertyStr(ctx, proto, "poll", JS_NewCFunction(ctx, pc_poll, "poll", 1));
    JS_SetPropertyStr(ctx, proto, "stats", JS_NewCFunction(ctx, pc_stats, "stats", 0));
    JS_SetPropertyStr(ctx, proto, "close", JS_NewCFunction(ctx, pc_close, "close", 0));
    JS_SetClassProto(ctx, g_pcClassId, proto);
}

} // namespace

void bind_data_channel(const std::shared_ptr<rtc::DataChannel>& dc,
    const std::shared_ptr<DataChannelState>& state) {
    std::weak_ptr<DataChannelState> weak = state;
    dc->onOpen([weak] { if (auto s = weak.lock()) { s->open = true; s->cv.notify_all(); } });
    dc->onClosed([weak] { if (auto s = weak.lock()) { s->open = false; s->closed = true; s->cv.notify_all(); } });
    dc->onMessage([weak](rtc::message_variant data) {
        auto s = weak.lock();
        if (!s) return;
        DataChannelState::Message m;
        std::visit([&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, rtc::binary>) {
                m.binary = true;
                m.data.assign(reinterpret_cast<const char*>(arg.data()), arg.size());
            } else {
                m.binary = false;
                m.data.assign(arg.begin(), arg.end());
            }
        }, data);
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            if (s->messages.size() >= s->maxQueued) { s->messages.pop_front(); s->dropped.fetch_add(1); }
            s->messages.push_back(std::move(m));
        }
        s->cv.notify_all();
    });
    if (dc->isOpen()) state->open = true;
}

void register_webrtc_classes(JSContext* ctx) {
    register_pc_class(ctx);
    register_dc_class(ctx);
    register_track_class(ctx);
    register_signaling_classes(ctx);
}

// PeerConnection.create(options?) -> PeerConnection
JSValue webrtc_peerconnection_create_fn(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    JSValueConst options = argc > 0 ? argv[0] : JS_UNDEFINED;
    const bool loopbackOnly = option_bool(ctx, options, "loopbackOnly", false);
    std::string receivePrefix = "/wl2_webrtc_recv";
    option_string(ctx, options, "receivePacketBufferNamePrefix", receivePrefix);

    rtc::Configuration config;
    if (loopbackOnly) {
        // Host candidates on loopback only: no STUN/TURN reach, deterministic
        // for tests and locked-down deployments.
        config.bindAddress = "127.0.0.1";
    } else {
        std::string stun;
        if (option_string(ctx, options, "stunServer", stun) && !stun.empty()) {
            if (!authorize_ice(ctx, stun)) return JS_EXCEPTION;
            config.iceServers.emplace_back(stun);
        }
        JSValue turns = JS_IsObject(options) ? JS_GetPropertyStr(ctx, options, "turnServers") : JS_UNDEFINED;
        if (JS_IsArray(ctx, turns)) {
            JSValue lenVal = JS_GetPropertyStr(ctx, turns, "length");
            uint32_t len = 0;
            JS_ToUint32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            for (uint32_t i = 0; i < len; ++i) {
                JSValue item = JS_GetPropertyUint32(ctx, turns, i);
                const char* url = JS_ToCString(ctx, item);
                JS_FreeValue(ctx, item);
                if (url) {
                    std::string u = url;
                    JS_FreeCString(ctx, url);
                    if (!authorize_ice(ctx, u)) { JS_FreeValue(ctx, turns); return JS_EXCEPTION; }
                    config.iceServers.emplace_back(u);
                }
            }
        }
        JS_FreeValue(ctx, turns);
    }
    std::string policy;
    if (option_string(ctx, options, "iceTransportPolicy", policy) && policy == "relay") {
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }

    auto state = std::make_shared<SessionState>();
    std::shared_ptr<rtc::PeerConnection> pc;
    try {
        pc = std::make_shared<rtc::PeerConnection>(config);
    } catch (const std::exception& e) {
        return throw_webrtc_error(ctx, "webrtc_failed", "PeerConnection.create", e.what());
    }

    std::weak_ptr<rtc::PeerConnection> weakPc = pc;
    pc->onLocalDescription([state](rtc::Description desc) {
        SessionEvent e;
        e.kind = SessionEvent::Kind::LocalDescription;
        e.a = desc.typeString();
        e.b = std::string(desc);
        state->push(std::move(e));
    });
    pc->onLocalCandidate([state](rtc::Candidate cand) {
        SessionEvent e;
        e.kind = SessionEvent::Kind::LocalCandidate;
        e.a = std::string(cand);
        e.b = cand.mid();
        state->push(std::move(e));
    });
    auto emitState = [state, weakPc] {
        if (auto p = weakPc.lock()) state->push(make_state_event(*p));
    };
    pc->onStateChange([emitState](rtc::PeerConnection::State) { emitState(); });
    pc->onIceStateChange([emitState](rtc::PeerConnection::IceState) { emitState(); });
    pc->onGatheringStateChange([emitState](rtc::PeerConnection::GatheringState) { emitState(); });
    pc->onDataChannel([state](std::shared_ptr<rtc::DataChannel> dc) {
        auto dcState = std::make_shared<DataChannelState>();
        bind_data_channel(dc, dcState);
        SessionEvent e;
        e.kind = SessionEvent::Kind::DataChannel;
        e.a = dc->label();
        e.channel = std::move(dc);
        e.channelState = std::move(dcState);
        state->push(std::move(e));
    });
    pc->onTrack([state, receivePrefix, runtime = current_runtime(ctx)](std::shared_ptr<rtc::Track> track) {
        auto trackState = std::make_shared<TrackState>();
        trackState->track = track;
        const auto desc = track->description();
        trackState->media = desc.type();
        trackState->codec = "rtp";
        if (!desc.payloadTypes().empty()) {
            const int pt = desc.payloadTypes().front();
            trackState->payloadType = pt;
            if (const auto* map = desc.rtpMap(pt)) {
                trackState->codec = map->format;
                trackState->clockRate = map->clockRate;
            }
        }
        trackState->trackId = 0;
        trackState->recvPacketBufferName = receivePrefix + "_" + track->mid() + "_"
            + std::to_string(g_receiveBufferCounter.fetch_add(1));

        if (!runtime) {
            SessionEvent e;
            e.kind = SessionEvent::Kind::Error;
            e.a = "Shared-memory access is not permitted without a runtime policy";
            state->push(std::move(e));
            return;
        }
        if (auto ok = runtime->authorizeSharedMemory(trackState->recvPacketBufferName); !ok) {
            SessionEvent e;
            e.kind = SessionEvent::Kind::Error;
            e.a = ok.error().message();
            state->push(std::move(e));
            return;
        }
        auto created = wl2::PacketBuffer::create(trackState->recvPacketBufferName,
            64, 1024 * 1024, 65536, 0, 0,
            packet_metadata_json(*trackState, 0));
        if (!created) {
            SessionEvent e;
            e.kind = SessionEvent::Kind::Error;
            e.a = created.error().message();
            state->push(std::move(e));
            return;
        }
        trackState->recvBuffer = std::move(created.value());
        std::weak_ptr<TrackState> weak = trackState;
        track->onMessage([weak](rtc::message_variant data) {
            auto st = weak.lock();
            if (!st || !st->recvBuffer) return;
            std::string bytes;
            bool binary = false;
            std::visit([&](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, rtc::binary>) {
                    binary = true;
                    bytes.assign(reinterpret_cast<const char*>(arg.data()), arg.size());
                } else {
                    bytes.assign(arg.begin(), arg.end());
                }
            }, data);
            if (!binary) {
                st->droppedPackets.fetch_add(1);
                return;
            }
            const int64_t pts = st->receivedPackets.load();
            auto written = st->recvBuffer->write(bytes, packet_kind_for_media(st->media),
                st->trackId, pts, packet_metadata_json(*st, pts));
            if (!written) {
                st->droppedPackets.fetch_add(1);
                return;
            }
            st->receivedPackets.fetch_add(1);
            st->receivedBytes.fetch_add(static_cast<int64_t>(bytes.size()));
        });
        SessionEvent e;
        e.kind = SessionEvent::Kind::Track;
        e.a = trackState->media;
        e.b = trackState->recvPacketBufferName;
        e.track = track;
        e.trackState = std::move(trackState);
        state->push(std::move(e));
    });

    JSValue obj = JS_NewObjectClass(ctx, g_pcClassId);
    if (JS_IsException(obj)) {
        try { reset_pc_callbacks(*pc); pc->close(); } catch (...) {}
        return obj;
    }
    auto* box = new PcBox{std::move(pc), std::move(state), receivePrefix, {}, false};
    JS_SetOpaque(obj, box);
    return obj;
}

} // namespace wl2_webrtc

#endif // WL2_HAVE_QUICKJS
