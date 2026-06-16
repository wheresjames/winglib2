#include "wl2/membus.h"

#if WL2_HAVE_LIBMEMBUS
#include "libmembus.h"
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

#ifndef WL2_LIBMEMBUS_HAS_1_2_SURFACE
#define WL2_LIBMEMBUS_HAS_1_2_SURFACE 0
#endif

namespace wl2 {

namespace {

Error unavailable_error() {
    return Error("libmembus_unavailable", "Winglib2 was built without libmembus support");
}

Error open_error(std::string_view type, std::string_view name) {
    return Error("libmembus_open_failed", "Unable to open " + std::string(type) + " " + std::string(name));
}

Error surface_error(std::string_view type) {
    return Error("libmembus_v1_2_unavailable", std::string(type) + " requires libmembus v1.2.0 surface");
}

bool key_value_schema_is_aligned(int64_t maxNameLength, int64_t maxValueLength) {
    if (maxNameLength <= 0 || maxValueLength <= 0) {
        return false;
    }
    const int64_t variableBytes = (maxNameLength + 1) + (maxValueLength + 1);
    return variableBytes % static_cast<int64_t>(sizeof(int64_t)) == 0;
}

uint64_t timeout_ms(std::chrono::milliseconds timeout) {
    return static_cast<uint64_t>(std::max<int64_t>(0, timeout.count()));
}

#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
mmb::video_format to_native(VideoPixelFormat format) {
    return static_cast<mmb::video_format>(static_cast<int64_t>(format));
}

VideoPixelFormat from_native(mmb::video_format format) {
    return static_cast<VideoPixelFormat>(static_cast<int64_t>(format));
}

mmb::audio_format to_native(AudioSampleFormat format) {
    return static_cast<mmb::audio_format>(static_cast<int64_t>(format));
}

AudioSampleFormat from_native(mmb::audio_format format) {
    return static_cast<AudioSampleFormat>(static_cast<int64_t>(format));
}

AudioSampleFormat format_from_bits(int64_t bitsPerSample) {
    switch (bitsPerSample) {
    case 8:
        return AudioSampleFormat::U8;
    case 16:
        return AudioSampleFormat::S16Le;
    case 24:
        return AudioSampleFormat::S24Le;
    case 32:
        return AudioSampleFormat::S32Le;
    default:
        return AudioSampleFormat::S16Le;
    }
}
#endif

} // namespace

bool libmembusHasV12Surface() noexcept {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return true;
#else
    return false;
#endif
}

#if WL2_HAVE_LIBMEMBUS
struct SharedBuffer::Impl {
    mmb::memmap map;
};

struct SharedQueue::Impl {
    mmb::memmsg queue;
};

struct VideoBuffer::Impl {
    mmb::memvid video;
};

struct AudioBuffer::Impl {
    mmb::memaud audio;
};

#if WL2_LIBMEMBUS_HAS_1_2_SURFACE
struct CommandChannel::Impl {
    mmb::memcmd channel;
};

struct KeyValueStore::Impl {
    mmb::memkv store;
};
#else
struct CommandChannel::Impl {};
struct KeyValueStore::Impl {};
#endif
#else
struct SharedBuffer::Impl {};
struct SharedQueue::Impl {};
struct VideoBuffer::Impl {};
struct AudioBuffer::Impl {};
struct CommandChannel::Impl {};
struct KeyValueStore::Impl {};
#endif

Result<SharedBuffer> SharedBuffer::create(std::string name, size_t size, bool replaceExisting) {
#if WL2_HAVE_LIBMEMBUS
    SharedBuffer buffer{name, size, true};
    buffer.impl_ = std::make_shared<Impl>();
    if (!buffer.impl_->map.open(name, static_cast<int64_t>(size), true, replaceExisting)) {
        buffer.impl_.reset();
        return open_error("shared buffer", name);
    }
    buffer.size_ = static_cast<size_t>(buffer.impl_->map.size());
    return buffer;
#else
    (void)name;
    (void)size;
    (void)replaceExisting;
    return unavailable_error();
#endif
}

Result<SharedBuffer> SharedBuffer::attach(std::string name, size_t size) {
#if WL2_HAVE_LIBMEMBUS
    SharedBuffer buffer{name, size, false};
    buffer.impl_ = std::make_shared<Impl>();
    if (!buffer.impl_->map.open(name, static_cast<int64_t>(size), false, false)) {
        buffer.impl_.reset();
        return open_error("shared buffer", name);
    }
    buffer.size_ = static_cast<size_t>(buffer.impl_->map.size());
    return buffer;
#else
    (void)name;
    (void)size;
    return unavailable_error();
#endif
}

bool SharedBuffer::isOpen() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->map.isOpen();
#else
    return false;
#endif
}

bool SharedBuffer::existing() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->map.existing();
#else
    return false;
#endif
}

void SharedBuffer::close() {
#if WL2_HAVE_LIBMEMBUS
    if (impl_) {
        impl_->map.close();
    }
#endif
    impl_.reset();
}

Result<size_t> SharedBuffer::write(std::string_view bytes) {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "SharedBuffer is not open");
    }
    std::string copy(bytes);
    auto written = impl_->map.write(copy);
    return static_cast<size_t>(std::max<int64_t>(0, written));
#else
    (void)bytes;
    return unavailable_error();
#endif
}

Result<std::string> SharedBuffer::read(size_t maxBytes) const {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "SharedBuffer is not open");
    }
    const int64_t requested = maxBytes == AllBytes ? -1 : static_cast<int64_t>(maxBytes);
    return impl_->map.read(requested);
#else
    (void)maxBytes;
    return unavailable_error();
#endif
}

char* SharedBuffer::data() {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->map.data() : nullptr;
#else
    return nullptr;
#endif
}

const char* SharedBuffer::data() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->map.data() : nullptr;
#else
    return nullptr;
#endif
}

Result<SharedQueue> SharedQueue::create(std::string name, size_t size, bool writable) {
#if WL2_HAVE_LIBMEMBUS
    SharedQueue queue;
    queue.name_ = name;
    queue.size_ = size;
    queue.writable_ = writable;
    queue.impl_ = std::make_shared<Impl>();
    if (!queue.impl_->queue.open(name, static_cast<int64_t>(size), writable, true)) {
        queue.impl_.reset();
        return open_error("shared queue", name);
    }
    return queue;
#else
    (void)name;
    (void)size;
    (void)writable;
    return unavailable_error();
#endif
}

Result<SharedQueue> SharedQueue::attach(std::string name, size_t size, bool writable) {
#if WL2_HAVE_LIBMEMBUS
    SharedQueue queue;
    queue.name_ = name;
    queue.size_ = size;
    queue.writable_ = writable;
    queue.impl_ = std::make_shared<Impl>();
    if (!queue.impl_->queue.open(name, static_cast<int64_t>(size), writable, false)) {
        queue.impl_.reset();
        return open_error("shared queue", name);
    }
    return queue;
#else
    (void)name;
    (void)size;
    (void)writable;
    return unavailable_error();
#endif
}

bool SharedQueue::isOpen() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ != nullptr;
#else
    return false;
#endif
}

bool SharedQueue::existing() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->queue.existing();
#else
    return false;
#endif
}

void SharedQueue::close() {
#if WL2_HAVE_LIBMEMBUS
    if (impl_) {
        impl_->queue.close();
    }
#endif
    impl_.reset();
}

Result<void> SharedQueue::write(std::string_view message) {
#if WL2_HAVE_LIBMEMBUS
    if (!impl_) {
        return Error("libmembus_not_open", "SharedQueue is not open");
    }
    if (!impl_->queue.write(std::string(message))) {
        return Error("libmembus_write_failed", "Unable to write message to SharedQueue");
    }
    return {};
#else
    (void)message;
    return unavailable_error();
#endif
}

Result<std::string> SharedQueue::read(std::chrono::milliseconds timeout) {
#if WL2_HAVE_LIBMEMBUS
    if (!impl_) {
        return Error("libmembus_not_open", "SharedQueue is not open");
    }
    return impl_->queue.read(timeout_ms(timeout));
#else
    (void)timeout;
    return unavailable_error();
#endif
}

Result<MembusReadResult> SharedQueue::readWithStatus(std::chrono::milliseconds timeout) {
#if WL2_HAVE_LIBMEMBUS
    if (!impl_) {
        return Error("libmembus_not_open", "SharedQueue is not open");
    }
#if WL2_LIBMEMBUS_HAS_1_2_SURFACE
    bool overrun = false;
    auto payload = impl_->queue.read(timeout_ms(timeout), &overrun);
    return MembusReadResult{std::move(payload), overrun};
#else
    return MembusReadResult{impl_->queue.read(timeout_ms(timeout)), false};
#endif
#else
    (void)timeout;
    return unavailable_error();
#endif
}

bool SharedQueue::poll() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->queue.poll();
#else
    return false;
#endif
}

int64_t SharedQueue::sessionId() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ ? impl_->queue.getSessionId() : 0;
#else
    return 0;
#endif
}

Result<VideoBuffer> VideoBuffer::create(std::string name, int64_t width, int64_t height, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return create(std::move(name), width, height, VideoPixelFormat::Rgb24, fps, buffers);
#else
#if WL2_HAVE_LIBMEMBUS
    VideoBuffer video;
    video.name_ = name;
    video.impl_ = std::make_shared<Impl>();
    if (!video.impl_->video.open(name, true, width, height, 24, fps, buffers)) {
        video.impl_.reset();
        return open_error("video buffer", name);
    }
    return video;
#else
    (void)name;
    (void)width;
    (void)height;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
#endif
}

Result<VideoBuffer> VideoBuffer::create(std::string name, int64_t width, int64_t height, VideoPixelFormat format, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    VideoBuffer video;
    video.name_ = name;
    video.impl_ = std::make_shared<Impl>();
    if (!video.impl_->video.open(name, true, width, height, to_native(format), fps, buffers)) {
        video.impl_.reset();
        return open_error("video buffer", name);
    }
    return video;
#elif WL2_HAVE_LIBMEMBUS
    if (format != VideoPixelFormat::Rgb24) {
        return surface_error("non-RGB24 VideoBuffer formats");
    }
    return create(std::move(name), width, height, fps, buffers);
#else
    (void)name;
    (void)width;
    (void)height;
    (void)format;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
}

Result<VideoBuffer> VideoBuffer::attach(std::string name, int64_t width, int64_t height, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS
    VideoBuffer video;
    video.name_ = name;
    video.impl_ = std::make_shared<Impl>();
#if WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!video.impl_->video.open(name, false, width, height, mmb::video_format::rgb24, fps, buffers)) {
#else
    if (!video.impl_->video.open(name, false, width, height, 24, fps, buffers)) {
#endif
        video.impl_.reset();
        return open_error("video buffer", name);
    }
    return video;
#else
    (void)name;
    (void)width;
    (void)height;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
}

Result<VideoBuffer> VideoBuffer::openExisting(std::string name) {
#if WL2_HAVE_LIBMEMBUS
    VideoBuffer video;
    video.name_ = name;
    video.impl_ = std::make_shared<Impl>();
    if (!video.impl_->video.open_existing(name)) {
        video.impl_.reset();
        return open_error("video buffer", name);
    }
    return video;
#else
    (void)name;
    return unavailable_error();
#endif
}

void VideoBuffer::close() {
#if WL2_HAVE_LIBMEMBUS
    if (impl_) {
        impl_->video.close();
    }
#endif
    impl_.reset();
}

bool VideoBuffer::isOpen() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->video.isOpen();
#else
    return false;
#endif
}

bool VideoBuffer::existing() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->video.existing();
#else
    return false;
#endif
}

Result<VideoFrameView> VideoBuffer::frame(int64_t index) {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "VideoBuffer is not open");
    }
    try {
        auto view = impl_->video.getBuf(index);
        return VideoFrameView{view.m_ptr, static_cast<size_t>(view.m_size), view.m_sw, view.m_w, view.m_h};
    } catch (...) {
        return Error("libmembus_view_failed", "Unable to get VideoBuffer frame");
    }
#else
    (void)index;
    return unavailable_error();
#endif
}

Result<void> VideoBuffer::fill(int64_t index, int value) {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "VideoBuffer is not open");
    }
    if (!impl_->video.fill(index, value)) {
        return Error("libmembus_fill_failed", "Unable to fill VideoBuffer frame");
    }
    return {};
#else
    (void)index;
    (void)value;
    return unavailable_error();
#endif
}

int64_t VideoBuffer::width() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getWidth() : -1;
#else
    return -1;
#endif
}

int64_t VideoBuffer::height() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getHeight() : -1;
#else
    return -1;
#endif
}

int64_t VideoBuffer::bitsPerPixel() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return bytesPerPixel() * 8;
#elif WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getBpp() : -1;
#else
    return -1;
#endif
}

int64_t VideoBuffer::bytesPerPixel() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? mmb::video_format_bytes_per_pixel(impl_->video.getFormat()) : 0;
#elif WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getBpp() / 8 : 0;
#else
    return 0;
#endif
}

std::optional<VideoPixelFormat> VideoBuffer::format() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return std::nullopt;
    }
    return from_native(impl_->video.getFormat());
#else
    return std::nullopt;
#endif
}

std::string VideoBuffer::formatName() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->video.getFormatName() : "";
#else
    return "";
#endif
}

int64_t VideoBuffer::fps() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getFps() : -1;
#else
    return -1;
#endif
}

int64_t VideoBuffer::buffers() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getBufs() : 0;
#else
    return 0;
#endif
}

int64_t VideoBuffer::pointer(int64_t offset) const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.getPtr(offset) : -1;
#else
    (void)offset;
    return -1;
#endif
}

int64_t VideoBuffer::setPointer(int64_t pointer) {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.setPtr(pointer) : -1;
#else
    (void)pointer;
    return -1;
#endif
}

int64_t VideoBuffer::next(int64_t increment) {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->video.next(increment) : -1;
#else
    (void)increment;
    return -1;
#endif
}

int64_t VideoBuffer::sessionId() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->video.getSessionId() : 0;
#else
    return 0;
#endif
}

int64_t VideoBuffer::sequence() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->video.getSeq() : -1;
#else
    return -1;
#endif
}

int64_t VideoBuffer::frameSequence(int64_t index) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->video.getFrameSeq(index) : -1;
#else
    (void)index;
    return -1;
#endif
}

bool VideoBuffer::waitForFrame(std::chrono::milliseconds timeout, int64_t lastSequence) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() && impl_->video.waitForFrame(timeout_ms(timeout), lastSequence);
#else
    (void)timeout;
    (void)lastSequence;
    return false;
#endif
}

Result<AudioBuffer> AudioBuffer::create(std::string name, int64_t channels, int64_t bitsPerSample, int64_t sampleRate, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return create(std::move(name), channels, format_from_bits(bitsPerSample), sampleRate, fps, buffers);
#else
#if WL2_HAVE_LIBMEMBUS
    AudioBuffer audio;
    audio.name_ = name;
    audio.impl_ = std::make_shared<Impl>();
    if (!audio.impl_->audio.open(name, true, channels, bitsPerSample, sampleRate, fps, buffers)) {
        audio.impl_.reset();
        return open_error("audio buffer", name);
    }
    return audio;
#else
    (void)name;
    (void)channels;
    (void)bitsPerSample;
    (void)sampleRate;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
#endif
}

Result<AudioBuffer> AudioBuffer::create(std::string name, int64_t channels, AudioSampleFormat format, int64_t sampleRate, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    AudioBuffer audio;
    audio.name_ = name;
    audio.impl_ = std::make_shared<Impl>();
    if (!audio.impl_->audio.open(name, true, channels, to_native(format), sampleRate, fps, buffers)) {
        audio.impl_.reset();
        return open_error("audio buffer", name);
    }
    return audio;
#elif WL2_HAVE_LIBMEMBUS
    if (format != AudioSampleFormat::U8 && format != AudioSampleFormat::S16Le) {
        return surface_error("non-8/16-bit AudioBuffer formats");
    }
    const int64_t bits = format == AudioSampleFormat::U8 ? 8 : 16;
    return create(std::move(name), channels, bits, sampleRate, fps, buffers);
#else
    (void)name;
    (void)channels;
    (void)format;
    (void)sampleRate;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
}

Result<AudioBuffer> AudioBuffer::attach(std::string name, int64_t channels, int64_t bitsPerSample, int64_t sampleRate, int64_t fps, int64_t buffers) {
#if WL2_HAVE_LIBMEMBUS
    AudioBuffer audio;
    audio.name_ = name;
    audio.impl_ = std::make_shared<Impl>();
#if WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!audio.impl_->audio.open(name, false, channels, to_native(format_from_bits(bitsPerSample)), sampleRate, fps, buffers)) {
#else
    if (!audio.impl_->audio.open(name, false, channels, bitsPerSample, sampleRate, fps, buffers)) {
#endif
        audio.impl_.reset();
        return open_error("audio buffer", name);
    }
    return audio;
#else
    (void)name;
    (void)channels;
    (void)bitsPerSample;
    (void)sampleRate;
    (void)fps;
    (void)buffers;
    return unavailable_error();
#endif
}

Result<AudioBuffer> AudioBuffer::openExisting(std::string name) {
#if WL2_HAVE_LIBMEMBUS
    AudioBuffer audio;
    audio.name_ = name;
    audio.impl_ = std::make_shared<Impl>();
    if (!audio.impl_->audio.open_existing(name)) {
        audio.impl_.reset();
        return open_error("audio buffer", name);
    }
    return audio;
#else
    (void)name;
    return unavailable_error();
#endif
}

void AudioBuffer::close() {
#if WL2_HAVE_LIBMEMBUS
    if (impl_) {
        impl_->audio.close();
    }
#endif
    impl_.reset();
}

bool AudioBuffer::isOpen() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->audio.isOpen();
#else
    return false;
#endif
}

bool AudioBuffer::existing() const {
#if WL2_HAVE_LIBMEMBUS
    return impl_ && impl_->audio.existing();
#else
    return false;
#endif
}

Result<AudioBufferView> AudioBuffer::buffer(int64_t index) {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "AudioBuffer is not open");
    }
    try {
        auto view = impl_->audio.getBuf(index);
#if WL2_LIBMEMBUS_HAS_1_2_SURFACE
        return AudioBufferView{view.m_ptr, static_cast<size_t>(view.m_size), view.m_ch, mmb::audio_format_bytes_per_sample(view.m_format) * 8};
#else
        return AudioBufferView{view.m_ptr, static_cast<size_t>(view.m_size), view.m_ch, view.m_bps};
#endif
    } catch (...) {
        return Error("libmembus_view_failed", "Unable to get AudioBuffer view");
    }
#else
    (void)index;
    return unavailable_error();
#endif
}

Result<void> AudioBuffer::fill(int64_t index, int value) {
#if WL2_HAVE_LIBMEMBUS
    if (!isOpen()) {
        return Error("libmembus_not_open", "AudioBuffer is not open");
    }
    if (!impl_->audio.fill(index, value)) {
        return Error("libmembus_fill_failed", "Unable to fill AudioBuffer");
    }
    return {};
#else
    (void)index;
    (void)value;
    return unavailable_error();
#endif
}

int64_t AudioBuffer::channels() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getChannels() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::bitsPerSample() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return bytesPerSample() * 8;
#elif WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getBps() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::bytesPerSample() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getBytesPerSample() : 0;
#elif WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getBps() / 8 : 0;
#else
    return 0;
#endif
}

std::optional<AudioSampleFormat> AudioBuffer::format() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return std::nullopt;
    }
    return from_native(impl_->audio.getFormat());
#else
    return std::nullopt;
#endif
}

std::string AudioBuffer::formatName() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getFormatName() : "";
#else
    return "";
#endif
}

int64_t AudioBuffer::sampleRate() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getSampleRate() : 0;
#elif WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getBitRate() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::fps() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getFps() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::buffers() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getBufs() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::bufferSize() const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getBufSize() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::pointer(int64_t offset) const {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.getPtr(offset) : -1;
#else
    (void)offset;
    return -1;
#endif
}

int64_t AudioBuffer::setPointer(int64_t pointer) {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.setPtr(pointer) : -1;
#else
    (void)pointer;
    return -1;
#endif
}

int64_t AudioBuffer::next(int64_t increment) {
#if WL2_HAVE_LIBMEMBUS
    return isOpen() ? impl_->audio.next(increment) : -1;
#else
    (void)increment;
    return -1;
#endif
}

int64_t AudioBuffer::sessionId() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getSessionId() : 0;
#else
    return 0;
#endif
}

int64_t AudioBuffer::sequence() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getSeq() : -1;
#else
    return -1;
#endif
}

int64_t AudioBuffer::frameSequence(int64_t index) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->audio.getFrameSeq(index) : -1;
#else
    (void)index;
    return -1;
#endif
}

bool AudioBuffer::waitForFrame(std::chrono::milliseconds timeout, int64_t lastSequence) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() && impl_->audio.waitForFrame(timeout_ms(timeout), lastSequence);
#else
    (void)timeout;
    (void)lastSequence;
    return false;
#endif
}

Result<CommandChannel> CommandChannel::create(std::string name, size_t size, bool reader) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    CommandChannel channel;
    channel.name_ = name;
    channel.size_ = size;
    channel.reader_ = reader;
    channel.impl_ = std::make_shared<Impl>();
    if (!channel.impl_->channel.open(name, static_cast<int64_t>(size), reader, true)) {
        channel.impl_.reset();
        return open_error("command channel", name);
    }
    return channel;
#else
    (void)name;
    (void)size;
    (void)reader;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("CommandChannel");
#else
    return unavailable_error();
#endif
#endif
}

Result<CommandChannel> CommandChannel::attach(std::string name, size_t size, bool reader) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    CommandChannel channel;
    channel.name_ = name;
    channel.size_ = size;
    channel.reader_ = reader;
    channel.impl_ = std::make_shared<Impl>();
    if (!channel.impl_->channel.open(name, static_cast<int64_t>(size), reader, false)) {
        channel.impl_.reset();
        return open_error("command channel", name);
    }
    return channel;
#else
    (void)name;
    (void)size;
    (void)reader;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("CommandChannel");
#else
    return unavailable_error();
#endif
#endif
}

bool CommandChannel::isOpen() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->channel.isOpen();
#else
    return false;
#endif
}

bool CommandChannel::existing() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->channel.existing();
#else
    return false;
#endif
}

void CommandChannel::close() {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (impl_) {
        impl_->channel.close();
    }
#endif
    impl_.reset();
}

Result<void> CommandChannel::write(std::string_view command) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "CommandChannel is not open");
    }
    if (!impl_->channel.write(std::string(command))) {
        return Error("libmembus_write_failed", "Unable to write CommandChannel command");
    }
    return {};
#else
    (void)command;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("CommandChannel");
#else
    return unavailable_error();
#endif
#endif
}

Result<MembusReadResult> CommandChannel::read(std::chrono::milliseconds timeout) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "CommandChannel is not open");
    }
    bool overrun = false;
    auto payload = impl_->channel.read(timeout_ms(timeout), &overrun);
    return MembusReadResult{std::move(payload), overrun};
#else
    (void)timeout;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("CommandChannel");
#else
    return unavailable_error();
#endif
#endif
}

bool CommandChannel::poll() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->channel.poll();
#else
    return false;
#endif
}

int64_t CommandChannel::readerCount() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ ? impl_->channel.readerCount() : 0;
#else
    return 0;
#endif
}

int64_t CommandChannel::sessionId() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ ? impl_->channel.getSessionId() : 0;
#else
    return 0;
#endif
}

Result<KeyValueStore> KeyValueStore::create(std::string name, int64_t count, int64_t maxNameLength, int64_t maxValueLength, bool replaceExisting) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (count <= 0 || !key_value_schema_is_aligned(maxNameLength, maxValueLength)) {
        return Error("libmembus_invalid_schema",
            "KeyValueStore name/value limits must be positive and produce 8-byte aligned slots; "
            "lengths such as 15/31 are safe");
    }
    KeyValueStore store;
    store.name_ = name;
    store.impl_ = std::make_shared<Impl>();
    if (!store.impl_->store.create(name, count, maxNameLength, maxValueLength, replaceExisting)) {
        store.impl_.reset();
        return open_error("key-value store", name);
    }
    return store;
#else
    (void)name;
    (void)count;
    (void)maxNameLength;
    (void)maxValueLength;
    (void)replaceExisting;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<KeyValueStore> KeyValueStore::open(std::string name) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    KeyValueStore store;
    store.name_ = name;
    store.impl_ = std::make_shared<Impl>();
    if (!store.impl_->store.open(name)) {
        store.impl_.reset();
        return open_error("key-value store", name);
    }
    return store;
#else
    (void)name;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

bool KeyValueStore::isOpen() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->store.isOpen();
#else
    return false;
#endif
}

bool KeyValueStore::existing() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return impl_ && impl_->store.existing();
#else
    return false;
#endif
}

void KeyValueStore::close() {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (impl_) {
        impl_->store.close();
    }
#endif
    impl_.reset();
}

Result<void> KeyValueStore::setName(int64_t index, std::string_view name) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    if (!impl_->store.setName(index, std::string(name))) {
        return Error("libmembus_write_failed", "Unable to set KeyValueStore name");
    }
    return {};
#else
    (void)index;
    (void)name;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<void> KeyValueStore::setValue(int64_t index, std::string_view value) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    if (!impl_->store.setValue(index, std::string(value))) {
        return Error("libmembus_write_failed", "Unable to set KeyValueStore value");
    }
    return {};
#else
    (void)index;
    (void)value;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<void> KeyValueStore::setValue(std::string_view name, std::string_view value) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    if (!impl_->store.setValue(std::string(name), std::string(value))) {
        return Error("libmembus_write_failed", "Unable to set KeyValueStore value");
    }
    return {};
#else
    (void)name;
    (void)value;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<void> KeyValueStore::setAll(const Map& values) {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    if (!impl_->store.setAll(values)) {
        return Error("libmembus_write_failed", "Unable to set KeyValueStore values");
    }
    return {};
#else
    (void)values;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<std::string> KeyValueStore::value(int64_t index) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.getValue(index);
#else
    (void)index;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<std::string> KeyValueStore::value(std::string_view name) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.getValue(std::string(name));
#else
    (void)name;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<KeyValueStore::Map> KeyValueStore::all() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.getAll();
#else
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<KeyValueStore::Map> KeyValueStore::changed(int64_t& epoch) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.getChanged(epoch);
#else
    (void)epoch;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<KeyValueStore::Map> KeyValueStore::waitChanged(std::chrono::milliseconds timeout, int64_t& epoch) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.getChanged(timeout_ms(timeout), epoch);
#else
    (void)timeout;
    (void)epoch;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

Result<bool> KeyValueStore::waitForChange(std::chrono::milliseconds timeout, int64_t& epoch) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    if (!isOpen()) {
        return Error("libmembus_not_open", "KeyValueStore is not open");
    }
    return impl_->store.waitForChange(timeout_ms(timeout), epoch);
#else
    (void)timeout;
    (void)epoch;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("KeyValueStore");
#else
    return unavailable_error();
#endif
#endif
}

int64_t KeyValueStore::epoch() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.getEpoch() : -1;
#else
    return -1;
#endif
}

int64_t KeyValueStore::sessionId() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.getSessionId() : 0;
#else
    return 0;
#endif
}

int64_t KeyValueStore::findName(std::string_view name) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.findName(std::string(name)) : -1;
#else
    (void)name;
    return -1;
#endif
}

std::string KeyValueStore::slotName(int64_t index) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.getName(index) : "";
#else
    (void)index;
    return "";
#endif
}

int64_t KeyValueStore::count() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.count() : 0;
#else
    return 0;
#endif
}

int64_t KeyValueStore::maxNameLength() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.maxNameLen() : 0;
#else
    return 0;
#endif
}

int64_t KeyValueStore::maxValueLength() const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return isOpen() ? impl_->store.maxValueLen() : 0;
#else
    return 0;
#endif
}

void MembusSelector::add(Condition condition) {
    conditions_.push_back(std::move(condition));
}

void MembusSelector::clear() {
    conditions_.clear();
}

Result<int> MembusSelector::wait(std::chrono::milliseconds timeout) const {
#if WL2_HAVE_LIBMEMBUS && WL2_LIBMEMBUS_HAS_1_2_SURFACE
    return mmb::select(timeout_ms(timeout), conditions_);
#else
    (void)timeout;
#if WL2_HAVE_LIBMEMBUS
    return surface_error("MembusSelector");
#else
    return unavailable_error();
#endif
#endif
}

} // namespace wl2
