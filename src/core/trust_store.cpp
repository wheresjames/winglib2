#include "wl2/trust_store.h"

#include "wl2/permissions.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

#include <nlohmann/json.hpp>

namespace wl2 {

namespace {

using json = nlohmann::json;

json permission_set_json(const TrustPermissions& permissions) {
    json fs = json::array();
    for (const auto& entry : permissions.filesystemRead) {
        fs.push_back({
            {"raw", entry.raw.empty() ? entry.resolved.string() : entry.raw},
            {"resolved", entry.resolved.string()},
        });
    }
    return {
        {"network", permissions.network},
        {"listen", permissions.listen},
        {"sharedMemory", permissions.sharedMemory},
        {"filesystemRead", fs},
        {"ui", permissions.ui},
        {"graphics", permissions.graphics},
    };
}

TrustPermissions trust_permissions_from_json(const json& value) {
    TrustPermissions out;
    if (!value.is_object()) {
        return out;
    }
    out.network = value.value("network", std::vector<std::string>{});
    out.listen = value.value("listen", std::vector<std::string>{});
    out.sharedMemory = value.value("sharedMemory", std::vector<std::string>{});
    out.ui = value.value("ui", false);
    out.graphics = value.value("graphics", false);
    if (auto it = value.find("filesystemRead"); it != value.end() && it->is_array()) {
        for (const auto& entry : *it) {
            if (entry.is_string()) {
                const auto text = entry.get<std::string>();
                out.filesystemRead.push_back({text, text});
            } else if (entry.is_object()) {
                const auto resolved = entry.value("resolved", std::string{});
                if (!resolved.empty()) {
                    out.filesystemRead.push_back({
                        entry.value("raw", resolved),
                        std::filesystem::path(resolved),
                    });
                }
            }
        }
    }
    return out;
}

json record_json(const TrustRecord& record) {
    return {
        {"id", record.id},
        {"kind", record.kind},
        {"path", record.path.string()},
        {"displayPath", record.displayPath},
        {"sha256", record.sha256},
        {"permissions", permission_set_json(record.permissions)},
        {"grantedAt", record.grantedAt},
        {"lastUsedAt", record.lastUsedAt},
        {"source", record.source},
    };
}

TrustRecord record_from_json(const json& value) {
    TrustRecord out;
    out.id = value.value("id", std::string{});
    out.kind = value.value("kind", std::string{"script"});
    out.path = value.value("path", std::string{});
    out.displayPath = value.value("displayPath", out.path.string());
    out.sha256 = value.value("sha256", std::string{});
    if (auto permissions = value.find("permissions"); permissions != value.end()) {
        out.permissions = trust_permissions_from_json(*permissions);
    }
    out.grantedAt = value.value("grantedAt", std::string{});
    out.lastUsedAt = value.value("lastUsedAt", std::string{});
    out.source = value.value("source", std::string{"interactive"});
    return out;
}

Result<void> write_text_atomic(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return Error("trust_store_write_failed", "Unable to create trust store directory: " + ec.message());
    }

    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tempPath = path.parent_path() / (path.filename().string() + ".tmp." + std::to_string(ticks));
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Error("trust_store_write_failed", "Unable to open temporary trust store: " + tempPath.string());
        }
        out << text;
        if (!out) {
            return Error("trust_store_write_failed", "Unable to write temporary trust store: " + tempPath.string());
        }
    }

    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        std::filesystem::remove(tempPath);
        return Error("trust_store_write_failed", "Unable to replace trust store: " + ec.message());
    }
    return {};
}

} // namespace

PermissionSet permissionSetFromTrustPermissions(const TrustPermissions& permissions) {
    PermissionSet out;
    out.network = permissions.network;
    out.listen = permissions.listen;
    out.sharedMemory = permissions.sharedMemory;
    out.ui = permissions.ui;
    out.graphics = permissions.graphics;
    for (const auto& entry : permissions.filesystemRead) {
        out.filesystemRead.push_back(entry.resolved);
    }
    return normalizePermissionSet(out);
}

TrustPermissions trustPermissionsFromPermissionSet(
    const PermissionSet& permissions,
    const std::vector<std::string>& rawFilesystemRead) {
    const auto normalized = normalizePermissionSet(permissions);
    TrustPermissions out;
    out.network = normalized.network;
    out.listen = normalized.listen;
    out.sharedMemory = normalized.sharedMemory;
    out.ui = normalized.ui;
    out.graphics = normalized.graphics;
    for (size_t i = 0; i < normalized.filesystemRead.size(); ++i) {
        const auto resolved = normalized.filesystemRead[i];
        const std::string raw = i < rawFilesystemRead.size() && !rawFilesystemRead[i].empty()
            ? rawFilesystemRead[i]
            : resolved.string();
        out.filesystemRead.push_back({raw, resolved});
    }
    return out;
}

std::string trustPermissionSummary(const TrustPermissions& permissions) {
    std::vector<std::string> categories;
    if (!permissions.network.empty()) {
        categories.push_back("network");
    }
    if (!permissions.listen.empty()) {
        categories.push_back("listen");
    }
    if (!permissions.sharedMemory.empty()) {
        categories.push_back("sharedMemory");
    }
    if (!permissions.filesystemRead.empty()) {
        categories.push_back("filesystemRead");
    }
    if (permissions.ui) {
        categories.push_back("ui");
    }
    if (permissions.graphics) {
        categories.push_back("graphics");
    }
    std::string out;
    for (size_t i = 0; i < categories.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += categories[i];
    }
    return out.empty() ? std::string{"-"} : out;
}

std::string trustRecordToJsonText(const TrustRecord& record, bool pretty) {
    const auto value = record_json(record);
    return pretty ? value.dump(2) : value.dump();
}

std::string trustRecordsToJsonText(const std::vector<TrustRecord>& records, bool pretty) {
    json array = json::array();
    for (const auto& record : records) {
        array.push_back(record_json(record));
    }
    return pretty ? array.dump(2) : array.dump();
}

Result<TrustStore> TrustStore::load(const std::filesystem::path& path) {
    TrustStore store;
    if (path.empty() || !std::filesystem::exists(path)) {
        return store;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Error("trust_store_read_failed", "Unable to open trust store: " + path.string());
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        const auto root = json::parse(text);
        if (!root.is_object() || root.value("schema", std::string{}) != "wl2.trust.v1") {
            return Error("trust_store_invalid", "Trust store has an unsupported schema: " + path.string());
        }
        if (auto records = root.find("records"); records != root.end() && records->is_array()) {
            for (const auto& record : *records) {
                if (!record.is_object()) {
                    continue;
                }
                auto parsed = record_from_json(record);
                if (!parsed.id.empty() && !parsed.path.empty() && !parsed.sha256.empty()) {
                    store.records_.push_back(std::move(parsed));
                }
            }
        }
    } catch (const json::parse_error& e) {
        return Error("trust_store_invalid_json", "Unable to parse trust store JSON", e.what());
    } catch (const json::exception& e) {
        return Error("trust_store_invalid", "Unable to load trust store", e.what());
    }
    return store;
}

Result<void> TrustStore::save(const std::filesystem::path& path) const {
    json records = json::array();
    for (const auto& record : records_) {
        records.push_back(record_json(record));
    }
    const json root = {
        {"schema", "wl2.trust.v1"},
        {"records", records},
    };
    return write_text_atomic(path, root.dump(2) + "\n");
}

TrustMatch TrustStore::match(
    const std::filesystem::path& path,
    std::string_view sha256,
    const PermissionSet& requested) const {
    TrustMatch sameHashDifferentPath;
    TrustMatch samePathDifferentHash;
    const auto normalizedRequested = normalizePermissionSet(requested);

    for (const auto& record : records_) {
        const bool pathMatches = record.path == path;
        const bool hashMatches = record.sha256 == sha256;
        if (pathMatches && hashMatches) {
            const auto approved = permissionSetFromTrustPermissions(record.permissions);
            if (permissionSetContains(approved, normalizedRequested)) {
                return TrustMatch{TrustMatchKind::Exact, &record, {}};
            }
            return TrustMatch{TrustMatchKind::BroaderPermissions, &record,
                permissionSetDelta(approved, normalizedRequested)};
        }
        if (!pathMatches && hashMatches && !sameHashDifferentPath.record) {
            sameHashDifferentPath = TrustMatch{TrustMatchKind::SameHashDifferentPath, &record, {}};
        }
        if (pathMatches && !hashMatches && !samePathDifferentHash.record) {
            const auto approved = permissionSetFromTrustPermissions(record.permissions);
            samePathDifferentHash = TrustMatch{TrustMatchKind::SamePathDifferentHash, &record,
                permissionSetDelta(approved, normalizedRequested)};
        }
    }

    if (sameHashDifferentPath.record) {
        return sameHashDifferentPath;
    }
    if (samePathDifferentHash.record) {
        return samePathDifferentHash;
    }
    return {};
}

void TrustStore::addOrUpdate(TrustRecord record) {
    for (auto& existing : records_) {
        if ((!record.id.empty() && existing.id == record.id) ||
            (existing.path == record.path && existing.sha256 == record.sha256)) {
            existing = std::move(record);
            return;
        }
    }
    records_.push_back(std::move(record));
}

bool TrustStore::revoke(std::string_view id) {
    const auto before = records_.size();
    records_.erase(
        std::remove_if(records_.begin(), records_.end(), [&](const TrustRecord& record) {
            return record.id == id;
        }),
        records_.end());
    return records_.size() != before;
}

void TrustStore::clear() noexcept {
    records_.clear();
}

} // namespace wl2
