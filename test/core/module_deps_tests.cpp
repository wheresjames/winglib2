#include "wl2/module_deps.h"

#include "wl2/hash.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
    std::cerr << "module deps test failed: " << message << '\n';
    return 1;
}

bool git_available() {
    return std::system("git --version >/dev/null 2>&1") == 0;
}

// Run a command in a directory, returning true on success.
bool run(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

int run_tests() {
    if (!git_available()) {
        std::cout << "module_deps skipped (git unavailable)\n";
        return 0;
    }

    const fs::path root = fs::temp_directory_path() / fs::path("wl2-deps-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // Build a tiny git repository with a tagged commit.
    const fs::path repo = root / "repo";
    fs::create_directories(repo);
    const std::string g = "git -C " + repo.string();
    if (!run(g + " init -q")
        || !run(g + " config user.email test@example.com")
        || !run(g + " config user.name Test")) {
        return fail("could not initialize git repo");
    }
    {
        std::ofstream file(repo / "module.txt");
        file << "fixture module\n";
    }
    if (!run(g + " add module.txt")
        || !run(g + " commit -q -m initial")
        || !run(g + " tag v1.0.0")) {
        return fail("could not create tagged commit");
    }

    // Tag resolves to a commit.
    auto commit = wl2::resolveGitCommit(repo.string(), "v1.0.0");
    if (!commit) {
        return fail("resolveGitCommit failed: " + commit.error().code());
    }
    if (commit.value().size() < 40) {
        return fail("resolved commit is not a full SHA: " + commit.value());
    }

    // Locking is stable across runs.
    std::vector<wl2::ModuleDependency> deps;
    wl2::ModuleDependency dep;
    dep.name = "wl2_fixture";
    dep.git = repo.string();
    dep.tag = "v1.0.0";
    deps.push_back(dep);

    auto lock1 = wl2::lockModuleDependencies(deps);
    auto lock2 = wl2::lockModuleDependencies(deps);
    if (!lock1 || !lock2) {
        return fail("lockModuleDependencies failed");
    }
    if (lock1.value().modules.size() != 1
        || lock1.value().modules[0].commit != commit.value()
        || lock1.value().modules[0].commit != lock2.value().modules[0].commit) {
        return fail("lockfile is not stable");
    }

    // Lockfile round-trips through disk.
    const fs::path lockPath = root / "wl2.lock.yml";
    if (auto rc = wl2::writeLockfile(lockPath, lock1.value()); !rc) {
        return fail("writeLockfile failed");
    }
    auto reloaded = wl2::loadLockfile(lockPath);
    if (!reloaded || reloaded.value().modules.size() != 1
        || reloaded.value().modules[0].commit != commit.value()
        || reloaded.value().schema != "wl2.lock.v1") {
        return fail("lockfile did not round-trip");
    }

    // Fetch into project-local dependency storage at the locked commit.
    const fs::path depsRoot = root / "project" / ".wl2" / "deps";
    if (auto rc = wl2::fetchLockedModule(reloaded.value().modules[0], depsRoot); !rc) {
        return fail("fetchLockedModule failed: " + rc.error().code());
    }
    if (!fs::is_directory(depsRoot / "wl2_fixture" / ".git")) {
        return fail("dependency was not fetched into project-local storage");
    }
    if (!fs::is_regular_file(depsRoot / "wl2_fixture" / "module.txt")) {
        return fail("fetched dependency is missing its checked-out content");
    }

    // Status reports the dependency as locked and fetched.
    auto statuses = wl2::dependencyStatus(deps, reloaded.value(), depsRoot);
    if (statuses.size() != 1 || statuses[0].lockedCommit != commit.value() || !statuses[0].fetched) {
        return fail("dependency status is incorrect");
    }

    // Transitive lock captures a tree checksum and bumps the lockfile schema.
    const fs::path checksumDeps = root / "checksum" / ".wl2" / "deps";
    auto tlock = wl2::lockModuleDependenciesTransitive(deps, checksumDeps);
    if (!tlock) {
        return fail("transitive lock failed: " + tlock.error().code());
    }
    if (tlock.value().schema != "wl2.lock.v2"
        || tlock.value().modules.size() != 1
        || tlock.value().modules[0].treeChecksum.empty()) {
        return fail("transitive lock did not record a tree checksum");
    }

    // The checksum round-trips through disk.
    const fs::path checksumLock = root / "wl2.lock.v2.yml";
    if (auto rc = wl2::writeLockfile(checksumLock, tlock.value()); !rc) {
        return fail("writeLockfile (v2) failed");
    }
    auto reloadedV2 = wl2::loadLockfile(checksumLock);
    if (!reloadedV2 || reloadedV2.value().schema != "wl2.lock.v2"
        || reloadedV2.value().modules[0].treeChecksum != tlock.value().modules[0].treeChecksum) {
        return fail("v2 lockfile did not round-trip the tree checksum");
    }

    // A clean checkout verifies; tampering with a tracked file is rejected.
    if (auto rc = wl2::verifyFetchedCommit(reloadedV2.value().modules[0], checksumDeps); !rc) {
        return fail("verifyFetchedCommit rejected a clean checkout: " + rc.error().code());
    }
    {
        std::ofstream tampered(checksumDeps / "wl2_fixture" / "module.txt", std::ios::app);
        tampered << "tampered\n";
    }
    auto dirty = wl2::verifyFetchedCommit(reloadedV2.value().modules[0], checksumDeps);
    if (dirty || dirty.error().code() != "deps_tree_dirty") {
        return fail("verifyFetchedCommit did not detect a tampered checkout");
    }

    // A git URL that looks like an option is rejected, not handed to git.
    if (wl2::resolveGitCommit("--upload-pack=touch", "v1.0.0")) {
        return fail("option-like git URL was accepted by resolveGitCommit");
    }

    // A traversing dependency name must not reach the destructive fetch path.
    wl2::LockedModule evilName = reloaded.value().modules[0];
    evilName.name = "../escape";
    if (auto rc = wl2::fetchLockedModule(evilName, depsRoot); rc) {
        return fail("traversing dependency name was fetched");
    }
    if (fs::exists(depsRoot / ".." / "escape")) {
        fs::remove_all(depsRoot / ".." / "escape");
        return fail("fetch escaped the dependency root");
    }

    // An option-like git URL is rejected by the fetch path too.
    wl2::LockedModule evilUrl = reloaded.value().modules[0];
    evilUrl.git = "--upload-pack=touch";
    if (auto rc = wl2::fetchLockedModule(evilUrl, depsRoot); rc) {
        return fail("option-like git URL was fetched");
    }

    fs::remove_all(root);
    std::cout << "module_deps ok\n";
    return 0;
}

// Write a fetched module's source metadata under <depsRoot>/<name>.
void write_source_metadata(
    const fs::path& depsRoot,
    const std::string& name,
    const std::string& provides,
    const std::vector<std::pair<std::string, std::string>>& sourceDeps) {
    fs::create_directories(depsRoot / name);
    std::ofstream out(depsRoot / name / "wl2.module.source.yml");
    out << "schema: wl2.module-source.v1\nprovides: " << provides << "\nversion: 1.0.0\n";
    if (!sourceDeps.empty()) {
        out << "sourceDependencies:\n  modules:\n";
        for (const auto& [depName, depGit] : sourceDeps) {
            out << "    - name: " << depName << "\n      git: " << depGit
                << "\n      tag: v1.0.0\n";
        }
    }
}

wl2::LockedModule locked(const std::string& name) {
    wl2::LockedModule m;
    m.name = name;
    m.git = "https://example.com/" + name + ".git";
    m.commit = "0000000000000000000000000000000000000000";
    return m;
}

// orderLockedModulesForBuild sorts dependency-first using fetched source metadata.
int order_test() {
    const fs::path root = fs::temp_directory_path() / fs::path("wl2-order-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path depsRoot = root / "deps";
    write_source_metadata(depsRoot, "app", "wl2:app", {{"base", "https://example.com/base.git"}});
    write_source_metadata(depsRoot, "base", "wl2:base", {});

    wl2::Lockfile lock;
    lock.modules = {locked("app"), locked("base")};
    auto ordered = wl2::orderLockedModulesForBuild(lock, depsRoot);
    if (!ordered) {
        fs::remove_all(root);
        return fail("ordering failed: " + ordered.error().code());
    }
    if (ordered.value().size() != 2
        || ordered.value()[0].name != "base"
        || ordered.value()[1].name != "app") {
        fs::remove_all(root);
        return fail("build order was not dependency-first");
    }

    // A source dependency cycle is rejected.
    write_source_metadata(depsRoot, "app", "wl2:app", {{"base", "https://example.com/base.git"}});
    write_source_metadata(depsRoot, "base", "wl2:base", {{"app", "https://example.com/app.git"}});
    auto cyclic = wl2::orderLockedModulesForBuild(lock, depsRoot);
    fs::remove_all(root);
    if (cyclic || cyclic.error().code() != "deps_source_cycle") {
        return fail("source dependency cycle was not detected");
    }
    return 0;
}

// Source trust policy and content hashing need no git or network.
int trust_and_hash_test() {
    // Known SHA-256 vectors.
    if (wl2::sha256Hex("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        return fail("sha256 of empty string is wrong");
    }
    if (wl2::sha256Hex("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        return fail("sha256 of 'abc' is wrong");
    }

    wl2::SourceTrustPolicy policy;  // defaults: local + secure transports allowed.
    if (!wl2::checkSourceTrust("https://example.com/x.git", policy)) {
        return fail("https source rejected by default policy");
    }
    if (!wl2::checkSourceTrust("git@example.com:group/x.git", policy)) {
        return fail("ssh/scp source rejected by default policy");
    }
    if (!wl2::checkSourceTrust("/tmp/local/repo", policy)) {
        return fail("local path rejected by default policy");
    }
    if (wl2::checkSourceTrust("http://example.com/x.git", policy)) {
        return fail("cleartext http source accepted by default policy");
    }
    if (wl2::checkSourceTrust("git://example.com/x.git", policy)) {
        return fail("cleartext git source accepted by default policy");
    }
    // Case-folded schemes must not evade the cleartext check (git is
    // case-insensitive about schemes, so the trust policy must be too).
    if (wl2::checkSourceTrust("HTTP://example.com/x.git", policy)) {
        return fail("upper-case http scheme bypassed the cleartext check");
    }
    if (wl2::checkSourceTrust("Git://example.com/x.git", policy)) {
        return fail("mixed-case git scheme bypassed the cleartext check");
    }

    // Insecure transport is permitted only when explicitly allowed.
    wl2::SourceTrustPolicy insecure;
    insecure.allowInsecureTransport = true;
    if (!wl2::checkSourceTrust("http://example.com/x.git", insecure)) {
        return fail("http rejected even with allowInsecureTransport");
    }

    // Local paths can be denied for stricter reproducible builds.
    wl2::SourceTrustPolicy noLocal;
    noLocal.allowLocalPaths = false;
    if (auto rc = wl2::checkSourceTrust("/tmp/repo", noLocal); rc) {
        return fail("local path accepted when allowLocalPaths is false");
    } else if (rc.error().code() != "deps_untrusted_local") {
        return fail("unexpected error code for denied local path: " + rc.error().code());
    }

    // A host allow-list constrains remote sources.
    wl2::SourceTrustPolicy hosts;
    hosts.allowedHosts = {"github.com"};
    if (!wl2::checkSourceTrust("https://github.com/o/x.git", hosts)) {
        return fail("allowed host was rejected");
    }
    // Host matching is case-insensitive, like DNS.
    if (!wl2::checkSourceTrust("https://GitHub.com/o/x.git", hosts)) {
        return fail("allowed host rejected due to case sensitivity");
    }
    if (auto rc = wl2::checkSourceTrust("https://evil.example/x.git", hosts); rc) {
        return fail("host outside the allow-list was accepted");
    } else if (rc.error().code() != "deps_untrusted_host") {
        return fail("unexpected error code for disallowed host: " + rc.error().code());
    }

    return 0;
}

} // namespace

int wl2_module_deps_tests_entry() {
    if (int rc = trust_and_hash_test(); rc != 0) {
        return rc;
    }
    if (int rc = order_test(); rc != 0) {
        return rc;
    }
    return run_tests();
}
