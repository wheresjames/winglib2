#include "wl2/module_deps.h"

#include "wl2/module_resolver.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace wl2 {

namespace {

std::string trim(std::string_view value) {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    auto end = value.find_last_not_of(" \t\r\n");
    auto out = std::string(value.substr(begin, end - begin + 1));
    if (out.size() >= 2
        && ((out.front() == '"' && out.back() == '"') || (out.front() == '\'' && out.back() == '\''))) {
        return out.substr(1, out.size() - 2);
    }
    return out;
}

// An argument that begins with '-' can be misread by git as an option even when
// it is shell-quoted, so untrusted URLs and commits must not look like options.
bool looks_like_option(const std::string& value) {
    return !value.empty() && value.front() == '-';
}

// A dependency name is joined onto the dependency root and then removed and
// recreated, so it must be a single path segment that cannot traverse out.
bool is_safe_dependency_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".."
        || name.find('/') != std::string::npos
        || name.find('\\') != std::string::npos
        || name.find('\0') != std::string::npos) {
        return false;
    }
    return true;
}

// Single-quote an argument for the shell, escaping embedded single quotes.
std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

struct GitResult {
    int status = -1;
    std::string output;
};

GitResult run_git(const std::vector<std::string>& args) {
#if defined(_WIN32)
    (void)args;
    return GitResult{-1, ""};
#else
    std::string command = "git";
    for (const auto& arg : args) {
        command += ' ';
        command += shell_quote(arg);
    }
    command += " 2>/dev/null";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return GitResult{-1, ""};
    }
    std::string output;
    std::array<char, 4096> buffer{};
    size_t read = 0;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        output.append(buffer.data(), read);
    }
    int rc = pclose(pipe);
    int status = (rc == -1 || !WIFEXITED(rc)) ? -1 : WEXITSTATUS(rc);
    return GitResult{status, output};
#endif
}

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// Classification of a Git source URL for trust evaluation.
enum class SourceKind { Local, SecureRemote, InsecureRemote };

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string to_lower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

// Classify the source by a case-folded copy of the URL: Git treats URL schemes
// and host names case-insensitively, so trust decisions must too, or an
// upper-case scheme like "HTTP://" would evade the cleartext/host checks while
// Git still fetched it. The original URL is what is handed to Git unchanged.
SourceKind classify_source(const std::string& rawUrl, std::string& hostOut) {
    hostOut.clear();
    const std::string url = to_lower(rawUrl);
    auto remote_host = [&](const std::string& afterScheme) {
        // host is up to the next '/', ':' (port/path), or '@' boundary handled.
        std::string rest = afterScheme;
        auto at = rest.find('@');
        if (at != std::string::npos) {
            rest = rest.substr(at + 1);
        }
        auto end = rest.find_first_of("/:");
        hostOut = end == std::string::npos ? rest : rest.substr(0, end);
    };

    if (starts_with(url, "https://")) {
        remote_host(url.substr(std::string("https://").size()));
        return SourceKind::SecureRemote;
    }
    if (starts_with(url, "ssh://")) {
        remote_host(url.substr(std::string("ssh://").size()));
        return SourceKind::SecureRemote;
    }
    if (starts_with(url, "http://")) {
        remote_host(url.substr(std::string("http://").size()));
        return SourceKind::InsecureRemote;
    }
    if (starts_with(url, "git://")) {
        remote_host(url.substr(std::string("git://").size()));
        return SourceKind::InsecureRemote;
    }
    if (starts_with(url, "file://")) {
        return SourceKind::Local;
    }
    // scp-like syntax: user@host:path (no scheme, has ':' before any '/').
    auto colon = url.find(':');
    auto slash = url.find('/');
    if (colon != std::string::npos && url.find("://") == std::string::npos
        && (slash == std::string::npos || colon < slash)) {
        std::string before = url.substr(0, colon);
        auto at = before.find('@');
        hostOut = at == std::string::npos ? before : before.substr(at + 1);
        return SourceKind::SecureRemote;
    }
    // Anything else is treated as a local filesystem path.
    return SourceKind::Local;
}

} // namespace

SourceTrustPolicy sourceTrustPolicyFromEnv() {
    SourceTrustPolicy policy;
    policy.allowInsecureTransport = env_value("WL2_DEPS_ALLOW_INSECURE") == "1";
    policy.allowLocalPaths = env_value("WL2_DEPS_DENY_LOCAL") != "1";
    std::string hosts = env_value("WL2_DEPS_ALLOWED_HOSTS");
    std::string current;
    for (char ch : hosts) {
        if (ch == ',' || ch == ' ') {
            if (!current.empty()) {
                policy.allowedHosts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        policy.allowedHosts.push_back(current);
    }
    return policy;
}

Result<void> checkSourceTrust(const std::string& gitUrl, const SourceTrustPolicy& policy) {
    std::string host;
    const SourceKind kind = classify_source(gitUrl, host);
    if (kind == SourceKind::Local) {
        if (!policy.allowLocalPaths) {
            return Error("deps_untrusted_local",
                "Local source paths are not permitted by policy: " + gitUrl);
        }
        return {};
    }
    if (kind == SourceKind::InsecureRemote && !policy.allowInsecureTransport) {
        return Error("deps_untrusted_transport",
            "Insecure source transport (http/git) is not permitted by policy: " + gitUrl);
    }
    bool hostAllowed = false;
    for (const auto& allowed : policy.allowedHosts) {
        if (to_lower(allowed) == host) {  // host is already case-folded
            hostAllowed = true;
            break;
        }
    }
    if (!policy.allowedHosts.empty() && !hostAllowed) {
        return Error("deps_untrusted_host",
            "Source host is not in the allowed-hosts list: " + (host.empty() ? gitUrl : host));
    }
    return {};
}

const LockedModule* Lockfile::find(const std::string& name) const {
    for (const auto& module : modules) {
        if (module.name == name) {
            return &module;
        }
    }
    return nullptr;
}

Result<std::string> resolveGitCommit(const std::string& gitUrl, const std::string& tag) {
    if (looks_like_option(gitUrl)) {
        return Error("deps_invalid_git_url", "Git URL must not begin with '-': " + gitUrl);
    }
    if (auto trusted = checkSourceTrust(gitUrl, sourceTrustPolicyFromEnv()); !trusted) {
        return trusted.error();
    }
    // Query the peeled commit first (annotated tags) and the tag ref itself.
    GitResult result = run_git({"ls-remote", "--", gitUrl,
        "refs/tags/" + tag + "^{}",
        "refs/tags/" + tag});
    if (result.status != 0) {
        return Error("deps_git_failed", "git ls-remote failed for " + gitUrl);
    }

    std::string peeled;
    std::string plain;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        std::string sha = trim(std::string_view(line).substr(0, tab));
        std::string ref = trim(std::string_view(line).substr(tab + 1));
        if (ref.size() >= 3 && ref.compare(ref.size() - 3, 3, "^{}") == 0) {
            peeled = sha;
        } else {
            plain = sha;
        }
    }
    std::string commit = !peeled.empty() ? peeled : plain;
    if (commit.empty()) {
        return Error("deps_tag_not_found", "Tag not found in " + gitUrl + ": " + tag);
    }
    return commit;
}

Result<Lockfile> lockModuleDependencies(const std::vector<ModuleDependency>& dependencies) {
    Lockfile lock;
    for (const auto& dep : dependencies) {
        LockedModule locked;
        locked.name = dep.name;
        locked.git = dep.git;
        locked.tag = dep.tag;
        locked.path = dep.path;
        if (!dep.commit.empty()) {
            locked.commit = dep.commit;
        } else {
            auto commit = resolveGitCommit(dep.git, dep.tag);
            if (!commit) {
                return commit.error();
            }
            locked.commit = commit.value();
        }
        lock.modules.push_back(std::move(locked));
    }
    std::sort(lock.modules.begin(), lock.modules.end(),
        [](const LockedModule& a, const LockedModule& b) { return a.name < b.name; });
    return lock;
}

Result<void> writeLockfile(const std::filesystem::path& path, const Lockfile& lock) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Error("lockfile_write_failed", "Unable to write lockfile: " + path.string());
    }
    out << "schema: " << lock.schema << "\n";
    out << "modules:\n";
    auto sorted = lock.modules;
    std::sort(sorted.begin(), sorted.end(),
        [](const LockedModule& a, const LockedModule& b) { return a.name < b.name; });
    for (const auto& module : sorted) {
        out << "  - name: " << module.name << "\n";
        out << "    git: " << module.git << "\n";
        if (!module.tag.empty()) {
            out << "    tag: " << module.tag << "\n";
        }
        out << "    commit: " << module.commit << "\n";
        if (!module.treeChecksum.empty()) {
            out << "    tree: " << module.treeChecksum << "\n";
        }
        if (!module.path.empty()) {
            out << "    path: " << module.path << "\n";
        }
        if (!module.provides.empty()) {
            out << "    provides: " << module.provides << "\n";
        }
    }
    return {};
}

Result<Lockfile> loadLockfile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return Error("lockfile_not_found", "Unable to open lockfile: " + path.string());
    }
    Lockfile lock;
    lock.schema.clear();
    std::string line;
    bool inModules = false;
    while (std::getline(in, line)) {
        std::string text = trim(line);
        if (text.empty()) {
            continue;
        }
        auto indent = line.find_first_not_of(' ');
        if (indent == 0) {
            auto colon = text.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            auto key = trim(std::string_view(text).substr(0, colon));
            auto value = trim(std::string_view(text).substr(colon + 1));
            if (key == "schema") {
                lock.schema = value;
            }
            inModules = (key == "modules");
            continue;
        }
        if (!inModules) {
            continue;
        }
        auto colon = text.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (text.rfind("- ", 0) == 0) {
            lock.modules.emplace_back();
            text = trim(std::string_view(text).substr(2));
            colon = text.find(':');
            if (colon == std::string::npos) {
                continue;
            }
        }
        if (lock.modules.empty()) {
            continue;
        }
        auto key = trim(std::string_view(text).substr(0, colon));
        auto value = trim(std::string_view(text).substr(colon + 1));
        auto& module = lock.modules.back();
        if (key == "name") module.name = value;
        else if (key == "git") module.git = value;
        else if (key == "tag") module.tag = value;
        else if (key == "commit") module.commit = value;
        else if (key == "tree") module.treeChecksum = value;
        else if (key == "path") module.path = value;
        else if (key == "provides") module.provides = value;
    }
    if (lock.schema != "wl2.lock.v1" && lock.schema != "wl2.lock.v2") {
        return Error("lockfile_invalid_schema", "Unsupported lockfile schema: " + lock.schema);
    }
    return lock;
}

Result<void> fetchLockedModule(const LockedModule& locked, const std::filesystem::path& depsRoot) {
    if (locked.commit.empty()) {
        return Error("deps_unlocked", "Dependency is not locked to a commit: " + locked.name);
    }
    // The name is joined onto depsRoot and the result is removed and recreated;
    // a traversing name could delete or write outside the dependency root.
    if (!is_safe_dependency_name(locked.name)) {
        return Error("deps_invalid_name", "Unsafe dependency name: " + locked.name);
    }
    if (looks_like_option(locked.git)) {
        return Error("deps_invalid_git_url", "Git URL must not begin with '-': " + locked.git);
    }
    if (looks_like_option(locked.commit)) {
        return Error("deps_invalid_commit", "Commit must not begin with '-': " + locked.commit);
    }
    if (auto trusted = checkSourceTrust(locked.git, sourceTrustPolicyFromEnv()); !trusted) {
        return trusted.error();
    }
    std::error_code ec;
    std::filesystem::create_directories(depsRoot, ec);
    const auto target = depsRoot / locked.name;

    if (std::filesystem::is_directory(target / ".git", ec)) {
        // Already cloned: make sure the requested commit is checked out.
        GitResult head = run_git({"-C", target.string(), "rev-parse", "HEAD"});
        if (head.status == 0 && trim(head.output) == locked.commit) {
            return {};
        }
        run_git({"-C", target.string(), "fetch", "--all", "--tags"});
    } else {
        std::filesystem::remove_all(target, ec);
        GitResult clone = run_git({"clone", "--", locked.git, target.string()});
        if (clone.status != 0) {
            return Error("deps_clone_failed", "git clone failed for " + locked.name + " (" + locked.git + ")");
        }
    }

    GitResult checkout = run_git({"-C", target.string(), "checkout", "--quiet", locked.commit});
    if (checkout.status != 0) {
        return Error("deps_checkout_failed",
            "git checkout failed for " + locked.name + " at " + locked.commit);
    }
    return {};
}

Result<void> verifyFetchedCommit(const LockedModule& locked, const std::filesystem::path& depsRoot) {
    if (!is_safe_dependency_name(locked.name)) {
        return Error("deps_invalid_name", "Unsafe dependency name: " + locked.name);
    }
    const auto target = depsRoot / locked.name;
    std::error_code ec;
    if (!std::filesystem::is_directory(target / ".git", ec)) {
        return Error("deps_not_fetched", "Dependency is not fetched: " + locked.name);
    }
    GitResult head = run_git({"-C", target.string(), "rev-parse", "HEAD"});
    if (head.status != 0 || trim(head.output) != locked.commit) {
        return Error("deps_commit_mismatch",
            "Fetched checkout for " + locked.name + " does not match the locked commit");
    }
    // When the lockfile records a content checksum, the checkout must match it
    // exactly: the committed tree id must equal the locked tree, and the working
    // tree must be clean so locally modified files cannot be built.
    if (!locked.treeChecksum.empty()) {
        GitResult tree = run_git({"-C", target.string(), "rev-parse", "HEAD^{tree}"});
        if (tree.status != 0 || trim(tree.output) != locked.treeChecksum) {
            return Error("deps_tree_mismatch",
                "Fetched checkout for " + locked.name + " does not match the locked tree checksum");
        }
        GitResult dirty = run_git({"-C", target.string(), "status", "--porcelain"});
        if (dirty.status != 0 || !trim(dirty.output).empty()) {
            return Error("deps_tree_dirty",
                "Fetched checkout for " + locked.name + " has local modifications");
        }
    }
    return {};
}

namespace {

// Read a fetched module's source metadata, if present, from
// `<depsRoot>/<name>/<path>/wl2.module.source.yml`.
Result<ModuleSourceMetadata> read_fetched_source_metadata(
    const LockedModule& locked, const std::filesystem::path& depsRoot) {
    auto sourceRoot = depsRoot / locked.name;
    if (!locked.path.empty()) {
        sourceRoot /= locked.path;
    }
    const auto metaPath = sourceRoot / "wl2.module.source.yml";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(metaPath, ec)) {
        return Error("deps_no_source_metadata", "No wl2.module.source.yml for " + locked.name);
    }
    return loadModuleSourceMetadata(metaPath);
}

} // namespace

Result<Lockfile> lockModuleDependenciesTransitive(
    const std::vector<ModuleDependency>& dependencies,
    const std::filesystem::path& depsRoot) {
    Lockfile lock;
    std::set<std::string> locked;
    std::deque<ModuleDependency> queue(dependencies.begin(), dependencies.end());

    while (!queue.empty()) {
        ModuleDependency dep = queue.front();
        queue.pop_front();
        if (dep.name.empty() || locked.count(dep.name)) {
            continue;
        }

        LockedModule module;
        module.name = dep.name;
        module.git = dep.git;
        module.tag = dep.tag;
        module.path = dep.path;
        module.provides = dep.provides;
        if (!dep.commit.empty()) {
            module.commit = dep.commit;
        } else {
            auto commit = resolveGitCommit(dep.git, dep.tag);
            if (!commit) {
                return commit.error();
            }
            module.commit = commit.value();
        }

        // Fetch so transitive dependencies can be discovered from the checked-out
        // source metadata. Fetching during lock is intentional; resolution stays
        // out of `wl2 run`.
        if (auto fetched = fetchLockedModule(module, depsRoot); !fetched) {
            return fetched.error();
        }
        // Record the git tree object id of the locked commit as a content
        // checksum, verified before build to detect a tampered checkout.
        GitResult tree = run_git({"-C", (depsRoot / module.name).string(),
            "rev-parse", module.commit + "^{tree}"});
        if (tree.status == 0) {
            module.treeChecksum = trim(tree.output);
        }
        if (auto metadata = read_fetched_source_metadata(module, depsRoot)) {
            if (module.provides.empty()) {
                module.provides = metadata.value().provides;
            }
            for (const auto& sourceDep : metadata.value().sourceDependencies) {
                if (!sourceDep.name.empty() && !locked.count(sourceDep.name)) {
                    queue.push_back(sourceDep);
                }
            }
        }

        locked.insert(module.name);
        lock.modules.push_back(std::move(module));
    }

    // Any captured checksum upgrades the lockfile to the checksum-bearing schema.
    for (const auto& module : lock.modules) {
        if (!module.treeChecksum.empty()) {
            lock.schema = "wl2.lock.v2";
            break;
        }
    }
    std::sort(lock.modules.begin(), lock.modules.end(),
        [](const LockedModule& a, const LockedModule& b) { return a.name < b.name; });
    return lock;
}

Result<std::vector<LockedModule>> orderLockedModulesForBuild(
    const Lockfile& lock,
    const std::filesystem::path& depsRoot) {
    // Map repository name -> locked module and its source dependency names.
    std::map<std::string, const LockedModule*> byName;
    std::map<std::string, std::vector<std::string>> deps;
    for (const auto& module : lock.modules) {
        byName[module.name] = &module;
        std::vector<std::string> edges;
        if (auto metadata = read_fetched_source_metadata(module, depsRoot)) {
            for (const auto& sourceDep : metadata.value().sourceDependencies) {
                edges.push_back(sourceDep.name);
            }
        }
        deps[module.name] = std::move(edges);
    }

    // Kahn's algorithm over present dependency edges, deterministic by lockfile
    // order (modules are already sorted by name).
    std::map<std::string, int> indegree;
    for (const auto& module : lock.modules) {
        int count = 0;
        for (const auto& dep : deps[module.name]) {
            if (byName.count(dep)) {
                ++count;
            }
        }
        indegree[module.name] = count;
    }

    std::vector<LockedModule> ordered;
    std::set<std::string> placed;
    bool progress = true;
    while (ordered.size() < lock.modules.size() && progress) {
        progress = false;
        for (const auto& module : lock.modules) {
            if (placed.count(module.name) || indegree[module.name] != 0) {
                continue;
            }
            ordered.push_back(module);
            placed.insert(module.name);
            progress = true;
            for (const auto& other : lock.modules) {
                if (placed.count(other.name)) {
                    continue;
                }
                for (const auto& dep : deps[other.name]) {
                    if (dep == module.name) {
                        --indegree[other.name];
                    }
                }
            }
            break;
        }
    }

    if (ordered.size() != lock.modules.size()) {
        return Error("deps_source_cycle", "Source dependency cycle detected among locked modules");
    }
    return ordered;
}

std::vector<DependencyStatus> dependencyStatus(
    const std::vector<ModuleDependency>& dependencies,
    const Lockfile& lock,
    const std::filesystem::path& depsRoot) {
    std::vector<DependencyStatus> statuses;
    for (const auto& dep : dependencies) {
        DependencyStatus status;
        status.name = dep.name;
        status.tag = dep.tag;
        if (const LockedModule* locked = lock.find(dep.name)) {
            status.lockedCommit = locked->commit;
            std::error_code ec;
            const auto target = depsRoot / dep.name;
            if (std::filesystem::is_directory(target / ".git", ec)) {
                GitResult head = run_git({"-C", target.string(), "rev-parse", "HEAD"});
                status.fetched = (head.status == 0 && trim(head.output) == locked->commit);
            }
        }
        statuses.push_back(std::move(status));
    }
    return statuses;
}

} // namespace wl2
