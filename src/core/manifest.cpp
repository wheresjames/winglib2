#include "wl2/manifest.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

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

size_t indent_of(std::string_view line) {
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') {
        ++indent;
    }
    return indent;
}

std::string strip_comment(std::string_view line) {
    bool singleQuoted = false;
    bool doubleQuoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '\'' && !doubleQuoted) {
            singleQuoted = !singleQuoted;
        } else if (ch == '"' && !singleQuoted) {
            doubleQuoted = !doubleQuoted;
        } else if (ch == '#' && !singleQuoted && !doubleQuoted) {
            return std::string(line.substr(0, i));
        }
    }
    return std::string(line);
}

bool is_known_top_level(const std::string& key) {
    static const std::set<std::string> keys = {
        "schema",
        "prefix",
        "root",
        "entry",
        "resources",
        "exclude",
        "modules",
        "tests",
        "dependencies",
        "capabilities",
    };
    return keys.contains(key);
}

Result<bool> parse_bool_value(const std::string& value, const std::string& key, int lineNumber) {
    if (value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "no" || value == "off") {
        return false;
    }
    return Error("manifest_invalid_capabilities",
        "Expected boolean for capabilities." + key + " at line " + std::to_string(lineNumber));
}

Result<void> assign_dependency_field(
    ModuleDependency& dep,
    const std::string& key,
    const std::string& value,
    int lineNumber) {
    if (key == "name") {
        dep.name = value;
    } else if (key == "git") {
        dep.git = value;
    } else if (key == "tag") {
        dep.tag = value;
    } else if (key == "commit") {
        dep.commit = value;
    } else if (key == "path") {
        dep.path = value;
    } else if (key == "provides") {
        dep.provides = value;
    } else if (key == "branch") {
        return Error("manifest_floating_branch",
            "Floating branch dependencies are not allowed; pin a tag or commit at line "
                + std::to_string(lineNumber));
    } else {
        return Error("manifest_unknown_dependency_key",
            "Unknown dependency key at line " + std::to_string(lineNumber) + ": " + key);
    }
    return {};
}

Result<void> append_list_value(std::vector<std::string>& values, const std::string& value, int lineNumber) {
    if (value.empty()) {
        return Error("manifest_invalid_list", "Empty list value at line " + std::to_string(lineNumber));
    }
    values.push_back(value);
    return {};
}

bool is_relative_clean_path(const std::string& path) {
    std::filesystem::path fsPath(path);
    if (path.empty() || fsPath.is_absolute()) {
        return false;
    }
    for (const auto& part : fsPath) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

Result<void> validate_paths(const std::vector<std::string>& paths, const char* label) {
    for (const auto& path : paths) {
        if (!is_relative_clean_path(path)) {
            return Error("manifest_invalid_path", std::string(label) + " path must be relative and stay within root: " + path);
        }
    }
    return {};
}

// Reject manifests that list a resource twice (a duplicate logical path) or
// that point at files/directories which do not exist under the resolved root.
// Path shape (relative, no traversal) is already enforced by validate_paths,
// so these checks assume clean relative paths.
Result<void> validate_resource_entries(const ResourceManifest& manifest) {
    const auto root = manifest.resolvedRoot();
    std::set<std::string> seen;

    const std::vector<std::pair<const std::vector<std::string>*, const char*>> fileLists = {
        {&manifest.store.files, "store file"},
        {&manifest.compress.files, "compress file"},
        {&manifest.autoPolicy.files, "auto file"},
    };
    for (const auto& [files, label] : fileLists) {
        for (const auto& file : *files) {
            if (!seen.insert(file).second) {
                return Error("manifest_duplicate_path", "Duplicate resource path in manifest: " + file);
            }
            std::error_code ec;
            if (!std::filesystem::is_regular_file(root / file, ec)) {
                return Error("manifest_missing_file", std::string(label) + " does not exist: " + (root / file).string());
            }
        }
    }

    const std::vector<std::pair<const std::vector<std::string>*, const char*>> dirLists = {
        {&manifest.store.directories, "store directory"},
        {&manifest.compress.directories, "compress directory"},
        {&manifest.autoPolicy.directories, "auto directory"},
    };
    for (const auto& [dirs, label] : dirLists) {
        for (const auto& dir : *dirs) {
            if (!seen.insert(dir).second) {
                return Error("manifest_duplicate_path", "Duplicate resource path in manifest: " + dir);
            }
            std::error_code ec;
            if (!std::filesystem::is_directory(root / dir, ec)) {
                return Error("manifest_missing_directory", std::string(label) + " does not exist: " + (root / dir).string());
            }
        }
    }
    return {};
}

// A module may be listed at most once, and never in both require and optional.
// Required modules are enforced at runtime before the entry script executes;
// the manifest only guarantees the lists are well-formed here.
Result<void> validate_module_requirements(const ResourceManifest& manifest) {
    std::set<std::string> seen;
    for (const auto& name : manifest.requiredModules) {
        if (!seen.insert(name).second) {
            return Error("manifest_duplicate_module", "Module listed more than once: " + name);
        }
    }
    for (const auto& name : manifest.optionalModules) {
        if (!seen.insert(name).second) {
            return Error("manifest_duplicate_module",
                "Module listed in both modules.require and modules.optional: " + name);
        }
    }
    return {};
}

// Each module dependency must name a Git source and pin an immutable ref (a tag
// or an explicit commit). Names must be unique.
Result<void> validate_module_dependencies(const ResourceManifest& manifest) {
    std::set<std::string> seen;
    for (const auto& dep : manifest.moduleDependencies) {
        if (dep.name.empty()) {
            return Error("manifest_invalid_dependency", "Module dependency is missing a name");
        }
        // The name becomes a directory under the dependency root, so it must be a
        // single path segment that cannot traverse out of it.
        if (dep.name == "." || dep.name == ".."
            || dep.name.find('/') != std::string::npos
            || dep.name.find('\\') != std::string::npos) {
            return Error("manifest_invalid_dependency",
                "Module dependency name must be a single path segment: " + dep.name);
        }
        if (!seen.insert(dep.name).second) {
            return Error("manifest_duplicate_dependency", "Module dependency listed more than once: " + dep.name);
        }
        if (dep.git.empty()) {
            return Error("manifest_invalid_dependency", "Module dependency is missing a git source: " + dep.name);
        }
        if (dep.tag.empty() && dep.commit.empty()) {
            return Error("manifest_invalid_dependency",
                "Module dependency must pin a tag or commit: " + dep.name);
        }
    }
    return {};
}

} // namespace

std::filesystem::path ResourceManifest::resolvedRoot() const {
    return root.is_absolute() ? root : baseDir / root;
}

std::string ResourceManifest::entrySpecifier() const {
    auto normalizedPrefix = prefix;
    while (normalizedPrefix.size() > 4 && normalizedPrefix.back() == '/') {
        normalizedPrefix.pop_back();
    }
    return normalizedPrefix + "/" + entry;
}

Result<ResourceManifest> loadResourceManifest(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return Error("manifest_not_found", "Unable to open manifest: " + path.string());
    }

    ResourceManifest manifest;
    manifest.path = path;
    manifest.baseDir = path.parent_path().empty() ? std::filesystem::current_path() : std::filesystem::absolute(path.parent_path());

    std::string section;
    std::string resourcePolicy;
    std::string modulesSection;
    std::string depsSection;
    std::string listKey;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        auto withoutComment = strip_comment(line);
        auto text = trim(withoutComment);
        if (text.empty()) {
            continue;
        }
        const auto indent = indent_of(withoutComment);

        if (text.rfind("- ", 0) == 0) {
            auto value = trim(std::string_view(text).substr(2));
            if (section == "exclude" && listKey.empty()) {
                auto appended = append_list_value(manifest.excludePatterns, value, lineNumber);
                if (!appended) {
                    return appended.error();
                }
            } else if (section == "resources") {
                ResourceManifestPolicy* policy = nullptr;
                if (resourcePolicy == "store") {
                    policy = &manifest.store;
                } else if (resourcePolicy == "compress") {
                    policy = &manifest.compress;
                } else if (resourcePolicy == "auto") {
                    policy = &manifest.autoPolicy;
                }
                if (!policy) {
                    return Error("manifest_invalid_resources", "Resource list outside a known policy at line " + std::to_string(lineNumber));
                }
                auto& target = listKey == "directories" ? policy->directories : policy->files;
                auto appended = append_list_value(target, value, lineNumber);
                if (!appended) {
                    return appended.error();
                }
            } else if (section == "modules") {
                auto& target = modulesSection == "optional" ? manifest.optionalModules : manifest.requiredModules;
                auto appended = append_list_value(target, value, lineNumber);
                if (!appended) {
                    return appended.error();
                }
            } else if (section == "dependencies" && depsSection == "modules") {
                // Each list item starts a new module dependency with its first
                // "key: value" field inline (for example "- name: wl2_echo").
                auto fieldColon = value.find(':');
                if (fieldColon == std::string::npos) {
                    return Error("manifest_invalid_dependency",
                        "Expected key: value in module dependency at line " + std::to_string(lineNumber));
                }
                manifest.moduleDependencies.emplace_back();
                auto key = trim(std::string_view(value).substr(0, fieldColon));
                auto fieldValue = trim(std::string_view(value).substr(fieldColon + 1));
                auto assigned = assign_dependency_field(manifest.moduleDependencies.back(), key, fieldValue, lineNumber);
                if (!assigned) {
                    return assigned.error();
                }
            } else if (section == "tests" && listKey == "roots") {
                auto appended = append_list_value(manifest.testRoots, value, lineNumber);
                if (!appended) {
                    return appended.error();
                }
            } else {
                return Error("manifest_unexpected_list", "Unexpected list item at line " + std::to_string(lineNumber));
            }
            continue;
        }

        auto colon = text.find(':');
        if (colon == std::string::npos) {
            return Error("manifest_parse_error", "Expected key/value at line " + std::to_string(lineNumber));
        }
        auto key = trim(std::string_view(text).substr(0, colon));
        auto value = trim(std::string_view(text).substr(colon + 1));

        if (indent == 0) {
            if (!is_known_top_level(key)) {
                return Error("manifest_unknown_key", "Unknown manifest key at line " + std::to_string(lineNumber) + ": " + key);
            }
            section = key;
            resourcePolicy.clear();
            modulesSection.clear();
            depsSection.clear();
            listKey.clear();
            if (key == "schema") {
                manifest.schema = value;
            } else if (key == "prefix") {
                manifest.prefix = value;
            } else if (key == "root") {
                manifest.root = value;
            } else if (key == "entry") {
                manifest.entry = value;
            } else if (key == "exclude" && !value.empty()) {
                return Error("manifest_invalid_exclude", "exclude must be a list at line " + std::to_string(lineNumber));
            } else if (key == "capabilities" && !value.empty()) {
                return Error("manifest_invalid_capabilities",
                    "capabilities must be a mapping at line " + std::to_string(lineNumber));
            }
            continue;
        }

        if (section == "resources" && indent == 2) {
            if (key != "store" && key != "compress" && key != "auto") {
                return Error("manifest_invalid_resources", "Unknown resources policy at line " + std::to_string(lineNumber) + ": " + key);
            }
            resourcePolicy = key;
            listKey.clear();
            continue;
        }
        if (section == "resources" && indent == 4) {
            if (key != "files" && key != "directories") {
                return Error("manifest_invalid_resources", "Expected files or directories at line " + std::to_string(lineNumber));
            }
            listKey = key;
            continue;
        }
        if (section == "modules" && indent == 2) {
            if (key != "require" && key != "optional") {
                return Error("manifest_invalid_modules", "Expected require or optional at line " + std::to_string(lineNumber));
            }
            modulesSection = key == "optional" ? "optional" : "require";
            continue;
        }

        if (section == "dependencies" && indent == 2) {
            if (key != "modules") {
                return Error("manifest_invalid_dependencies",
                    "Unknown dependencies section at line " + std::to_string(lineNumber) + ": " + key);
            }
            depsSection = "modules";
            continue;
        }
        if (section == "dependencies" && depsSection == "modules" && indent >= 6) {
            if (manifest.moduleDependencies.empty()) {
                return Error("manifest_invalid_dependency",
                    "Dependency field outside a list item at line " + std::to_string(lineNumber));
            }
            auto assigned = assign_dependency_field(manifest.moduleDependencies.back(), key, value, lineNumber);
            if (!assigned) {
                return assigned.error();
            }
            continue;
        }

        if (section == "tests" && indent == 2) {
            if (key == "roots") {
                if (!value.empty()) {
                    return Error("manifest_invalid_tests", "tests.roots must be a list at line " + std::to_string(lineNumber));
                }
                listKey = "roots";
                continue;
            }
            if (key == "pattern") {
                if (value.empty()) {
                    return Error("manifest_invalid_tests", "tests.pattern must not be empty at line " + std::to_string(lineNumber));
                }
                manifest.testPattern = value;
                continue;
            }
            return Error("manifest_invalid_tests", "Expected roots or pattern at line " + std::to_string(lineNumber));
        }
        if (section == "tests") {
            continue;
        }

        if (section == "capabilities" && indent == 2) {
            if (key != "ui") {
                return Error("manifest_invalid_capabilities",
                    "Expected ui under capabilities at line " + std::to_string(lineNumber));
            }
            auto parsed = parse_bool_value(value, key, lineNumber);
            if (!parsed) {
                return parsed.error();
            }
            manifest.allowUi = parsed.value();
            continue;
        }

        return Error("manifest_parse_error", "Unexpected key at line " + std::to_string(lineNumber) + ": " + key);
    }

    if (manifest.schema != "wl2.resources.v1" && manifest.schema != "wl2.project.v1") {
        return Error("manifest_invalid_schema", "Unsupported manifest schema: " + manifest.schema);
    }
    if (!manifest.prefix.starts_with("wl2:/")) {
        return Error("manifest_invalid_prefix", "Manifest prefix must start with wl2:/");
    }
    if (manifest.root.empty()) {
        return Error("manifest_missing_root", "Manifest root is required");
    }
    if (manifest.entry.empty()) {
        return Error("manifest_missing_entry", "Manifest entry is required");
    }
    if (!is_relative_clean_path(manifest.entry)) {
        return Error("manifest_invalid_entry", "Manifest entry must be relative and stay within root: " + manifest.entry);
    }
    if (auto ok = validate_paths(manifest.store.files, "store file"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.compress.files, "compress file"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.autoPolicy.files, "auto file"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.store.directories, "store directory"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.compress.directories, "compress directory"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.autoPolicy.directories, "auto directory"); !ok) {
        return ok.error();
    }
    if (auto ok = validate_paths(manifest.testRoots, "test root"); !ok) {
        return ok.error();
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(manifest.resolvedRoot(), ec)) {
        return Error("manifest_invalid_root", "Manifest root is not a directory: " + manifest.resolvedRoot().string());
    }
    if (!std::filesystem::is_regular_file(manifest.resolvedRoot() / manifest.entry, ec)) {
        return Error("manifest_invalid_entry", "Manifest entry does not exist: " + (manifest.resolvedRoot() / manifest.entry).string());
    }
    if (auto ok = validate_resource_entries(manifest); !ok) {
        return ok.error();
    }
    if (auto ok = validate_module_requirements(manifest); !ok) {
        return ok.error();
    }
    if (auto ok = validate_module_dependencies(manifest); !ok) {
        return ok.error();
    }

    return manifest;
}

} // namespace wl2
