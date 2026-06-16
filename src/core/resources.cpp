#include "wl2/resources.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace wl2 {

namespace {

std::vector<std::byte> copy_bytes(std::span<const std::byte> bytes) {
    return std::vector<std::byte>(bytes.begin(), bytes.end());
}

std::string normalize_dir(std::string_view path) {
    std::string out(path);
    while (out.size() > 1 && out != "wl2:/" && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string normalize_prefix(std::string_view path) {
    return normalize_dir(path);
}

bool is_wl2_path(std::string_view path) {
    return path.rfind("wl2:/", 0) == 0;
}

bool path_matches_prefix(std::string_view path, std::string_view prefix) {
    return path == prefix || (path.size() > prefix.size()
        && path.rfind(prefix, 0) == 0
        && path[prefix.size()] == '/');
}

bool glob_match(std::string_view pattern, std::string_view value) {
    size_t p = 0;
    size_t v = 0;
    size_t star = std::string_view::npos;
    size_t match = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

bool is_excluded(const ResourceDirectoryMount& mount, std::string_view relative) {
    auto name = std::filesystem::path(std::string(relative)).filename().string();
    for (const auto& pattern : mount.excludePatterns) {
        if (glob_match(pattern, relative) || glob_match(pattern, name)) {
            return true;
        }
    }
    return false;
}

bool is_compressed_policy(const ResourceDirectoryMount& mount, std::string_view relative) {
    for (const auto& file : mount.compressedFiles) {
        if (relative == file) {
            return true;
        }
    }
    for (const auto& directory : mount.compressedDirectories) {
        if (relative == directory || (relative.size() > directory.size()
                && relative.rfind(directory, 0) == 0
                && relative[directory.size()] == '/')) {
            return true;
        }
    }
    return false;
}

std::string relative_resource_path(std::string_view path, std::string_view prefix) {
    if (path.size() == prefix.size()) {
        return {};
    }
    return std::string(path.substr(prefix.size() + 1));
}

std::optional<std::filesystem::path> contained_path(
    const std::filesystem::path& root,
    std::string_view relative) {
    namespace fs = std::filesystem;
    if (relative.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    fs::path rel(relative);
    if (rel.is_absolute()) {
        return std::nullopt;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }

    std::error_code ec;
    auto canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }
    auto target = fs::weakly_canonical(canonicalRoot / rel, ec);
    if (ec) {
        target = (canonicalRoot / rel).lexically_normal();
    }
    auto relativeToRoot = target.lexically_relative(canonicalRoot);
    if (relativeToRoot.empty() || *relativeToRoot.begin() == "..") {
        return std::nullopt;
    }
    return target;
}

std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::byte> out;
    char ch = 0;
    while (in.get(ch)) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string basename(std::string_view path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(slash + 1));
}

std::vector<std::byte> decompress_rle(std::span<const std::byte> input, size_t expectedSize) {
    std::vector<std::byte> out;
    out.reserve(expectedSize);
    for (size_t i = 0; i + 1 < input.size(); i += 2) {
        const auto count = static_cast<unsigned char>(input[i]);
        const auto value = input[i + 1];
        out.insert(out.end(), count, value);
    }
    return out;
}

} // namespace

ResourceHandle::ResourceHandle(ResourceEntry entry, std::shared_ptr<const std::vector<std::byte>> bytes)
    : entry_(std::move(entry)), bytes_(std::move(bytes)) {}

const void* ResourceHandle::data() const noexcept {
    return bytes_ && !bytes_->empty() ? bytes_->data() : nullptr;
}

size_t ResourceHandle::size() const noexcept {
    return bytes_ ? bytes_->size() : 0;
}

std::span<const std::byte> ResourceHandle::bytes() const noexcept {
    if (!bytes_) {
        return {};
    }
    return std::span<const std::byte>(bytes_->data(), bytes_->size());
}

std::string ResourceHandle::text() const {
    auto view = bytes();
    return std::string(reinterpret_cast<const char*>(view.data()), view.size());
}

void ResourceStore::add(std::string name, std::span<const std::byte> bytes) {
    ResourceEntry entry;
    entry.name = std::move(name);
    entry.originalSize = bytes.size();
    entry.storedSize = bytes.size();
    entry.compression = ResourceCompression::Stored;
    addStored(std::move(entry), bytes);
}

void ResourceStore::add(std::string name, const unsigned char* bytes, size_t size) {
    add(std::move(name), std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), size));
}

void ResourceStore::addStored(ResourceEntry entry, std::span<const std::byte> bytes) {
    entry.originalSize = bytes.size();
    entry.storedSize = bytes.size();
    entry.compression = ResourceCompression::Stored;
    resources_[entry.name] = Record{entry, std::make_shared<const std::vector<std::byte>>(copy_bytes(bytes))};
    decompressedCache_.erase(entry.name);
}

void ResourceStore::addCompressed(ResourceEntry entry, std::span<const std::byte> bytes) {
    entry.storedSize = bytes.size();
    resources_[entry.name] = Record{entry, std::make_shared<const std::vector<std::byte>>(copy_bytes(bytes))};
    decompressedCache_.erase(entry.name);
}

void ResourceStore::addResource(ResourceEntry entry, const unsigned char* bytes, size_t size) {
    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes), size);
    if (entry.compression == ResourceCompression::Stored) {
        addStored(std::move(entry), span);
    } else {
        addCompressed(std::move(entry), span);
    }
}

Result<void> ResourceStore::mountDirectory(
    std::filesystem::path root,
    std::string prefix,
    std::vector<std::string> excludePatterns,
    std::vector<std::string> compressedFiles,
    std::vector<std::string> compressedDirectories) {
    namespace fs = std::filesystem;
    prefix = normalize_prefix(prefix);
    if (!is_wl2_path(prefix)) {
        return Error("resource_mount_invalid_prefix", "Resource mount prefix must start with wl2:/");
    }
    std::error_code ec;
    auto canonicalRoot = fs::weakly_canonical(root, ec);
    if (ec || !fs::is_directory(canonicalRoot)) {
        return Error("resource_mount_invalid_root", "Resource mount root is not a directory: " + root.string());
    }
    mounts_.push_back(ResourceDirectoryMount{
        canonicalRoot,
        std::move(prefix),
        std::move(excludePatterns),
        std::move(compressedFiles),
        std::move(compressedDirectories)});
    return {};
}

std::optional<std::pair<ResourceEntry, std::filesystem::path>> ResourceStore::mappedEntry(std::string_view name) const {
    auto requested = normalize_dir(name);
    for (auto it = mounts_.rbegin(); it != mounts_.rend(); ++it) {
        if (!path_matches_prefix(requested, it->prefix)) {
            continue;
        }
        auto relative = relative_resource_path(requested, it->prefix);
        if (is_excluded(*it, relative)) {
            continue;
        }
        auto hostPath = contained_path(it->root, relative);
        if (!hostPath) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*hostPath, ec)) {
            continue;
        }
        auto size = std::filesystem::file_size(*hostPath, ec);
        if (ec) {
            continue;
        }
        ResourceEntry entry;
        entry.name = requested;
        entry.originalSize = static_cast<size_t>(size);
        entry.storedSize = static_cast<size_t>(size);
        entry.compression = ResourceCompression::Stored;
        if (is_compressed_policy(*it, relative)) {
            entry.compression = ResourceCompression::Rle;
        }
        return std::make_pair(std::move(entry), *hostPath);
    }
    return std::nullopt;
}

void ResourceStore::traceLookup(std::string_view operation, std::string_view path, std::string_view result) const {
    if (traceLookups_) {
        std::cerr << "wl2 resource " << operation << ": " << path << " -> " << result << '\n';
    }
}

std::optional<Resource> ResourceStore::get(std::string_view name) const {
    auto opened = open(name);
    if (!opened) {
        return std::nullopt;
    }
    const auto& handle = opened.value();
    return Resource{handle.entry().name, std::vector<std::byte>(handle.bytes().begin(), handle.bytes().end())};
}

Result<ResourceHandle> ResourceStore::open(std::string_view name) const {
    if (auto mapped = mappedEntry(name)) {
        traceLookup("open", name, mapped->second.string());
        auto bytes = std::make_shared<const std::vector<std::byte>>(read_file_bytes(mapped->second));
        return ResourceHandle{std::move(mapped->first), bytes};
    }

    auto it = resources_.find(std::string(name));
    if (it == resources_.end()) {
        traceLookup("open", name, "miss");
        return Error("resource_not_found", "No embedded or mapped resource named " + std::string(name));
    }
    traceLookup("open", name, "embedded");

    const auto& record = it->second;
    if (record.entry.compression == ResourceCompression::Stored) {
        return ResourceHandle{record.entry, record.storedBytes};
    }

    if (record.entry.compression == ResourceCompression::Rle) {
        if (auto cached = decompressedCache_[record.entry.name].lock()) {
            return ResourceHandle{record.entry, cached};
        }
        auto decompressed = std::make_shared<const std::vector<std::byte>>(
            decompress_rle(std::span<const std::byte>(record.storedBytes->data(), record.storedBytes->size()), record.entry.originalSize));
        if (decompressed->size() != record.entry.originalSize) {
            return Error("resource_decompress_failed", "Resource decompressed size mismatch for " + record.entry.name);
        }
        decompressedCache_[record.entry.name] = decompressed;
        return ResourceHandle{record.entry, decompressed};
    }

    return Error("resource_compression_unknown", "Unsupported compression mode for " + record.entry.name);
}

std::optional<ResourceEntry> ResourceStore::entry(std::string_view name) const {
    if (auto mapped = mappedEntry(name)) {
        return mapped->first;
    }
    auto it = resources_.find(std::string(name));
    if (it == resources_.end()) {
        return std::nullopt;
    }
    return it->second.entry;
}

bool ResourceStore::contains(std::string_view name) const {
    if (mappedEntry(name)) {
        return true;
    }
    return resources_.find(std::string(name)) != resources_.end();
}

bool ResourceStore::exists(std::string_view path) const {
    return contains(path) || isDirectory(path);
}

bool ResourceStore::isDirectory(std::string_view path) const {
    auto dir = normalize_dir(path);
    const auto prefix = dir + "/";
    for (const auto& mount : mounts_) {
        if (dir == mount.prefix || path_matches_prefix(dir, mount.prefix)) {
            auto relative = relative_resource_path(dir, mount.prefix);
            if (auto hostPath = contained_path(mount.root, relative)) {
                std::error_code ec;
                if (std::filesystem::is_directory(*hostPath, ec)) {
                    return true;
                }
            }
        } else if (path_matches_prefix(mount.prefix, dir)) {
            return true;
        }
    }
    for (const auto& [name, _] : resources_) {
        if (name.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<ResourceDirectoryEntry> ResourceStore::list(std::string_view path) const {
    std::map<std::string, ResourceDirectoryEntry> entries;
    auto dir = normalize_dir(path);
    const auto prefix = dir + "/";

    for (const auto& mount : mounts_) {
        if (dir == mount.prefix || path_matches_prefix(dir, mount.prefix)) {
            auto relative = relative_resource_path(dir, mount.prefix);
            auto hostDir = contained_path(mount.root, relative);
            std::error_code ec;
            if (hostDir && std::filesystem::is_directory(*hostDir, ec)) {
                for (const auto& child : std::filesystem::directory_iterator(*hostDir, ec)) {
                    if (ec) {
                        break;
                    }
                    auto logical = prefix + child.path().filename().string();
                    const bool directory = child.is_directory(ec);
                    const bool file = child.is_regular_file(ec);
                    if (!directory && !file) {
                        continue;
                    }
                    auto relative = child.path().lexically_relative(mount.root).generic_string();
                    if (is_excluded(mount, relative)) {
                        continue;
                    }
                    size_t size = 0;
                    if (file) {
                        auto fileSize = child.file_size(ec);
                        if (!ec) {
                            size = static_cast<size_t>(fileSize);
                        }
                    }
                    entries[logical] = ResourceDirectoryEntry{logical, basename(logical), directory, size, child.path()};
                }
            }
        } else if (path_matches_prefix(mount.prefix, dir)) {
            auto rest = std::string_view(mount.prefix).substr(prefix.size());
            auto slash = rest.find('/');
            auto logical = slash == std::string_view::npos
                ? mount.prefix
                : prefix + std::string(rest.substr(0, slash));
            entries[logical] = ResourceDirectoryEntry{logical, basename(logical), true, 0, mount.root};
        }
    }

    for (const auto& [name, record] : resources_) {
        if (name.rfind(prefix, 0) != 0) {
            continue;
        }
        auto rest = std::string_view(name).substr(prefix.size());
        if (rest.empty()) {
            continue;
        }
        auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            entries[name] = ResourceDirectoryEntry{name, basename(name), false, record.entry.originalSize};
        } else {
            auto child = prefix + std::string(rest.substr(0, slash));
            entries[child] = ResourceDirectoryEntry{child, basename(child), true, 0};
        }
    }

    std::vector<ResourceDirectoryEntry> out;
    out.reserve(entries.size());
    for (auto& [_, entry] : entries) {
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<ResourceDirectoryEntry> ResourceStore::walk(std::string_view path) const {
    std::vector<ResourceDirectoryEntry> out;
    auto root = normalize_dir(path);
    const auto prefix = root + "/";
    std::set<std::string> seen;

    for (const auto& mount : mounts_) {
        if (root == mount.prefix || path_matches_prefix(root, mount.prefix)) {
            auto relative = relative_resource_path(root, mount.prefix);
            auto hostRoot = contained_path(mount.root, relative);
            std::error_code ec;
            if (hostRoot && std::filesystem::is_regular_file(*hostRoot, ec)) {
                auto size = std::filesystem::file_size(*hostRoot, ec);
                auto logical = root;
                if (seen.insert(logical).second) {
                    out.push_back(ResourceDirectoryEntry{logical, basename(logical), false, ec ? 0 : static_cast<size_t>(size), *hostRoot});
                }
            } else if (hostRoot && std::filesystem::is_directory(*hostRoot, ec)) {
                for (const auto& child : std::filesystem::recursive_directory_iterator(*hostRoot, ec)) {
                    if (ec) {
                        break;
                    }
                    if (!child.is_regular_file(ec)) {
                        continue;
                    }
                    auto rel = child.path().lexically_relative(*hostRoot).generic_string();
                    if (is_excluded(mount, rel)) {
                        continue;
                    }
                    auto logical = prefix + rel;
                    auto size = child.file_size(ec);
                    if (seen.insert(logical).second) {
                        out.push_back(ResourceDirectoryEntry{logical, basename(logical), false, ec ? 0 : static_cast<size_t>(size), child.path()});
                    }
                }
            }
        } else if (path_matches_prefix(mount.prefix, root)) {
            auto hostRoot = mount.root;
            std::error_code ec;
            for (const auto& child : std::filesystem::recursive_directory_iterator(hostRoot, ec)) {
                if (ec) {
                    break;
                }
                if (!child.is_regular_file(ec)) {
                    continue;
                }
                auto rel = child.path().lexically_relative(hostRoot).generic_string();
                if (is_excluded(mount, rel)) {
                    continue;
                }
                auto logical = mount.prefix + "/" + rel;
                if (logical == root || logical.rfind(prefix, 0) == 0) {
                    auto size = child.file_size(ec);
                    if (seen.insert(logical).second) {
                        out.push_back(ResourceDirectoryEntry{logical, basename(logical), false, ec ? 0 : static_cast<size_t>(size), child.path()});
                    }
                }
            }
        }
    }

    for (const auto& [name, record] : resources_) {
        if (name == root || name.rfind(prefix, 0) == 0) {
            if (seen.insert(name).second) {
                out.push_back(ResourceDirectoryEntry{name, basename(name), false, record.entry.originalSize, {}});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.path < b.path;
    });
    return out;
}

std::vector<std::string> ResourceStore::names() const {
    std::vector<std::string> out;
    out.reserve(resources_.size());
    std::set<std::string> names;
    for (const auto& mount : mounts_) {
        std::error_code ec;
        for (const auto& child : std::filesystem::recursive_directory_iterator(mount.root, ec)) {
            if (ec) {
                break;
            }
            if (!child.is_regular_file(ec)) {
                continue;
            }
            auto rel = child.path().lexically_relative(mount.root).generic_string();
            if (is_excluded(mount, rel)) {
                continue;
            }
            names.insert(mount.prefix + "/" + rel);
        }
    }
    for (const auto& [name, _] : resources_) {
        names.insert(name);
    }
    for (const auto& name : names) {
        out.push_back(name);
    }
    return out;
}

} // namespace wl2
