#pragma once

#include "wl2/runtime.h"
#include "wl2/membus.h"
#include "wl2_webrtc/wl2_webrtc.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

#if WL2_HAVE_QUICKJS
#include <quickjs.h>
#endif

#ifndef WL2_VERSION
#define WL2_VERSION "0.0.0"
#endif
#ifndef WL2_BUILD
#define WL2_BUILD "0"
#endif
#ifndef WL2_WEBRTC_LIBVERSION
#define WL2_WEBRTC_LIBVERSION "unknown"
#endif
#ifndef WL2_WEBRTC_TLS_BACKEND
#define WL2_WEBRTC_TLS_BACKEND "openssl"
#endif
#ifndef WL2_WEBRTC_PROVIDER_USED
#define WL2_WEBRTC_PROVIDER_USED "unknown"
#endif

namespace wl2_webrtc {

extern const char* const WebRtcApi;

#if WL2_HAVE_QUICKJS

// --- Error contract + option helpers (shared module error shape) ------------
JSValue throw_webrtc_error(JSContext* ctx, const char* code, const char* operation,
    const std::string& message);
wl2::Runtime* current_runtime(JSContext* ctx);
bool option_string(JSContext* ctx, JSValueConst options, const char* key, std::string& out);
bool option_int(JSContext* ctx, JSValueConst options, const char* key, int64_t& out);
bool option_bool(JSContext* ctx, JSValueConst options, const char* key, bool fallback);

// --- Off-thread event marshaling --------------------------------------------
// libdatachannel invokes callbacks on its own threads. Callbacks convert their
// payload to one of these PODs and push it onto a bounded queue; JavaScript only
// ever reads through poll() on the JS thread.

struct DataChannelState {
    struct Message {
        bool binary = false;
        std::string data;
    };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Message> messages;
    std::atomic<bool> open{false};
    std::atomic<bool> closed{false};
    std::atomic<int64_t> dropped{0};
    size_t maxQueued = 1024;
};

struct TrackState {
    std::shared_ptr<rtc::Track> track;
    std::optional<wl2::PacketBuffer> sendBuffer;
    std::optional<wl2::PacketBuffer> recvBuffer;
    std::string sendPacketBufferName;
    std::string recvPacketBufferName;
    std::string media;
    std::string codec;
    int64_t payloadType = 96;
    int64_t clockRate = 90000;
    int64_t trackId = 0;
    int64_t lastSendSequence = 0;
    std::atomic<int64_t> sentPackets{0};
    std::atomic<int64_t> sentBytes{0};
    std::atomic<int64_t> receivedPackets{0};
    std::atomic<int64_t> receivedBytes{0};
    std::atomic<int64_t> droppedPackets{0};
    std::atomic<int64_t> sendErrors{0};
    std::atomic<bool> closed{false};
};

struct SessionEvent {
    enum class Kind { LocalDescription, LocalCandidate, StateChange, DataChannel, Track, Error };
    Kind kind;
    std::string a; // description type / candidate string / connection state / error message
    std::string b; // sdp / candidate mid
    std::string c; // ice state
    std::string d; // gathering state
    std::shared_ptr<rtc::DataChannel> channel;         // DataChannel events only
    std::shared_ptr<DataChannelState> channelState;    // DataChannel events only
    std::shared_ptr<rtc::Track> track;                 // Track events only
    std::shared_ptr<TrackState> trackState;            // Track events only
};

struct SessionState {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<SessionEvent> events;
    std::atomic<int64_t> dropped{0};
    size_t maxQueued = 4096;

    void push(SessionEvent&& event) {
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

// Bind message/open/closed callbacks that forward into a DataChannelState. Used
// for both locally created and inbound channels so no message is lost between
// arrival and JS wrapper creation.
void bind_data_channel(const std::shared_ptr<rtc::DataChannel>& dc,
    const std::shared_ptr<DataChannelState>& state);

extern JSClassID g_pcClassId;
extern JSClassID g_dcClassId;
extern JSClassID g_trackClassId;
extern JSClassID g_wsClassId;
extern JSClassID g_signalingServerClassId;

void register_webrtc_classes(JSContext* ctx);
void register_signaling_classes(JSContext* ctx);

JSValue webrtc_websocket_connect_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue webrtc_signaling_server_listen_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

JSValue webrtc_version_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue webrtc_capabilities_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);
JSValue webrtc_peerconnection_create_fn(JSContext* ctx, JSValueConst thisVal, int argc, JSValueConst* argv);

#endif // WL2_HAVE_QUICKJS

} // namespace wl2_webrtc
