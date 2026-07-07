#pragma once

/**
 * @file trust_store.h
 * @brief JSON-backed user trust records for approved script permission envelopes.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "wl2/errors.h"
#include "wl2/runtime.h"

namespace wl2 {

struct TrustFilesystemPath {
    std::string raw;
    std::filesystem::path resolved;
};

struct TrustPermissions {
    std::vector<std::string> network;
    std::vector<std::string> listen;
    std::vector<std::string> sharedMemory;
    std::vector<TrustFilesystemPath> filesystemRead;
    std::vector<TrustFilesystemPath> filesystemWrite;
    bool ui = false;
    bool graphics = false;
};

struct TrustRecord {
    std::string id;
    std::string kind = "script";
    std::filesystem::path path;
    std::string displayPath;
    std::string sha256;
    TrustPermissions permissions;
    std::string grantedAt;
    std::string lastUsedAt;
    std::string source = "interactive";
};

enum class TrustMatchKind {
    None,
    Exact,
    SameHashDifferentPath,
    SamePathDifferentHash,
    BroaderPermissions,
};

struct TrustMatch {
    TrustMatchKind kind = TrustMatchKind::None;
    const TrustRecord* record = nullptr;
    PermissionSet delta;
};

PermissionSet permissionSetFromTrustPermissions(const TrustPermissions& permissions);
TrustPermissions trustPermissionsFromPermissionSet(
    const PermissionSet& permissions,
    const std::vector<std::string>& rawFilesystemRead = {},
    const std::vector<std::string>& rawFilesystemWrite = {});

/// Comma-separated list of the permission categories present in a record, for `trust list`.
std::string trustPermissionSummary(const TrustPermissions& permissions);

/// Serialize a single record to canonical trust.json record-object text.
std::string trustRecordToJsonText(const TrustRecord& record, bool pretty = true);

/// Serialize records to the canonical trust.json array text (records only, without the envelope).
std::string trustRecordsToJsonText(const std::vector<TrustRecord>& records, bool pretty = true);

class TrustStore {
public:
    static Result<TrustStore> load(const std::filesystem::path& path);

    Result<void> save(const std::filesystem::path& path) const;

    const std::vector<TrustRecord>& records() const noexcept { return records_; }
    std::vector<TrustRecord>& records() noexcept { return records_; }

    TrustMatch match(
        const std::filesystem::path& path,
        std::string_view sha256,
        const PermissionSet& requested) const;

    void addOrUpdate(TrustRecord record);
    bool revoke(std::string_view id);
    void clear() noexcept;

private:
    std::vector<TrustRecord> records_;
};

} // namespace wl2
