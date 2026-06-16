#include "wl2/app_store.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <system_error>

namespace wl2 {

namespace {

constexpr const char* kAppMetaFile = "wl2.app.yml";
constexpr const char* kIndexFile = "index.yml";
constexpr const char* kCacheDir = ".cache";
constexpr const char* kBinDir = "bin";

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// An argument that begins with '-' can be misread by git as an option, so
// repository and ref values must not look like options.
bool looks_like_option(std::string_view value) {
    return !value.empty() && value.front() == '-';
}

// A subdirectory within a cloned repo is joined onto the checkout path, so it
// must stay inside the checkout: relative, no NUL, and no `..` traversal.
bool is_safe_subdir(std::string_view path) {
    if (path.find('\0') != std::string_view::npos) {
        return false;
    }
    std::filesystem::path rel(path);
    if (rel.is_absolute()) {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

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

std::map<std::string, std::string> read_flat_yaml(const std::filesystem::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = trim(std::string_view(line).substr(0, colon));
        auto value = trim(std::string_view(line).substr(colon + 1));
        if (!key.empty()) {
            values[key] = value;
        }
    }
    return values;
}

std::filesystem::path user_data_root() {
    std::string xdg = env_value("XDG_DATA_HOME");
    if (!xdg.empty()) {
        return std::filesystem::path(xdg) / "wl2";
    }
    std::string home = env_value("HOME");
    if (home.empty()) {
        return std::filesystem::path(".wl2-user") / "wl2";
    }
#if defined(__APPLE__)
    return std::filesystem::path(home) / "Library" / "Application Support" / "wl2";
#else
    return std::filesystem::path(home) / ".local" / "share" / "wl2";
#endif
}

void write_index(const std::filesystem::path& scopeRoot, const std::vector<InstalledAppRecord>& records) {
    std::error_code ec;
    std::filesystem::create_directories(scopeRoot, ec);
    std::ofstream out(scopeRoot / kIndexFile, std::ios::binary | std::ios::trunc);
    out << "schema: wl2.app-index.v1\n";
    out << "apps:\n";
    for (const auto& record : records) {
        out << "  - name: " << record.name << "\n";
        out << "    slug: " << appSlug(record.name) << "\n";
        out << "    version: " << record.version << "\n";
        out << "    executable: " << record.executablePath.filename().string() << "\n";
    }
}

Result<void> write_launcher(
    const std::filesystem::path& launcher,
    const std::filesystem::path& executable) {
    std::error_code ec;
    std::filesystem::create_directories(launcher.parent_path(), ec);
    std::ofstream out(launcher, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Error("app_install_failed", "Unable to write launcher: " + launcher.string());
    }
    out << "#!/bin/sh\n";
    out << "exec \"" << executable.string() << "\" \"$@\"\n";
    out.close();
    std::filesystem::permissions(
        launcher,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        ec);
    return {};
}

} // namespace

const std::filesystem::path& AppScopePaths::forScope(ModuleScope scope) const {
    switch (scope) {
        case ModuleScope::Local: return local;
        case ModuleScope::User: return user;
        case ModuleScope::System: return system;
    }
    return local;
}

AppScopePaths resolveAppScopePaths(const std::filesystem::path& projectRoot) {
    AppScopePaths paths;
    paths.local = projectRoot / ".wl2" / "apps";
    paths.user = user_data_root() / "apps";
    std::string override = env_value("WL2_SYSTEM_APP_DIR");
    paths.system = override.empty() ? std::filesystem::path("/usr/local/share/wl2/apps") : std::filesystem::path(override);
    return paths;
}

std::string appSlug(std::string_view name) {
    std::string slug;
    slug.reserve(name.size());
    for (char ch : name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '.' || ch == '-' || ch == '_') {
            slug.push_back(ch);
        } else {
            slug.push_back('_');
        }
    }
    return slug.empty() ? "app" : slug;
}

std::optional<AppSourceSpec> normalizeAppSource(std::string_view inputView) {
    // Reject a spec whose repo/ref could be misread by git as an option, or
    // whose subdirectory would escape the checkout when joined onto it.
    auto finish = [](AppSourceSpec s) -> std::optional<AppSourceSpec> {
        if (s.repo.empty() || looks_like_option(s.repo) || looks_like_option(s.ref)
            || !is_safe_subdir(s.path)) {
            return std::nullopt;
        }
        return s;
    };

    std::string input(inputView);
    AppSourceSpec spec;
    std::smatch githubTree;
    static const std::regex treePattern(R"(^https://github\.com/([^/]+)/([^/]+)/tree/([^/]+)(?:/(.*))?$)");
    if (std::regex_match(input, githubTree, treePattern)) {
        spec.repo = "https://github.com/" + githubTree[1].str() + "/" + githubTree[2].str() + ".git";
        spec.ref = githubTree[3].str();
        spec.path = githubTree.size() > 4 ? githubTree[4].str() : "";
        return finish(spec);
    }

    std::string rest = input;
    auto hash = rest.find('#');
    if (hash != std::string::npos) {
        spec.repo = rest.substr(0, hash);
        rest = rest.substr(hash + 1);
        auto colon = rest.find(':');
        if (colon == std::string::npos) {
            spec.ref = rest;
        } else {
            spec.ref = rest.substr(0, colon);
            spec.path = rest.substr(colon + 1);
        }
        return finish(spec);
    }

    auto colon = rest.rfind(':');
    if (colon != std::string::npos && rest.find("://") == std::string::npos && rest.find(':') == colon) {
        spec.repo = rest.substr(0, colon);
        spec.path = rest.substr(colon + 1);
    } else {
        spec.repo = rest;
    }
    return finish(spec);
}

AppStore::AppStore(AppScopePaths paths) : paths_(std::move(paths)) {}

std::vector<InstalledAppRecord> AppStore::scanScope(ModuleScope scope) const {
    std::vector<InstalledAppRecord> records;
    const auto& root = paths_.forScope(scope);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return records;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory() || entry.path().filename() == kCacheDir || entry.path().filename() == kBinDir) {
            continue;
        }
        auto meta = entry.path() / kAppMetaFile;
        std::error_code metaEc;
        if (!std::filesystem::is_regular_file(meta, metaEc)) {
            continue;
        }
        auto fields = read_flat_yaml(meta);
        InstalledAppRecord record;
        record.name = fields.count("name") ? fields["name"] : entry.path().filename().string();
        record.version = fields.count("version") ? fields["version"] : "";
        std::string executable = fields.count("executable") ? fields["executable"] : "";
        record.payloadPath = entry.path();
        record.executablePath = executable.empty() ? std::filesystem::path() : entry.path() / executable;
        record.launcherPath = root / kBinDir / appSlug(record.name);
        record.scope = scope;
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(),
        [](const InstalledAppRecord& a, const InstalledAppRecord& b) { return a.name < b.name; });
    return records;
}

Result<InstalledAppRecord> AppStore::install(const AppInstallPayload& payload, ModuleScope scope) {
    std::error_code ec;
    if (payload.name.empty()) {
        return Error("app_install_failed", "Installed app is missing a name");
    }
    if (!std::filesystem::is_regular_file(payload.executablePath, ec)) {
        return Error("app_executable_not_found", "Built app executable not found: " + payload.executablePath.string());
    }

    const auto& scopeRoot = paths_.forScope(scope);
    const std::string slug = appSlug(payload.name);
    const auto targetDir = scopeRoot / slug;
    const auto cacheDir = scopeRoot / kCacheDir / slug;
    std::filesystem::remove_all(targetDir, ec);
    std::filesystem::create_directories(targetDir, ec);
    if (ec) {
        return Error("app_install_failed", "Unable to create install directory: " + targetDir.string());
    }

    const auto executableName = payload.executablePath.filename();
    const auto installedExecutable = targetDir / executableName;
    std::filesystem::copy_file(payload.executablePath, installedExecutable,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Error("app_install_failed", "Unable to copy app executable to " + installedExecutable.string());
    }
    std::filesystem::permissions(
        installedExecutable,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add,
        ec);

    {
        std::ofstream meta(targetDir / kAppMetaFile, std::ios::binary | std::ios::trunc);
        meta << "schema: wl2.app.v1\n";
        meta << "name: " << payload.name << "\n";
        meta << "version: " << payload.version << "\n";
        meta << "executable: " << executableName.string() << "\n";
        meta << "source: " << payload.source << "\n";
        if (!payload.ref.empty()) {
            meta << "ref: " << payload.ref << "\n";
        }
        if (!payload.path.empty()) {
            meta << "path: " << payload.path << "\n";
        }
    }

    std::filesystem::create_directories(cacheDir, ec);
    std::filesystem::copy_file(payload.executablePath, cacheDir / executableName,
        std::filesystem::copy_options::overwrite_existing, ec);

    const auto launcher = scopeRoot / kBinDir / slug;
    if (auto rc = write_launcher(launcher, installedExecutable); !rc) {
        return rc.error();
    }

    write_index(scopeRoot, scanScope(scope));

    InstalledAppRecord record;
    record.name = payload.name;
    record.version = payload.version;
    record.executablePath = installedExecutable;
    record.launcherPath = launcher;
    record.payloadPath = targetDir;
    record.scope = scope;
    return record;
}

Result<void> AppStore::uninstall(const std::string& name, std::optional<ModuleScope> scope, bool purgeCache) {
    const std::string slug = appSlug(name);
    std::vector<ModuleScope> present;
    for (ModuleScope candidate : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(paths_.forScope(candidate) / slug / kAppMetaFile, ec)) {
            present.push_back(candidate);
        }
    }
    if (present.empty()) {
        return Error("app_not_installed", "App is not installed: " + name);
    }

    ModuleScope target;
    if (scope) {
        if (std::find(present.begin(), present.end(), *scope) == present.end()) {
            return Error("app_not_installed",
                "App " + name + " is not installed in scope " + moduleScopeName(*scope));
        }
        target = *scope;
    } else if (present.size() > 1) {
        return Error("app_scope_ambiguous",
            "App " + name + " is installed in multiple scopes; pass --scope");
    } else {
        target = present.front();
    }

    const auto& scopeRoot = paths_.forScope(target);
    std::error_code ec;
    std::filesystem::remove_all(scopeRoot / slug, ec);
    std::filesystem::remove(scopeRoot / kBinDir / slug, ec);
    if (purgeCache) {
        std::filesystem::remove_all(scopeRoot / kCacheDir / slug, ec);
    }
    write_index(scopeRoot, scanScope(target));
    return {};
}

std::vector<InstalledAppRecord> AppStore::list() const {
    std::vector<InstalledAppRecord> all;
    std::set<std::string> seen;
    for (ModuleScope scope : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        for (auto& record : scanScope(scope)) {
            record.shadowed = !seen.insert(record.name).second;
            all.push_back(std::move(record));
        }
    }
    return all;
}

std::optional<InstalledAppRecord> AppStore::resolve(const std::string& name) const {
    for (ModuleScope scope : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        for (auto& record : scanScope(scope)) {
            if (record.name == name) {
                return record;
            }
        }
    }
    return std::nullopt;
}

} // namespace wl2
