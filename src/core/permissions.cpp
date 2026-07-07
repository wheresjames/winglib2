#include "wl2/permissions.h"

#include <algorithm>
#include <cstdlib>

namespace wl2 {

namespace {

template <typename T>
void sort_unique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool endpoint_matches(const std::string& entry, const std::string& host, const std::string& portText) {
    if (entry == "*" || entry == "*:*") {
        return true;
    }
    auto colon = entry.rfind(':');
    if (colon == std::string::npos) {
        return entry == host;
    }
    const std::string entryHost = entry.substr(0, colon);
    const std::string entryPort = entry.substr(colon + 1);
    const bool hostOk = entryHost == "*" || entryHost == host;
    // A request for "*" (all ports) is only covered by an approval that also
    // spans all ports; it must not be satisfied by a specific-port approval.
    const bool portOk = entryPort == "*" || (portText != "*" && entryPort == portText);
    return hostOk && portOk;
}

bool endpoint_inside(const std::vector<std::string>& approved, const std::string& requested) {
    auto colon = requested.rfind(':');
    if (colon == std::string::npos) {
        for (const auto& entry : approved) {
            if (entry == "*" || entry == requested || entry == requested + ":*") {
                return true;
            }
        }
        return false;
    }
    const std::string host = requested.substr(0, colon);
    const std::string portText = requested.substr(colon + 1);
    for (const auto& entry : approved) {
        if (endpoint_matches(entry, host, portText)) {
            return true;
        }
    }
    return false;
}

bool prefix_inside(const std::vector<std::string>& approved, const std::string& requested) {
    for (const auto& entry : approved) {
        if (!entry.empty() && requested.rfind(entry, 0) == 0) {
            return true;
        }
    }
    return false;
}

bool path_contained_by(const std::filesystem::path& target, const std::filesystem::path& root) {
    const auto normalizedTarget = target.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    if (normalizedTarget == normalizedRoot) {
        return true;
    }
    const auto relative = normalizedTarget.lexically_relative(normalizedRoot);
    return !relative.empty() && *relative.begin() != "..";
}

bool path_inside(const std::vector<std::filesystem::path>& approved, const std::filesystem::path& requested) {
    for (const auto& root : approved) {
        if (path_contained_by(requested, root)) {
            return true;
        }
    }
    return false;
}

} // namespace

PermissionSet normalizePermissionSet(const PermissionSet& permissions) {
    PermissionSet out = permissions;
    sort_unique(out.network);
    sort_unique(out.listen);
    sort_unique(out.sharedMemory);
    for (auto& path : out.filesystemRead) {
        path = path.lexically_normal();
    }
    for (auto& path : out.filesystemWrite) {
        path = path.lexically_normal();
    }
    sort_unique(out.filesystemRead);
    sort_unique(out.filesystemWrite);
    return out;
}

bool permissionSetContains(const PermissionSet& approved, const PermissionSet& requested) {
    const auto normalizedApproved = normalizePermissionSet(approved);
    const auto normalizedRequested = normalizePermissionSet(requested);

    if (normalizedRequested.ui && !normalizedApproved.ui) {
        return false;
    }
    if (normalizedRequested.graphics && !normalizedApproved.graphics) {
        return false;
    }
    for (const auto& item : normalizedRequested.network) {
        if (!endpoint_inside(normalizedApproved.network, item)) {
            return false;
        }
    }
    for (const auto& item : normalizedRequested.listen) {
        if (!endpoint_inside(normalizedApproved.listen, item)) {
            return false;
        }
    }
    for (const auto& item : normalizedRequested.sharedMemory) {
        if (!prefix_inside(normalizedApproved.sharedMemory, item)) {
            return false;
        }
    }
    for (const auto& item : normalizedRequested.filesystemRead) {
        if (!path_inside(normalizedApproved.filesystemRead, item)) {
            return false;
        }
    }
    for (const auto& item : normalizedRequested.filesystemWrite) {
        if (!path_inside(normalizedApproved.filesystemWrite, item)) {
            return false;
        }
    }
    return true;
}

PermissionSet permissionSetDelta(const PermissionSet& approved, const PermissionSet& requested) {
    const auto normalizedApproved = normalizePermissionSet(approved);
    const auto normalizedRequested = normalizePermissionSet(requested);

    PermissionSet delta;
    if (normalizedRequested.ui && !normalizedApproved.ui) {
        delta.ui = true;
    }
    if (normalizedRequested.graphics && !normalizedApproved.graphics) {
        delta.graphics = true;
    }
    for (const auto& item : normalizedRequested.network) {
        if (!endpoint_inside(normalizedApproved.network, item)) {
            delta.network.push_back(item);
        }
    }
    for (const auto& item : normalizedRequested.listen) {
        if (!endpoint_inside(normalizedApproved.listen, item)) {
            delta.listen.push_back(item);
        }
    }
    for (const auto& item : normalizedRequested.sharedMemory) {
        if (!prefix_inside(normalizedApproved.sharedMemory, item)) {
            delta.sharedMemory.push_back(item);
        }
    }
    for (const auto& item : normalizedRequested.filesystemRead) {
        if (!path_inside(normalizedApproved.filesystemRead, item)) {
            delta.filesystemRead.push_back(item);
        }
    }
    for (const auto& item : normalizedRequested.filesystemWrite) {
        if (!path_inside(normalizedApproved.filesystemWrite, item)) {
            delta.filesystemWrite.push_back(item);
        }
    }
    return normalizePermissionSet(delta);
}

} // namespace wl2
