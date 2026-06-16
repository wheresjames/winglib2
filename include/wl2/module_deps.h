#pragma once

/**
 * @file module_deps.h
 * @brief Git-pinned module dependencies, lockfile, and fetch.
 */

#include <filesystem>
#include <string>
#include <vector>

#include "wl2/errors.h"
#include "wl2/manifest.h"

namespace wl2 {

/// A module dependency resolved to an immutable commit, recorded in the lockfile.
struct LockedModule {
    std::string name;    ///< Dependency name.
    std::string git;     ///< Git remote URL.
    std::string tag;     ///< Pinned tag this commit was resolved from.
    std::string commit;  ///< Immutable resolved commit SHA.
    std::string path;    ///< Subpath within the repository, if any.
    /// Canonical module name the repository provides, when known.
    std::string provides;
    /// Git tree object id of the locked commit (`<commit>^{tree}`), captured when
    /// the source is fetched during locking. Empty for older `wl2.lock.v1`
    /// lockfiles or when the tree could not be read. When present, build refuses
    /// a checkout whose tree does not match, hardening reproducible builds.
    std::string treeChecksum;
};

/// Parsed lockfile. Schema `wl2.lock.v1` has no checksums; `wl2.lock.v2` adds the
/// per-module `tree:` checksum.
struct Lockfile {
    std::string schema = "wl2.lock.v1";     ///< Lockfile schema identifier.
    std::vector<LockedModule> modules;      ///< Locked module entries.

    /// Find a locked module by @p name, or nullptr if absent.
    const LockedModule* find(const std::string& name) const;
};

/**
 * @brief Policy controlling which Git sources a project may fetch from.
 *
 * Source dependencies pull code from remote repositories, so the transport and
 * host are gated before any clone/fetch. Defaults permit local paths (you
 * control your own filesystem) and authenticated/secure transports, and reject
 * cleartext transports unless explicitly allowed.
 */
struct SourceTrustPolicy {
    /// Permit local filesystem paths and `file://` URLs.
    bool allowLocalPaths = true;
    /// Permit cleartext transports (`http://`, `git://`).
    bool allowInsecureTransport = false;
    /// When non-empty, only these remote hosts are permitted. Local paths are not
    /// subject to the host list.
    std::vector<std::string> allowedHosts;
};

/// Build a SourceTrustPolicy from the environment:
/// `WL2_DEPS_ALLOW_INSECURE=1` permits cleartext transports,
/// `WL2_DEPS_DENY_LOCAL=1` rejects local paths, and
/// `WL2_DEPS_ALLOWED_HOSTS=a,b` restricts remote hosts.
SourceTrustPolicy sourceTrustPolicyFromEnv();

/// Verify a Git source URL is permitted by @p policy. Returns a stable error
/// (`deps_untrusted_transport`, `deps_untrusted_host`, or `deps_untrusted_local`)
/// when the source is not allowed.
Result<void> checkSourceTrust(const std::string& gitUrl, const SourceTrustPolicy& policy);

/// Read a lockfile. Returns an error if the file is missing or malformed.
Result<Lockfile> loadLockfile(const std::filesystem::path& path);

/// Write a lockfile deterministically (modules sorted by name).
Result<void> writeLockfile(const std::filesystem::path& path, const Lockfile& lock);

/// Resolve a Git tag to a commit SHA via `git ls-remote`.
Result<std::string> resolveGitCommit(const std::string& gitUrl, const std::string& tag);

/// Build a lockfile from manifest dependencies, resolving each tag to a commit.
Result<Lockfile> lockModuleDependencies(const std::vector<ModuleDependency>& dependencies);

/// Build a lockfile from manifest dependencies including transitive source
/// dependencies. Each dependency is resolved to a commit and fetched into
/// `<depsRoot>/<name>` so its `wl2.module.source.yml` can be read; declared
/// `sourceDependencies.modules` are added to the closure and locked as well.
Result<Lockfile> lockModuleDependenciesTransitive(
    const std::vector<ModuleDependency>& dependencies,
    const std::filesystem::path& depsRoot);

/// Order locked modules dependency-first using the `sourceDependencies` declared
/// in each fetched module's `wl2.module.source.yml`. Modules with no discoverable
/// metadata keep their lockfile order. Fails on a source dependency cycle.
Result<std::vector<LockedModule>> orderLockedModulesForBuild(
    const Lockfile& lock,
    const std::filesystem::path& depsRoot);

/// Fetch a locked module into `<depsRoot>/<name>` checked out at its commit.
Result<void> fetchLockedModule(const LockedModule& locked, const std::filesystem::path& depsRoot);

/// Verify a fetched module checkout is present and matches its locked commit.
/// Returns an error when the checkout is missing or at a different commit. When
/// the lockfile records a tree checksum, the checkout's tree must match it and
/// the working tree must be clean (no local modifications), so a tampered
/// checkout is rejected before it is built.
Result<void> verifyFetchedCommit(const LockedModule& locked, const std::filesystem::path& depsRoot);

/// Per-dependency status used by `wl2 deps status`.
struct DependencyStatus {
    std::string name;          ///< Dependency name.
    std::string tag;           ///< Declared tag.
    std::string lockedCommit;  ///< Empty when not present in the lockfile.
    bool fetched = false;      ///< True when fetched at the locked commit.
    std::string provides;      ///< Canonical module name, when known.
    bool built = false;        ///< True when a built module library is present.
    bool installed = false;    ///< True when installed into the local scope.
    bool selected = false;     ///< True when the resolver selects this provider.
};

/// Compute the status of each manifest dependency against a lockfile and the
/// project-local dependency storage.
std::vector<DependencyStatus> dependencyStatus(
    const std::vector<ModuleDependency>& dependencies,
    const Lockfile& lock,
    const std::filesystem::path& depsRoot);

} // namespace wl2
