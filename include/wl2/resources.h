#pragma once

/**
 * @file resources.h
 * @brief Embedded and in-memory resource storage.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wl2/errors.h"

namespace wl2 {

/**
 * @brief Storage compression mode for an embedded resource payload.
 */
enum class ResourceCompression {
    /// Stored bytes are the original resource bytes.
    Stored,
    /// Stored bytes use Winglib2's built-in run-length encoding.
    Rle,
};

/**
 * @brief Metadata for one embedded or in-memory resource.
 */
struct ResourceEntry {
    /// Logical resource path such as `wl2:/app/main.js`.
    std::string name;
    /// Original byte size after decompression.
    size_t originalSize = 0;
    /// Stored byte size in the embedded table.
    size_t storedSize = 0;
    /// Compression mode used for stored bytes.
    ResourceCompression compression = ResourceCompression::Stored;
    /// Content hash, currently generated as a hex digest string by CMake.
    std::string contentHash;
    /// Optional MIME/type hint.
    std::string mimeType;
    /// Application-defined flags.
    uint32_t flags = 0;
};

/**
 * @brief One named resource and its bytes.
 *
 * Resources are addressed by logical names such as `wl2:/app/main.js`.
 */
struct Resource {
    /// Logical resource name.
    std::string name;

    /// Resource contents.
    std::vector<std::byte> bytes;
};

/**
 * @brief File-system-like entry returned when listing resources.
 */
struct ResourceDirectoryEntry {
    /// Full resource path.
    std::string path;
    /// Final path component.
    std::string name;
    /// True when this entry represents a synthetic directory.
    bool directory = false;
    /// Original resource size for files; zero for directories.
    size_t size = 0;
    /// Host source path when this entry comes from a development resource map.
    std::filesystem::path sourcePath;
};

/**
 * @brief Maps a host directory into the logical `wl2:/` resource namespace.
 */
struct ResourceDirectoryMount {
    /// Host directory that provides resource bytes.
    std::filesystem::path root;
    /// Logical resource prefix such as `wl2:/resources`.
    std::string prefix;
    /// File or relative-path glob patterns excluded from this mount.
    std::vector<std::string> excludePatterns;
    /// Relative files that should report compressed metadata in development.
    std::vector<std::string> compressedFiles;
    /// Relative directories whose files should report compressed metadata.
    std::vector<std::string> compressedDirectories;
};

/**
 * @brief Read-only handle that owns a resource view lifetime.
 *
 * ResourceHandle is the safe way to access a raw pointer. For uncompressed
 * resources the pointer may refer directly to embedded read-only storage. For
 * compressed resources it refers to an immutable decompression cache entry.
 * The pointer stays valid while the handle is alive.
 *
 * @code{.cpp}
 * auto opened = runtime.resources().open("wl2:/app/config.json");
 * if (!opened) {
 *     return opened.error();
 * }
 *
 * const void* raw = opened.value().data();
 * size_t size = opened.value().size();
 * @endcode
 */
class ResourceHandle {
public:
    ResourceHandle() = default;

    /**
     * @brief Create a handle over immutable resource bytes.
     * @param entry Resource metadata.
     * @param bytes Shared immutable byte storage.
     */
    ResourceHandle(ResourceEntry entry, std::shared_ptr<const std::vector<std::byte>> bytes);

    /// @return Resource metadata.
    const ResourceEntry& entry() const noexcept { return entry_; }
    /// @return Raw read-only pointer, or null for an empty handle.
    const void* data() const noexcept;
    /// @return Byte count visible through this handle.
    size_t size() const noexcept;
    /// @return Read-only byte span valid while this handle is alive.
    std::span<const std::byte> bytes() const noexcept;
    /// @return String copy of the bytes without transcoding.
    std::string text() const;
    /// @return True when this handle contains resource bytes.
    explicit operator bool() const noexcept { return bytes_ != nullptr; }

private:
    ResourceEntry entry_;
    std::shared_ptr<const std::vector<std::byte>> bytes_;
};

/**
 * @brief In-memory store for embedded resources and development overrides.
 *
 * ResourceStore is populated by generated resource code, tests, or embedders.
 * Runtime uses it to resolve `wl2:` script specifiers before falling back to
 * filesystem loading when allowed.
 *
 * @code{.cpp}
 * wl2::ResourceStore store;
 * const unsigned char data[] = {'h', 'e', 'l', 'l', 'o'};
 * store.add("wl2:/app/message.txt", data, sizeof(data));
 *
 * if (auto resource = store.get("wl2:/app/message.txt")) {
 *     std::cout << resource->name << " has "
 *               << resource->bytes.size() << " bytes\n";
 * }
 * @endcode
 */
class ResourceStore {
public:
    /**
     * @brief Add or replace a resource by copying its bytes.
     * @param name Logical resource name.
     * @param bytes Source bytes to copy.
     */
    void add(std::string name, std::span<const std::byte> bytes);

    /**
     * @brief Add or replace a resource from an unsigned byte pointer.
     * @param name Logical resource name.
     * @param bytes Pointer to the first byte to copy.
     * @param size Number of bytes to copy from `bytes`.
     */
    void add(std::string name, const unsigned char* bytes, size_t size);

    /**
     * @brief Add or replace a stored, uncompressed resource.
     * @param entry Metadata. `compression` is forced to Stored.
     * @param bytes Original resource bytes.
     */
    void addStored(ResourceEntry entry, std::span<const std::byte> bytes);

    /**
     * @brief Add or replace a compressed resource.
     * @param entry Metadata. `compression` identifies the compression format.
     * @param bytes Stored compressed bytes.
     */
    void addCompressed(ResourceEntry entry, std::span<const std::byte> bytes);

    /**
     * @brief Add or replace a generated resource from unsigned bytes.
     * @param entry Metadata.
     * @param bytes Stored bytes.
     * @param size Stored byte count.
     */
    void addResource(ResourceEntry entry, const unsigned char* bytes, size_t size);

    /**
     * @brief Mount a host directory at a logical `wl2:/` prefix.
     * @param root Existing host directory.
     * @param prefix Logical resource prefix.
     * @param excludePatterns Glob patterns of entries to skip.
     * @param compressedFiles Glob patterns of files to store compressed.
     * @param compressedDirectories Glob patterns of directories to store compressed.
     * @return Success or validation error.
     */
    Result<void> mountDirectory(
        std::filesystem::path root,
        std::string prefix,
        std::vector<std::string> excludePatterns = {},
        std::vector<std::string> compressedFiles = {},
        std::vector<std::string> compressedDirectories = {});

    /**
     * @brief Enable or disable stderr tracing for resource lookups.
     */
    void setTraceLookups(bool enabled) noexcept { traceLookups_ = enabled; }

    /**
     * @brief Return configured development resource directory mounts.
     */
    const std::vector<ResourceDirectoryMount>& mounts() const noexcept { return mounts_; }

    /**
     * @brief Return a copy of the named resource when present.
     * @param name Logical resource name to look up.
     * @return The resource when found, otherwise std::nullopt.
     */
    std::optional<Resource> get(std::string_view name) const;

    /**
     * @brief Open a resource as a read-only handle.
     * @param name Logical resource path.
     * @return Handle on success, or an Error.
     */
    Result<ResourceHandle> open(std::string_view name) const;

    /**
     * @brief Return metadata for a file resource.
     * @param name Logical resource path.
     * @return Metadata when the file exists.
     */
    std::optional<ResourceEntry> entry(std::string_view name) const;

    /**
     * @brief Test whether a resource exists.
     * @param name Logical resource name to look up.
     * @return True when the resource exists.
     */
    bool contains(std::string_view name) const;

    /**
     * @brief Test whether a logical path exists as a file or directory.
     * @param path Logical resource path.
     * @return True when the path is a file or synthetic directory.
     */
    bool exists(std::string_view path) const;

    /**
     * @brief Test whether a logical path is a synthetic directory.
     * @param path Logical resource path.
     * @return True when at least one resource lives below `path`.
     */
    bool isDirectory(std::string_view path) const;

    /**
     * @brief List direct children of a synthetic resource directory.
     * @param path Directory path.
     * @return Sorted direct children.
     */
    std::vector<ResourceDirectoryEntry> list(std::string_view path) const;

    /**
     * @brief Walk all file resources below a path.
     * @param path Directory or file path.
     * @return Sorted file entries.
     */
    std::vector<ResourceDirectoryEntry> walk(std::string_view path) const;

    /**
     * @brief Return sorted resource names.
     * @return Names currently stored in this ResourceStore.
     */
    std::vector<std::string> names() const;

private:
    struct Record {
        ResourceEntry entry;
        std::shared_ptr<const std::vector<std::byte>> storedBytes;
    };

    std::optional<std::pair<ResourceEntry, std::filesystem::path>> mappedEntry(std::string_view name) const;
    void traceLookup(std::string_view operation, std::string_view path, std::string_view result) const;

    std::unordered_map<std::string, Record> resources_;
    std::vector<ResourceDirectoryMount> mounts_;
    bool traceLookups_ = false;
    mutable std::unordered_map<std::string, std::weak_ptr<const std::vector<std::byte>>> decompressedCache_;
};

} // namespace wl2
