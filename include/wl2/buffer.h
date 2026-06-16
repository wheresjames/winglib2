#pragma once

/**
 * @file buffer.h
 * @brief Binary buffer and shared-memory handle primitives.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/errors.h"

namespace wl2 {

/**
 * @brief Copy-on-write byte buffer for in-process binary data.
 *
 * Buffer is the default C++ value type for byte payloads crossing runtime,
 * module, and future thread-tree boundaries. Copies are cheap because storage
 * is reference-counted. Calling mutableView() detaches first when storage is
 * shared or sliced, preserving copy-on-write behavior.
 *
 * JavaScript exposes this concept as `wl2.Buffer`.
 *
 * @code{.cpp}
 * auto a = wl2::Buffer::fromString("hello");
 * auto b = a.slice(1, 3);
 *
 * std::string text = b.text();        // "ell"
 * auto writable = b.mutableView();    // detaches b if needed
 * writable[0] = std::byte{'E'};
 * @endcode
 */
class Buffer {
public:
    /// Create an empty buffer.
    Buffer();

    /**
     * @brief Copy bytes into a new Buffer.
     * @param bytes Source bytes to copy. The caller retains ownership.
     * @return A Buffer owning an independent copy of `bytes`.
     */
    static Buffer copy(std::span<const std::byte> bytes);

    /**
     * @brief Copy string contents into a new Buffer.
     * @param text Source bytes. The contents are not transcoded.
     * @return A Buffer owning an independent copy of `text`.
     */
    static Buffer fromString(std::string_view text);

    /**
     * @brief Number of bytes visible through this buffer view.
     * @return The visible byte count.
     */
    size_t size() const noexcept;

    /**
     * @brief Test whether the buffer is empty.
     * @return True when size() is zero.
     */
    bool empty() const noexcept;

    /**
     * @brief Read-only view of the visible bytes.
     * @return A span valid while this Buffer object remains alive and
     * unchanged.
     */
    std::span<const std::byte> read() const noexcept;

    /**
     * @brief Writable view, detaching first when storage is shared or sliced.
     * @return A mutable span over this Buffer's private storage.
     */
    std::span<std::byte> mutableView();

    /**
     * @brief Return a cheap view over a subrange of this buffer.
     * @param offset First byte to include.
     * @param length Maximum number of bytes to include.
     * @return A Buffer view over the requested range, or an empty Buffer when
     * `offset` is outside the visible range.
     */
    Buffer slice(size_t offset, size_t length) const;

    /**
     * @brief Interpret bytes as a string without transcoding.
     * @return A string copy of the visible bytes.
     */
    std::string text() const;

private:
    explicit Buffer(std::shared_ptr<std::vector<std::byte>> storage, size_t offset, size_t size);
    void detach();

    std::shared_ptr<std::vector<std::byte>> storage_;
    size_t offset_ = 0;
    size_t size_ = 0;
};

/**
 * @brief Named shared memory handle descriptor.
 *
 * This type is the lightweight handle shape that will be backed by
 * `libmembus` memory maps. It intentionally records identity and access
 * policy, not a raw pointer, so handles can later be validated and passed
 * between script threads safely.
 *
 * @code{.cpp}
 * wl2::SharedBuffer handle{"/camera_frame", 1920 * 1080 * 3, false};
 * if (handle.valid() && !handle.writable()) {
 *     // Attach read-only through the libmembus-backed implementation.
 * }
 * @endcode
 */
class SharedBuffer {
public:
    /// Value used by read() to request the full mapped buffer.
    static constexpr size_t AllBytes = static_cast<size_t>(-1);

    /// Create an invalid handle.
    SharedBuffer() = default;

    /**
     * @brief Create a handle descriptor for a named shared-memory object.
     * @param name Shared-memory object name.
     * @param size Expected size in bytes.
     * @param writable True when the handle is intended to allow writes.
     */
    SharedBuffer(std::string name, size_t size, bool writable);

    /**
     * @brief Create and open a named shared-memory buffer.
     * @param name Shared-memory object name. POSIX names should start with `/`.
     * @param size Size in bytes to create.
     * @param replaceExisting True to unlink and recreate an existing object.
     * @return Open SharedBuffer on success, or an Error.
     */
    static Result<SharedBuffer> create(std::string name, size_t size, bool replaceExisting = true);

    /**
     * @brief Attach to an existing named shared-memory buffer.
     * @param name Shared-memory object name.
     * @param size Expected size in bytes, or zero to accept the mapped size.
     * @return Open SharedBuffer on success, or an Error.
     */
    static Result<SharedBuffer> attach(std::string name, size_t size = 0);

    /**
     * @brief Shared-memory object name.
     * @return The stored name. Empty means the handle is invalid.
     */
    const std::string& name() const noexcept { return name_; }

    /**
     * @brief Expected size in bytes.
     * @return The expected shared-memory size.
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief Whether this handle is intended to allow writes.
     * @return True for writable handles.
     */
    bool writable() const noexcept { return writable_; }

    /**
     * @brief Check whether the descriptor contains usable identity data.
     * @return True when the handle has both a name and non-zero size.
     */
    bool valid() const noexcept { return !name_.empty() && size_ > 0; }

    /**
     * @brief Check whether the underlying memory map is open.
     * @return True when this wrapper is attached to shared memory.
     */
    bool isOpen() const;

    /**
     * @brief Check whether the underlying shared memory already existed.
     * @return True when attach/open found an existing object.
     */
    bool existing() const;

    /// Close the underlying shared-memory mapping.
    void close();

    /**
     * @brief Copy bytes into the mapped memory.
     * @param bytes Bytes to write. Data longer than size() is truncated.
     * @return Number of bytes written, or an Error.
     */
    Result<size_t> write(std::string_view bytes);

    /**
     * @brief Copy bytes out of the mapped memory.
     * @param maxBytes Maximum bytes to read, or AllBytes for the full map.
     * @return Read bytes on success, or an Error.
     */
    Result<std::string> read(size_t maxBytes = AllBytes) const;

    /**
     * @brief Access the mapped memory directly.
     * @return Pointer to the mapped bytes, or null when not open.
     */
    char* data();

    /**
     * @brief Access the mapped memory directly.
     * @return Const pointer to the mapped bytes, or null when not open.
     */
    const char* data() const;

private:
    struct Impl;

    std::string name_;
    size_t size_ = 0;
    bool writable_ = false;
    std::shared_ptr<Impl> impl_;
};

} // namespace wl2
