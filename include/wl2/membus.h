#pragma once

/**
 * @file membus.h
 * @brief First-class C++ wrappers for libmembus shared-memory primitives.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/buffer.h"
#include "wl2/errors.h"

namespace wl2 {

/**
 * @brief Whether the configured libmembus dependency exposes the v1.2.0 API surface.
 *
 * This reports the compile-time surface selected by CMake. When false,
 * v1.2-only wrappers such as CommandChannel, KeyValueStore, and MembusSelector
 * return `libmembus_unavailable` style errors instead of trying to call APIs
 * that are not present in the selected libmembus checkout.
 */
bool libmembusHasV12Surface() noexcept;

/**
 * @brief Result of an overrun-aware queue or command-channel read.
 */
struct MembusReadResult {
    /// Payload bytes read from the source. Empty may mean timeout or overrun.
    std::string payload;
    /// True when the reader was lapped and resynchronized before returning.
    bool overrun = false;
};

/**
 * @brief Single-producer/single-consumer shared-memory message queue.
 *
 * SharedQueue wraps `mmb::memmsg` without exposing libmembus types to callers.
 * One process should open the writer side and another should open the reader
 * side with the same name and size.
 *
 * @code{.cpp}
 * auto tx = wl2::SharedQueue::create("/wl2_queue", 4096, true);
 * auto rx = wl2::SharedQueue::attach("/wl2_queue", 4096, false);
 *
 * tx.value().write("hello");
 * auto msg = rx.value().read(std::chrono::milliseconds{100});
 * @endcode
 */
class SharedQueue {
public:
    SharedQueue() = default;

    /**
     * @brief Create a shared-memory queue endpoint.
     * @param name Queue name. POSIX names should start with `/`.
     * @param size Queue payload capacity in bytes.
     * @param writable True for the producer endpoint, false for the consumer.
     * @return Open queue endpoint on success, or an Error.
     */
    static Result<SharedQueue> create(std::string name, size_t size, bool writable);

    /**
     * @brief Attach to an existing shared-memory queue endpoint.
     * @param name Queue name.
     * @param size Queue payload capacity in bytes.
     * @param writable True for the producer endpoint, false for the consumer.
     * @return Open queue endpoint on success, or an Error.
     */
    static Result<SharedQueue> attach(std::string name, size_t size, bool writable);

    /**
     * @brief Queue name.
     * @return Stored queue name.
     */
    const std::string& name() const noexcept { return name_; }

    /**
     * @brief Queue payload capacity in bytes.
     * @return Configured queue size.
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief Whether this endpoint may write messages.
     * @return True for producer endpoints.
     */
    bool writable() const noexcept { return writable_; }

    /**
     * @brief Check whether the queue endpoint is open.
     * @return True when attached to shared memory.
     */
    bool isOpen() const;

    /**
     * @brief Check whether the backing shared memory already existed.
     * @return True when open found an existing object.
     */
    bool existing() const;

    /// Close the queue endpoint.
    void close();

    /**
     * @brief Write a message to the queue.
     * @param message Message bytes to enqueue. Empty messages are rejected.
     * @return Successful Result on write, or an Error.
     */
    Result<void> write(std::string_view message);

    /**
     * @brief Read a message from the queue.
     * @param timeout Maximum time to wait for a message.
     * @return Message bytes, an empty string on timeout, or an Error.
     */
    Result<std::string> read(std::chrono::milliseconds timeout);

    /**
     * @brief Read a message and expose libmembus v1.2 overrun status.
     * @param timeout Maximum time to wait for a message.
     * @return Payload plus overrun flag, or an Error.
     */
    Result<MembusReadResult> readWithStatus(std::chrono::milliseconds timeout);

    /**
     * @brief Non-blocking readiness check.
     * @return True when at least one message appears ready for this reader.
     */
    bool poll() const;

    /**
     * @brief Creator session identifier.
     * @return Non-zero session id when supported and open, otherwise zero.
     */
    int64_t sessionId() const;

private:
    struct Impl;

    std::string name_;
    size_t size_ = 0;
    bool writable_ = false;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Video pixel formats supported by libmembus v1.2.0.
 */
enum class VideoPixelFormat : int64_t {
    Gray8 = 1,
    Rgb24,
    Bgr24,
    Rgba32,
    Bgra32,
    Yuyv422,
    Uyvy422
};

/**
 * @brief Metadata and view for one video frame.
 */
struct VideoFrameView {
    /// Pointer to RGB frame bytes.
    char* data = nullptr;
    /// Frame payload size in bytes.
    size_t size = 0;
    /// Scan width in bytes.
    int64_t scanWidth = 0;
    /// Image width in pixels.
    int64_t width = 0;
    /// Image height in pixels.
    int64_t height = 0;
};

/**
 * @brief Shared-memory 24-bit RGB video ring buffer.
 *
 * VideoBuffer wraps `mmb::memvid`. The wrapper preserves libmembus metadata
 * and pointer semantics while keeping the public Winglib2 API independent of
 * raw `mmb::*` types.
 */
class VideoBuffer {
public:
    VideoBuffer() = default;

    /**
     * @brief Create a video ring buffer.
     * @param name Shared-memory object name.
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @param fps Frames per second metadata.
     * @param buffers Number of ring-buffer frames.
     * @return Open VideoBuffer on success, or an Error.
     */
    static Result<VideoBuffer> create(std::string name, int64_t width, int64_t height, int64_t fps, int64_t buffers);

    /**
     * @brief Create a video ring buffer with an explicit pixel format.
     * @param name Shared-memory object name.
     * @param width Image width in pixels.
     * @param height Image height in pixels.
     * @param format Packed pixel format.
     * @param fps Frames per second metadata.
     * @param buffers Number of ring-buffer frames.
     * @return Open VideoBuffer on success, or an Error.
     */
    static Result<VideoBuffer> create(std::string name, int64_t width, int64_t height, VideoPixelFormat format, int64_t fps, int64_t buffers);

    /**
     * @brief Attach to an existing video ring buffer and validate metadata.
     * @param name Shared-memory object name.
     * @param width Expected image width.
     * @param height Expected image height.
     * @param fps Expected frames per second.
     * @param buffers Expected number of frames.
     * @return Open VideoBuffer on success, or an Error.
     */
    static Result<VideoBuffer> attach(std::string name, int64_t width, int64_t height, int64_t fps, int64_t buffers);

    /**
     * @brief Attach to an existing video ring buffer and read metadata from it.
     * @param name Shared-memory object name.
     * @return Open VideoBuffer on success, or an Error.
     */
    static Result<VideoBuffer> openExisting(std::string name);

    /// Close the video ring buffer.
    void close();

    /**
     * @brief Check whether the buffer is open.
     * @return True when attached to shared memory.
     */
    bool isOpen() const;

    /**
     * @brief Check whether the backing shared memory already existed.
     * @return True when open found an existing object.
     */
    bool existing() const;

    /**
     * @brief Get a frame view by ring index.
     * @param index Frame index. Values wrap through the ring.
     * @return Frame view on success, or an Error.
     */
    Result<VideoFrameView> frame(int64_t index);

    /**
     * @brief Fill one frame with a byte value.
     * @param index Frame index. Values wrap through the ring.
     * @param value Byte value used to fill the frame.
     * @return Successful Result on fill, or an Error.
     */
    Result<void> fill(int64_t index, int value);

    /// @return Image width in pixels.
    int64_t width() const;
    /// @return Image height in pixels.
    int64_t height() const;
    /// @return Bits per pixel.
    int64_t bitsPerPixel() const;
    /// @return Bytes per pixel, or zero when unavailable.
    int64_t bytesPerPixel() const;
    /// @return Pixel format value, or std::nullopt when unavailable.
    std::optional<VideoPixelFormat> format() const;
    /// @return Static pixel format name, or an empty string when unavailable.
    std::string formatName() const;
    /// @return Frames per second metadata.
    int64_t fps() const;
    /// @return Number of ring-buffer frames.
    int64_t buffers() const;

    /**
     * @brief Get the ring index at an offset from the current pointer.
     * @param offset Offset from the current pointer.
     * @return Wrapped ring index, or -1 when unavailable.
     */
    int64_t pointer(int64_t offset) const;

    /**
     * @brief Set the current ring pointer.
     * @param pointer New pointer value. Values wrap through the ring.
     * @return Wrapped pointer, or -1 when unavailable.
     */
    int64_t setPointer(int64_t pointer);

    /**
     * @brief Advance the current ring pointer.
     * @param increment Increment to apply.
     * @return Wrapped pointer, or -1 when unavailable.
     */
    int64_t next(int64_t increment);

    /// @return Creator session identifier, or zero when unavailable.
    int64_t sessionId() const;

    /// @return Global published-frame sequence, or -1 when unavailable.
    int64_t sequence() const;

    /**
     * @brief Return the sequence stamped into one frame slot.
     * @param index Ring slot index.
     * @return Slot sequence, or -1 when unavailable.
     */
    int64_t frameSequence(int64_t index) const;

    /**
     * @brief Wait until the global sequence advances beyond lastSequence.
     * @param timeout Maximum wait.
     * @param lastSequence Last sequence already processed by the reader.
     * @return True when a new frame is available.
     */
    bool waitForFrame(std::chrono::milliseconds timeout, int64_t lastSequence) const;

private:
    struct Impl;

    std::string name_;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief PCM sample formats supported by libmembus v1.2.0.
 */
enum class AudioSampleFormat : int64_t {
    U8 = 1,
    S16Le,
    S24Le,
    S32Le,
    F32Le,
    F64Le
};

/**
 * @brief Metadata and view for one audio buffer.
 */
struct AudioBufferView {
    /// Pointer to PCM bytes.
    char* data = nullptr;
    /// Audio payload size in bytes.
    size_t size = 0;
    /// Channel count.
    int64_t channels = 0;
    /// Bits per sample.
    int64_t bitsPerSample = 0;
};

/**
 * @brief Shared-memory PCM audio ring buffer.
 *
 * AudioBuffer wraps `mmb::memaud`, preserving channel, sample-size, sample
 * rate, frame-rate, and ring-buffer metadata.
 */
class AudioBuffer {
public:
    AudioBuffer() = default;

    /**
     * @brief Create an audio ring buffer.
     * @param name Shared-memory object name.
     * @param channels Channel count.
     * @param bitsPerSample Bits per sample. libmembus supports 8 or 16.
     * @param sampleRate Samples per second.
     * @param fps Frames per second metadata.
     * @param buffers Number of ring-buffer frames.
     * @return Open AudioBuffer on success, or an Error.
     */
    static Result<AudioBuffer> create(std::string name, int64_t channels, int64_t bitsPerSample, int64_t sampleRate, int64_t fps, int64_t buffers);

    /**
     * @brief Create an audio ring buffer with an explicit sample format.
     * @param name Shared-memory object name.
     * @param channels Channel count.
     * @param format Sample format.
     * @param sampleRate Samples per second.
     * @param fps Buffers per second metadata.
     * @param buffers Number of ring-buffer slots.
     * @return Open AudioBuffer on success, or an Error.
     */
    static Result<AudioBuffer> create(std::string name, int64_t channels, AudioSampleFormat format, int64_t sampleRate, int64_t fps, int64_t buffers);

    /**
     * @brief Attach to an existing audio ring buffer and validate metadata.
     * @param name Shared-memory object name.
     * @param channels Expected channel count.
     * @param bitsPerSample Expected bits per sample.
     * @param sampleRate Expected samples per second.
     * @param fps Expected frames per second.
     * @param buffers Expected number of ring-buffer frames.
     * @return Open AudioBuffer on success, or an Error.
     */
    static Result<AudioBuffer> attach(std::string name, int64_t channels, int64_t bitsPerSample, int64_t sampleRate, int64_t fps, int64_t buffers);

    /**
     * @brief Attach to an existing audio ring buffer and read metadata from it.
     * @param name Shared-memory object name.
     * @return Open AudioBuffer on success, or an Error.
     */
    static Result<AudioBuffer> openExisting(std::string name);

    /// Close the audio ring buffer.
    void close();

    /**
     * @brief Check whether the buffer is open.
     * @return True when attached to shared memory.
     */
    bool isOpen() const;

    /**
     * @brief Check whether the backing shared memory already existed.
     * @return True when open found an existing object.
     */
    bool existing() const;

    /**
     * @brief Get an audio view by ring index.
     * @param index Buffer index. Values wrap through the ring.
     * @return Audio view on success, or an Error.
     */
    Result<AudioBufferView> buffer(int64_t index);

    /**
     * @brief Fill one audio buffer with a byte value.
     * @param index Buffer index. Values wrap through the ring.
     * @param value Byte value used to fill the buffer.
     * @return Successful Result on fill, or an Error.
     */
    Result<void> fill(int64_t index, int value);

    /// @return Channel count.
    int64_t channels() const;
    /// @return Bits per sample.
    int64_t bitsPerSample() const;
    /// @return Bytes per sample, or zero when unavailable.
    int64_t bytesPerSample() const;
    /// @return Sample format, or std::nullopt when unavailable.
    std::optional<AudioSampleFormat> format() const;
    /// @return Static sample format name, or an empty string when unavailable.
    std::string formatName() const;
    /// @return Samples per second.
    int64_t sampleRate() const;
    /// @return Frames per second metadata.
    int64_t fps() const;
    /// @return Number of ring-buffer frames.
    int64_t buffers() const;
    /// @return Size of one audio payload in bytes.
    int64_t bufferSize() const;

    /**
     * @brief Get the ring index at an offset from the current pointer.
     * @param offset Offset from the current pointer.
     * @return Wrapped ring index, or -1 when unavailable.
     */
    int64_t pointer(int64_t offset) const;

    /**
     * @brief Set the current ring pointer.
     * @param pointer New pointer value. Values wrap through the ring.
     * @return Wrapped pointer, or -1 when unavailable.
     */
    int64_t setPointer(int64_t pointer);

    /**
     * @brief Advance the current ring pointer.
     * @param increment Increment to apply.
     * @return Wrapped pointer, or -1 when unavailable.
     */
    int64_t next(int64_t increment);

    /// @return Creator session identifier, or zero when unavailable.
    int64_t sessionId() const;

    /// @return Global published-buffer sequence, or -1 when unavailable.
    int64_t sequence() const;

    /**
     * @brief Return the sequence stamped into one audio slot.
     * @param index Ring slot index.
     * @return Slot sequence, or -1 when unavailable.
     */
    int64_t frameSequence(int64_t index) const;

    /**
     * @brief Wait until the global sequence advances beyond lastSequence.
     * @param timeout Maximum wait.
     * @param lastSequence Last sequence already processed by the reader.
     * @return True when a new audio buffer is available.
     */
    bool waitForFrame(std::chrono::milliseconds timeout, int64_t lastSequence) const;

private:
    struct Impl;

    std::string name_;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Multi-producer, multi-consumer broadcast command channel.
 *
 * CommandChannel wraps libmembus v1.2 `mmb::memcmd`. Any open handle may
 * write commands. Handles opened as readers maintain their own read position
 * and receive every command independently.
 *
 * @code{.cpp}
 * auto rx = wl2::CommandChannel::create("/camera_cmd", 4096, true).value();
 * auto tx = wl2::CommandChannel::attach("/camera_cmd", 4096, false).value();
 *
 * tx.write("pan_left");
 * auto cmd = rx.read(std::chrono::milliseconds{100});
 * @endcode
 */
class CommandChannel {
public:
    CommandChannel() = default;

    /**
     * @brief Create a command channel.
     * @param name Shared-memory object name.
     * @param size Ring capacity in bytes.
     * @param reader True to register this handle as a command receiver.
     * @return Open channel on success, or an Error.
     */
    static Result<CommandChannel> create(std::string name, size_t size, bool reader);

    /**
     * @brief Attach to an existing command channel.
     * @param name Shared-memory object name.
     * @param size Expected ring capacity in bytes.
     * @param reader True to register this handle as a command receiver.
     * @return Open channel on success, or an Error.
     */
    static Result<CommandChannel> attach(std::string name, size_t size, bool reader = false);

    /// @return Shared-memory object name.
    const std::string& name() const noexcept { return name_; }
    /// @return Configured ring capacity in bytes.
    size_t size() const noexcept { return size_; }
    /// @return True when this handle is registered as a reader.
    bool reader() const noexcept { return reader_; }

    /// @return True when the channel is open.
    bool isOpen() const;
    /// @return True when this handle attached to an existing channel.
    bool existing() const;
    /// Close the command channel.
    void close();

    /// Write one command payload.
    Result<void> write(std::string_view command);
    /// Read one command payload with overrun status.
    Result<MembusReadResult> read(std::chrono::milliseconds timeout);
    /// @return True when a command appears ready for this reader.
    bool poll() const;
    /// @return Number of registered readers, or zero when unavailable.
    int64_t readerCount() const;
    /// @return Creator session identifier, or zero when unavailable.
    int64_t sessionId() const;

private:
    struct Impl;

    std::string name_;
    size_t size_ = 0;
    bool reader_ = false;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Fixed-schema shared-memory key-value state store.
 *
 * KeyValueStore wraps libmembus v1.2 `mmb::memkv`. The creator defines a fixed
 * number of slots and name/value size limits; any attached handle may then read
 * or write values by slot or by name.
 *
 * The wrapper validates that the chosen name/value limits produce 8-byte
 * aligned slot records before calling libmembus. Limits such as 15 and 31 are
 * safe because the stored name and value fields include a trailing null byte.
 *
 * @code{.cpp}
 * auto kv = wl2::KeyValueStore::create("/camera_state", 3, 15, 31).value();
 * kv.setName(0, "pan");
 * kv.setValue("pan", "10");
 *
 * auto reader = wl2::KeyValueStore::open("/camera_state").value();
 * std::string pan = reader.value("pan").value();
 * @endcode
 */
class KeyValueStore {
public:
    /// Map type used for snapshots, batch writes, and change sets.
    using Map = std::map<std::string, std::string>;

    KeyValueStore() = default;

    /**
     * @brief Create a fixed-schema key-value store.
     * @param name Shared-memory object name.
     * @param count Number of fixed slots.
     * @param maxNameLength Maximum slot-name bytes, excluding null terminator.
     * @param maxValueLength Maximum value bytes, excluding null terminator.
     * @param replaceExisting True to unlink and recreate an existing store.
     * @return Open store on success, or an Error.
     */
    static Result<KeyValueStore> create(std::string name, int64_t count, int64_t maxNameLength, int64_t maxValueLength, bool replaceExisting = true);

    /**
     * @brief Attach to an existing key-value store.
     * @param name Shared-memory object name.
     * @return Open store on success, or an Error.
     */
    static Result<KeyValueStore> open(std::string name);

    /// @return Shared-memory object name.
    const std::string& name() const noexcept { return name_; }

    /// @return True when the store is open.
    bool isOpen() const;
    /// @return True when this handle attached to an existing store.
    bool existing() const;
    /// Close the key-value store.
    void close();

    /// Set the immutable name for one slot.
    Result<void> setName(int64_t index, std::string_view name);
    /// Set a value by slot index.
    Result<void> setValue(int64_t index, std::string_view value);
    /// Set a value by slot name.
    Result<void> setValue(std::string_view name, std::string_view value);
    /// Write all matching values under one store update.
    Result<void> setAll(const Map& values);

    /// Read a value by slot index.
    Result<std::string> value(int64_t index) const;
    /// Read a value by slot name.
    Result<std::string> value(std::string_view name) const;
    /// Read all named values.
    Result<Map> all() const;
    /// Read values changed since epoch and update epoch.
    Result<Map> changed(int64_t& epoch) const;
    /// Wait for changed values since epoch and update epoch.
    Result<Map> waitChanged(std::chrono::milliseconds timeout, int64_t& epoch) const;
    /// Wait until the store epoch advances beyond epoch.
    Result<bool> waitForChange(std::chrono::milliseconds timeout, int64_t& epoch) const;

    /// @return Current store epoch, or -1 when unavailable.
    int64_t epoch() const;
    /// @return Creator session identifier, or zero when unavailable.
    int64_t sessionId() const;
    /// Find the slot index for a name.
    int64_t findName(std::string_view name) const;
    /// Return the name for a slot index.
    std::string slotName(int64_t index) const;
    /// @return Slot count.
    int64_t count() const;
    /// @return Maximum slot-name bytes, excluding null terminator.
    int64_t maxNameLength() const;
    /// @return Maximum value bytes, excluding null terminator.
    int64_t maxValueLength() const;

private:
    struct Impl;

    std::string name_;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Simple wait helper over arbitrary non-consuming readiness predicates.
 *
 * MembusSelector wraps libmembus v1.2 `mmb::select`. Each predicate must be
 * cheap and non-consuming; perform the actual read after wait() returns the
 * selected index.
 *
 * @code{.cpp}
 * wl2::MembusSelector selector;
 * selector.add([&] { return queue.poll(); });
 * selector.add([&] { return video.sequence() > lastVideoSeq; });
 *
 * int ready = selector.wait(std::chrono::milliseconds{100}).value();
 * @endcode
 */
class MembusSelector {
public:
    /// Non-consuming readiness predicate.
    using Condition = std::function<bool()>;

    /// Add one readiness predicate.
    void add(Condition condition);
    /// Remove all predicates.
    void clear();
    /// @return Number of registered predicates.
    size_t size() const noexcept { return conditions_.size(); }
    /// Wait for the first ready predicate index, or -1 on timeout.
    Result<int> wait(std::chrono::milliseconds timeout) const;

private:
    std::vector<Condition> conditions_;
};

} // namespace wl2
