#pragma once

/**
 * @file hash.h
 * @brief Small self-contained content hashing used for integrity checks.
 *
 * These helpers back the module-system hardening: lockfiles and installed
 * module metadata record a content digest so a later step can detect a stale or
 * tampered artifact before it is built or loaded. The implementation is a
 * dependency-free SHA-256 so integrity checks work everywhere the runtime does.
 */

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "wl2/errors.h"

namespace wl2 {

/// Lowercase hex SHA-256 digest of @p length bytes at @p data.
std::string sha256Hex(const void* data, std::size_t length);

/// Lowercase hex SHA-256 digest of @p data.
std::string sha256Hex(std::string_view data);

/// Lowercase hex SHA-256 digest of a file's contents. Returns an error when the
/// file cannot be opened or read.
Result<std::string> sha256File(const std::filesystem::path& path);

} // namespace wl2
