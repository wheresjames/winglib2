#include "wl2/module_store.h"

#include "wl2/hash.h"
#include "wl2/module.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace wl2 {

namespace {

constexpr const char* kModuleMetaFile = "wl2.module.yml";
constexpr const char* kIndexFile = "index.yml";
constexpr const char* kCacheDir = ".cache";

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
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

size_t indent_of(std::string_view line) {
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
        ++indent;
    }
    return indent;
}

/// Parsed installed-module metadata: flat top-level scalars plus the optional
/// `dependencies:` block (`wl2.module.v2`). `wl2.module.v1` files have no
/// dependency block and yield an empty dependency list.
struct ModuleMetadata {
    std::map<std::string, std::string> fields;
    std::vector<ModuleDependencyRequirement> dependencies;
};

ModuleMetadata read_module_metadata(const std::filesystem::path& path) {
    ModuleMetadata meta;
    std::ifstream in(path);
    std::string line;
    std::string section;       // top-level key currently open, e.g. "dependencies"
    std::string depKind;       // "required" or "optional" inside dependencies
    while (std::getline(in, line)) {
        const auto indent = indent_of(line);
        auto text = trim(line);
        if (text.empty()) {
            continue;
        }

        if (text.rfind("- ", 0) == 0 && section == "dependencies"
            && (depKind == "required" || depKind == "optional")) {
            auto value = trim(std::string_view(text).substr(2));
            auto colon = value.find(':');
            ModuleDependencyRequirement dep;
            dep.kind = depKind == "optional" ? ModuleDependencyKind::Optional
                                             : ModuleDependencyKind::Required;
            if (colon != std::string::npos) {
                auto key = trim(std::string_view(value).substr(0, colon));
                auto fieldValue = trim(std::string_view(value).substr(colon + 1));
                if (key == "name") dep.name = fieldValue;
                else if (key == "version") dep.versionRange = fieldValue;
                else if (key == "stableId") dep.stableId = fieldValue;
            }
            meta.dependencies.push_back(std::move(dep));
            continue;
        }

        auto colon = text.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto key = trim(std::string_view(text).substr(0, colon));
        auto value = trim(std::string_view(text).substr(colon + 1));

        // Continuation of the most recent dependency list item, e.g. an indented
        // `version:`/`stableId:` line beneath a `- name:` entry.
        if (section == "dependencies" && indent >= 6 && !meta.dependencies.empty()) {
            auto& dep = meta.dependencies.back();
            if (key == "name") dep.name = value;
            else if (key == "version") dep.versionRange = value;
            else if (key == "stableId") dep.stableId = value;
            continue;
        }

        if (indent == 0) {
            section = key;
            depKind.clear();
            if (key != "dependencies" && !key.empty()) {
                meta.fields[key] = value;
            }
            continue;
        }
        if (section == "dependencies" && indent == 2 && (key == "required" || key == "optional")) {
            depKind = key;
            continue;
        }
    }
    return meta;
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

std::filesystem::path system_data_root() {
    std::string override = env_value("WL2_SYSTEM_MODULE_DIR");
    if (!override.empty()) {
        // The override points directly at the system modules directory.
        return std::filesystem::path(override).parent_path();
    }
    return std::filesystem::path("/usr/local/share/wl2");
}

} // namespace

std::string moduleScopeName(ModuleScope scope) {
    switch (scope) {
        case ModuleScope::Local: return "local";
        case ModuleScope::User: return "user";
        case ModuleScope::System: return "system";
    }
    return "local";
}

std::optional<ModuleScope> parseModuleScope(std::string_view value) {
    if (value == "local") return ModuleScope::Local;
    if (value == "user") return ModuleScope::User;
    if (value == "system") return ModuleScope::System;
    return std::nullopt;
}

const std::filesystem::path& ModuleScopePaths::forScope(ModuleScope scope) const {
    switch (scope) {
        case ModuleScope::Local: return local;
        case ModuleScope::User: return user;
        case ModuleScope::System: return system;
    }
    return local;
}

ModuleScopePaths resolveModuleScopePaths(const std::filesystem::path& projectRoot) {
    ModuleScopePaths paths;
    paths.local = projectRoot / ".wl2" / "modules";
    paths.user = user_data_root() / "modules";
    std::string override = env_value("WL2_SYSTEM_MODULE_DIR");
    paths.system = override.empty() ? (system_data_root() / "modules") : std::filesystem::path(override);
    return paths;
}

std::string moduleSlug(std::string_view name) {
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
    return slug;
}

ModuleStore::ModuleStore(ModuleScopePaths paths) : paths_(std::move(paths)) {}

std::vector<InstalledModuleRecord> ModuleStore::scanScope(ModuleScope scope) const {
    std::vector<InstalledModuleRecord> records;
    const auto& root = paths_.forScope(scope);
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return records;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory() || entry.path().filename() == kCacheDir) {
            continue;
        }
        auto meta = entry.path() / kModuleMetaFile;
        std::error_code metaEc;
        if (!std::filesystem::is_regular_file(meta, metaEc)) {
            continue;
        }
        auto parsed = read_module_metadata(meta);
        auto& fields = parsed.fields;
        InstalledModuleRecord record;
        record.name = fields.count("name") ? fields["name"] : entry.path().filename().string();
        record.version = fields.count("version") ? fields["version"] : "";
        record.build = fields.count("build") ? fields["build"] : "";
        record.stableId = fields.count("stableId") ? fields["stableId"] : "";
        record.abiVersion = fields.count("abi") ? static_cast<std::uint32_t>(std::strtoul(fields["abi"].c_str(), nullptr, 10)) : 0;
        std::string library = fields.count("library") ? fields["library"] : "";
        record.libraryPath = library.empty() ? std::filesystem::path() : entry.path() / library;
        record.scope = scope;
        record.checksum = fields.count("sha256") ? fields["sha256"] : "";
        record.dependencies = std::move(parsed.dependencies);
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(),
        [](const InstalledModuleRecord& a, const InstalledModuleRecord& b) { return a.name < b.name; });
    return records;
}

namespace {

// Serialize a module's dependency list as an indented `dependencies:` block.
// `indent` is the leading-space prefix for the `dependencies:` key itself.
void write_dependencies_block(std::ostream& out, const std::string& indent,
    const std::vector<ModuleDependencyRequirement>& dependencies) {
    if (dependencies.empty()) {
        return;
    }
    const std::string i2 = indent + "  ";
    const std::string i4 = indent + "    ";
    const std::string i6 = indent + "      ";
    out << indent << "dependencies:\n";
    bool wroteRequired = false;
    bool wroteOptional = false;
    for (const auto& dep : dependencies) {
        const bool required = dep.kind == ModuleDependencyKind::Required;
        if (required && !wroteRequired) {
            out << i2 << "required:\n";
            wroteRequired = true;
        }
        if (!required && !wroteOptional) {
            out << i2 << "optional:\n";
            wroteOptional = true;
        }
        out << i4 << "- name: " << dep.name << "\n";
        if (!dep.versionRange.empty()) {
            out << i6 << "version: \"" << dep.versionRange << "\"\n";
        }
        if (!dep.stableId.empty()) {
            out << i6 << "stableId: " << dep.stableId << "\n";
        }
    }
}

void write_index(const std::filesystem::path& scopeRoot, const std::vector<InstalledModuleRecord>& records) {
    std::error_code ec;
    std::filesystem::create_directories(scopeRoot, ec);
    std::ofstream out(scopeRoot / kIndexFile, std::ios::binary | std::ios::trunc);
    out << "schema: wl2.module-index.v2\n";
    out << "modules:\n";
    for (const auto& record : records) {
        out << "  - name: " << record.name << "\n";
        out << "    slug: " << moduleSlug(record.name) << "\n";
        out << "    version: " << record.version << "\n";
        if (!record.build.empty()) {
            out << "    build: " << record.build << "\n";
        }
        out << "    abi: " << record.abiVersion << "\n";
        out << "    library: " << record.libraryPath.filename().string() << "\n";
        if (!record.checksum.empty()) {
            out << "    sha256: " << record.checksum << "\n";
        }
        // Dependency metadata is indexed so a provider graph can be built without
        // opening each module library.
        write_dependencies_block(out, "    ", record.dependencies);
    }
}

} // namespace

Result<InstalledModuleRecord> ModuleStore::install(const std::filesystem::path& librarySource, ModuleScope scope) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(librarySource, ec)) {
        return Error("module_source_not_found", "Module library not found: " + librarySource.string());
    }
    auto info = ModuleLoader::inspectDynamicModule(librarySource);
    if (!info) {
        return info.error();
    }

    const auto& scopeRoot = paths_.forScope(scope);
    const std::string slug = moduleSlug(info.value().name);
    const auto targetDir = scopeRoot / slug;
    const auto cacheDir = scopeRoot / kCacheDir / slug;

    std::filesystem::remove_all(targetDir, ec);
    std::filesystem::create_directories(targetDir, ec);
    if (ec) {
        return Error("module_install_failed", "Unable to create install directory: " + targetDir.string());
    }
    const auto libraryName = librarySource.filename();
    const auto installedLibrary = targetDir / libraryName;
    std::filesystem::copy_file(librarySource, installedLibrary,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        return Error("module_install_failed", "Unable to copy module library to " + installedLibrary.string());
    }

    // Content checksum of the installed library, recorded so a later load can
    // detect a stale or tampered library without re-inspecting its ABI.
    auto checksum = sha256File(installedLibrary);
    if (!checksum) {
        return checksum.error();
    }

    {
        std::ofstream meta(targetDir / kModuleMetaFile, std::ios::binary | std::ios::trunc);
        meta << "schema: wl2.module.v2\n";
        meta << "name: " << info.value().name << "\n";
        meta << "version: " << info.value().version << "\n";
        if (!info.value().build.empty()) {
            meta << "build: " << info.value().build << "\n";
        }
        meta << "abi: " << info.value().abiVersion << "\n";
        meta << "stableId: " << info.value().stableId << "\n";
        meta << "library: " << libraryName.string() << "\n";
        meta << "sha256: " << checksum.value() << "\n";
        // Dependency metadata read from the validated dynamic ABI. ABI v2 modules
        // report no dependencies and the block is omitted.
        write_dependencies_block(meta, "", info.value().dependencies);
    }

    // Retain the build artifact in the scope cache; uninstall keeps it unless
    // explicitly purged.
    std::filesystem::create_directories(cacheDir, ec);
    std::filesystem::copy_file(librarySource, cacheDir / libraryName,
        std::filesystem::copy_options::overwrite_existing, ec);

    write_index(scopeRoot, scanScope(scope));

    InstalledModuleRecord record;
    record.name = info.value().name;
    record.version = info.value().version;
    record.build = info.value().build;
    record.abiVersion = info.value().abiVersion;
    record.stableId = info.value().stableId;
    record.libraryPath = installedLibrary;
    record.scope = scope;
    record.checksum = checksum.value();
    record.dependencies = info.value().dependencies;
    return record;
}

Result<void> ModuleStore::uninstall(
    const std::string& name,
    std::optional<ModuleScope> scope,
    bool force,
    bool purgeCache,
    const std::set<std::string>& referencedNames) {
    const std::string slug = moduleSlug(name);

    // Find scopes that actually contain the module.
    std::vector<ModuleScope> present;
    for (ModuleScope candidate : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(paths_.forScope(candidate) / slug / kModuleMetaFile, ec)) {
            present.push_back(candidate);
        }
    }
    if (present.empty()) {
        return Error("module_not_installed", "Module is not installed: " + name);
    }

    ModuleScope target;
    if (scope) {
        if (std::find(present.begin(), present.end(), *scope) == present.end()) {
            return Error("module_not_installed",
                "Module " + name + " is not installed in scope " + moduleScopeName(*scope));
        }
        target = *scope;
    } else if (present.size() > 1) {
        return Error("module_scope_ambiguous",
            "Module " + name + " is installed in multiple scopes; pass --scope");
    } else {
        target = present.front();
    }

    if (!force && referencedNames.count(name)) {
        return Error("module_referenced",
            "Module " + name + " is referenced by the manifest/lockfile; use --force to remove it");
    }

    const auto& scopeRoot = paths_.forScope(target);
    std::error_code ec;
    std::filesystem::remove_all(scopeRoot / slug, ec);
    if (ec) {
        return Error("module_uninstall_failed", "Unable to remove " + (scopeRoot / slug).string());
    }
    if (purgeCache) {
        std::filesystem::remove_all(scopeRoot / kCacheDir / slug, ec);
    }
    write_index(scopeRoot, scanScope(target));
    return {};
}

std::vector<InstalledModuleRecord> ModuleStore::list() const {
    std::vector<InstalledModuleRecord> all;
    std::set<std::string> seen;
    for (ModuleScope scope : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        for (auto& record : scanScope(scope)) {
            record.shadowed = !seen.insert(record.name).second;
            all.push_back(std::move(record));
        }
    }
    return all;
}

std::optional<InstalledModuleRecord> ModuleStore::resolve(const std::string& name) const {
    for (ModuleScope scope : {ModuleScope::Local, ModuleScope::User, ModuleScope::System}) {
        for (auto& record : scanScope(scope)) {
            if (record.name == name) {
                return record;
            }
        }
    }
    return std::nullopt;
}

Result<void> ModuleStore::verifyInstalled(const InstalledModuleRecord& record) const {
    std::error_code ec;
    if (record.libraryPath.empty() || !std::filesystem::is_regular_file(record.libraryPath, ec)) {
        return Error("module_library_missing",
            "Installed module library is missing: " + record.name);
    }
    // Legacy installs without a recorded checksum cannot be verified; treat as
    // valid so older scopes keep loading.
    if (record.checksum.empty()) {
        return {};
    }
    auto actual = sha256File(record.libraryPath);
    if (!actual) {
        return actual.error();
    }
    if (actual.value() != record.checksum) {
        return Error("module_checksum_mismatch",
            "Installed module " + record.name + " does not match its recorded checksum "
            "(stale or tampered library): " + record.libraryPath.string());
    }
    return {};
}

} // namespace wl2
