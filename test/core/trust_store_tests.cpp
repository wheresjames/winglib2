#include "wl2/permissions.h"
#include "wl2/trust_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
    std::cerr << "trust store test failed: " << message << '\n';
    return 1;
}

fs::path temp_root() {
    auto root = fs::temp_directory_path() / ("wl2-trust-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

wl2::PermissionSet permissions(std::vector<std::string> listen = {}) {
    wl2::PermissionSet set;
    set.network = {"127.0.0.1:*", "example.com:443", "example.com:443"};
    set.listen = std::move(listen);
    set.sharedMemory = {"/wl2/demo/"};
    set.filesystemRead = {"/tmp/demo/../demo"};
    set.graphics = true;
    return wl2::normalizePermissionSet(set);
}

int permission_tests() {
    wl2::PermissionSet approved = permissions({"127.0.0.1:8080"});
    wl2::PermissionSet requested;
    requested.network = {"127.0.0.1:4567", "example.com:443"};
    requested.listen = {"127.0.0.1:8080"};
    requested.sharedMemory = {"/wl2/demo/frames"};
    requested.filesystemRead = {"/tmp/demo/file.txt"};
    requested.graphics = true;

    if (!wl2::permissionSetContains(approved, requested)) {
        return fail("approved permissions should contain narrower request");
    }

    requested.listen.push_back("127.0.0.1:9090");
    auto delta = wl2::permissionSetDelta(approved, requested);
    if (delta.listen.size() != 1 || delta.listen[0] != "127.0.0.1:9090") {
        return fail("listen delta was not computed");
    }
    if (wl2::permissionSetContains(approved, requested)) {
        return fail("broader request should not be contained");
    }

    return 0;
}

int store_tests() {
    const auto root = temp_root();
    const auto storePath = root / "trust.json";

    wl2::TrustRecord record;
    record.id = "tr_test";
    record.path = root / "script.js";
    record.displayPath = "script.js";
    record.sha256 = "abc";
    record.permissions = wl2::trustPermissionsFromPermissionSet(
        permissions({"127.0.0.1:8080"}),
        {"${HOME}/demo"});
    record.grantedAt = "2026-07-03T12:00:00Z";
    record.lastUsedAt = "2026-07-03T12:00:00Z";

    wl2::TrustStore store;
    store.addOrUpdate(record);
    if (auto saved = store.save(storePath); !saved) {
        return fail("save failed: " + saved.error().message());
    }

    auto loaded = wl2::TrustStore::load(storePath);
    if (!loaded) {
        return fail("load failed: " + loaded.error().message());
    }
    if (loaded.value().records().size() != 1) {
        return fail("unexpected record count after load");
    }
    const auto& loadedRecord = loaded.value().records()[0];
    if (loadedRecord.permissions.filesystemRead.empty()
        || loadedRecord.permissions.filesystemRead[0].raw != "${HOME}/demo"
        || loadedRecord.permissions.filesystemRead[0].resolved != "/tmp/demo") {
        return fail("filesystem raw/resolved values did not round-trip");
    }

    wl2::PermissionSet requested;
    requested.network = {"127.0.0.1:1234"};
    requested.listen = {"127.0.0.1:8080"};
    requested.sharedMemory = {"/wl2/demo/frames"};
    requested.filesystemRead = {"/tmp/demo/nested.txt"};
    requested.graphics = true;
    auto match = loaded.value().match(record.path, "abc", requested);
    if (match.kind != wl2::TrustMatchKind::Exact || !match.record) {
        return fail("expected exact trust match");
    }

    requested.listen.push_back("127.0.0.1:9090");
    match = loaded.value().match(record.path, "abc", requested);
    if (match.kind != wl2::TrustMatchKind::BroaderPermissions
        || match.delta.listen.size() != 1
        || match.delta.listen[0] != "127.0.0.1:9090") {
        return fail("expected broader permission match");
    }

    match = loaded.value().match(root / "moved.js", "abc", permissions({"127.0.0.1:8080"}));
    if (match.kind != wl2::TrustMatchKind::SameHashDifferentPath) {
        return fail("expected same-hash moved-path match");
    }

    match = loaded.value().match(record.path, "def", permissions({"127.0.0.1:8080"}));
    if (match.kind != wl2::TrustMatchKind::SamePathDifferentHash) {
        return fail("expected same-path changed-hash match");
    }

    if (!loaded.value().revoke("tr_test") || !loaded.value().records().empty()) {
        return fail("revoke did not remove the record");
    }

    std::ofstream malformed(storePath, std::ios::binary | std::ios::trunc);
    malformed << "{";
    malformed.close();
    auto bad = wl2::TrustStore::load(storePath);
    if (bad || bad.error().code() != "trust_store_invalid_json") {
        return fail("malformed JSON should fail with trust_store_invalid_json");
    }

    fs::remove_all(root);
    return 0;
}

} // namespace

int wl2_trust_store_tests_entry() {
    if (int rc = permission_tests(); rc != 0) {
        return rc;
    }
    if (int rc = store_tests(); rc != 0) {
        return rc;
    }
    std::cout << "trust_store ok\n";
    return 0;
}
