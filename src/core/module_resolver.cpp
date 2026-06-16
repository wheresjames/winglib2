#include "wl2/module_resolver.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

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

bool known_source_key(const std::string& key) {
    static const std::set<std::string> keys = {
        "schema",
        "provides",
        "version",
        "stableId",
        "path",
        "dependencies",
        "sourceDependencies",
    };
    return keys.contains(key);
}

Result<void> assign_requirement_field(
    ModuleDependencyRequirement& dep,
    const std::string& key,
    const std::string& value,
    int lineNumber) {
    if (key == "name") {
        dep.name = value;
    } else if (key == "version") {
        dep.versionRange = value;
    } else if (key == "stableId") {
        dep.stableId = value;
    } else {
        return Error("module_source_unknown_dependency_key",
            "Unknown module dependency key at line " + std::to_string(lineNumber) + ": " + key);
    }
    return {};
}

Result<void> assign_source_dependency_field(
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
        return Error("module_source_floating_branch",
            "Floating branch dependencies are not allowed; pin a tag or commit at line "
                + std::to_string(lineNumber));
    } else {
        return Error("module_source_unknown_source_dependency_key",
            "Unknown source dependency key at line " + std::to_string(lineNumber) + ": " + key);
    }
    return {};
}

bool is_safe_name(const std::string& name) {
    return !name.empty();
}

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

Result<Version> parse_version(const std::string& text) {
    Version version;
    char dot1 = 0;
    char dot2 = 0;
    std::istringstream in(text);
    if (!(in >> version.major >> dot1 >> version.minor >> dot2 >> version.patch)
        || dot1 != '.' || dot2 != '.' || version.major < 0 || version.minor < 0 || version.patch < 0) {
        return Error("module_dependency_invalid_version", "Invalid module version: " + text);
    }
    std::string rest;
    if (in >> rest) {
        return Error("module_dependency_invalid_version", "Invalid module version: " + text);
    }
    return version;
}

int compare_versions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

int source_rank(ModuleProvider::Source source) {
    switch (source) {
        case ModuleProvider::Source::Explicit: return 0;
        case ModuleProvider::Source::Project: return 1;
        case ModuleProvider::Source::Local: return 2;
        case ModuleProvider::Source::User: return 3;
        case ModuleProvider::Source::System: return 4;
        case ModuleProvider::Source::Builtin: return 5;
    }
    return 5;
}

struct Resolver {
    const ModuleResolutionRequest& request;
    std::map<std::string, std::vector<const ModuleProvider*>> providers;
    std::map<std::string, ResolvedModule> selected;
    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::set<std::string> shadowDiagnostics;
    std::vector<std::string> order;
    ModuleResolutionPlan plan;

    explicit Resolver(const ModuleResolutionRequest& req) : request(req) {
        for (const auto& provider : request.providers) {
            if (!provider.info.name.empty()) {
                providers[provider.info.name].push_back(&provider);
            }
        }
        for (auto& [name, entries] : providers) {
            std::sort(entries.begin(), entries.end(), [](const ModuleProvider* a, const ModuleProvider* b) {
                return source_rank(a->source) < source_rank(b->source);
            });
        }
    }

    void diagnose(
        std::string code,
        std::string message,
        const std::vector<std::string>& chain) {
        plan.diagnostics.push_back(ModuleResolutionDiagnostic{
            .code = std::move(code),
            .message = std::move(message),
            .chain = chain,
        });
    }

    const ModuleProvider* choose(
        const ModuleDependencyRequirement& requirement,
        const std::vector<std::string>& chain,
        bool required) {
        auto found = providers.find(requirement.name);
        if (found == providers.end()) {
            if (required) {
                diagnose("module_dependency_missing", "Required module is missing: " + requirement.name, chain);
            } else {
                diagnose("module_optional_missing", "Optional module is not available: " + requirement.name, chain);
            }
            return nullptr;
        }

        bool hadIdentityMismatch = false;
        bool hadVersionMismatch = false;
        for (const auto* provider : found->second) {
            if (!requirement.stableId.empty() && provider->info.stableId != requirement.stableId) {
                hadIdentityMismatch = true;
                continue;
            }
            auto versionOk = moduleVersionSatisfies(provider->info.version, requirement.versionRange);
            if (!versionOk || !versionOk.value()) {
                hadVersionMismatch = true;
                continue;
            }
            for (const auto* shadowed : found->second) {
                if (shadowed == provider) {
                    continue;
                }
                const std::string key = requirement.name + "\n"
                    + moduleProviderSourceName(provider->source) + "\n"
                    + moduleProviderSourceName(shadowed->source) + "\n"
                    + shadowed->path.string();
                if (source_rank(provider->source) < source_rank(shadowed->source)
                    && shadowDiagnostics.insert(key).second) {
                    diagnose("module_provider_shadowed",
                        "Provider for " + requirement.name + " from "
                            + moduleProviderSourceName(shadowed->source)
                            + " was shadowed by "
                            + moduleProviderSourceName(provider->source),
                        chain);
                }
            }
            return provider;
        }

        if (required) {
            if (hadIdentityMismatch) {
                diagnose("module_dependency_identity_mismatch",
                    "No provider for " + requirement.name + " matched the requested stableId", chain);
            } else if (hadVersionMismatch) {
                diagnose("module_dependency_version_mismatch",
                    "No provider for " + requirement.name + " satisfied version range " + requirement.versionRange, chain);
            } else {
                diagnose("module_dependency_missing", "Required module is missing: " + requirement.name, chain);
            }
        } else {
            diagnose("module_optional_unavailable", "Optional module is not compatible: " + requirement.name, chain);
        }
        return nullptr;
    }

    bool visit(const ModuleDependencyRequirement& requirement, bool inheritedOptional, std::vector<std::string> chain) {
        const bool required = requirement.kind == ModuleDependencyKind::Required && !inheritedOptional;
        chain.push_back(requirement.name);

        const ModuleProvider* provider = choose(requirement, chain, required);
        if (!provider) {
            return !required;
        }

        auto selectedIt = selected.find(provider->info.name);
        if (selectedIt != selected.end()) {
            if (!requirement.stableId.empty() && selectedIt->second.provider.info.stableId != requirement.stableId) {
                diagnose("module_dependency_conflict", "Selected provider does not satisfy stableId for " + provider->info.name, chain);
                return !required;
            }
            auto versionOk = moduleVersionSatisfies(selectedIt->second.provider.info.version, requirement.versionRange);
            if (!versionOk || !versionOk.value()) {
                diagnose("module_dependency_conflict", "Selected provider does not satisfy version range for " + provider->info.name, chain);
                return !required;
            }
        } else {
            selected.emplace(provider->info.name, ResolvedModule{
                .provider = *provider,
                .optional = inheritedOptional || requirement.kind == ModuleDependencyKind::Optional,
            });
        }

        if (visited.contains(provider->info.name)) {
            return true;
        }
        if (visiting.contains(provider->info.name)) {
            diagnose("module_dependency_cycle", "Module dependency cycle detected at " + provider->info.name, chain);
            return false;
        }

        visiting.insert(provider->info.name);
        for (const auto& dep : provider->info.dependencies) {
            if (!visit(dep, inheritedOptional || requirement.kind == ModuleDependencyKind::Optional, chain)
                && dep.kind == ModuleDependencyKind::Required && !inheritedOptional) {
                return false;
            }
        }
        visiting.erase(provider->info.name);
        visited.insert(provider->info.name);
        order.push_back(provider->info.name);
        return true;
    }

    Result<ModuleResolutionPlan> run() {
        for (const auto& root : request.roots) {
            ModuleDependencyRequirement requirement;
            requirement.name = root.name;
            requirement.kind = root.kind;
            const bool ok = visit(requirement, root.kind == ModuleDependencyKind::Optional, {});
            if (!ok && root.kind == ModuleDependencyKind::Required) {
                return Error(plan.diagnostics.empty() ? "module_dependency_missing" : plan.diagnostics.back().code,
                    plan.diagnostics.empty() ? "Module dependency resolution failed" : plan.diagnostics.back().message);
            }
        }

        for (const auto& name : order) {
            auto it = selected.find(name);
            if (it != selected.end()) {
                plan.loadOrder.push_back(it->second);
            }
        }
        return plan;
    }
};

} // namespace

std::string moduleProviderSourceName(ModuleProvider::Source source) {
    switch (source) {
        case ModuleProvider::Source::Explicit: return "explicit";
        case ModuleProvider::Source::Project: return "project";
        case ModuleProvider::Source::Local: return "local";
        case ModuleProvider::Source::User: return "user";
        case ModuleProvider::Source::System: return "system";
        case ModuleProvider::Source::Builtin: return "builtin";
    }
    return "builtin";
}

Result<bool> moduleVersionSatisfies(const std::string& versionText, const std::string& range) {
    if (range.empty()) {
        return true;
    }
    auto version = parse_version(versionText);
    if (!version) {
        return version.error();
    }

    if (range.front() == '=') {
        auto required = parse_version(range.substr(1));
        if (!required) {
            return required.error();
        }
        return compare_versions(version.value(), required.value()) == 0;
    }

    std::istringstream parts(range);
    std::string lowerPart;
    std::string upperPart;
    parts >> lowerPart >> upperPart;
    std::string extra;
    if (lowerPart.rfind(">=", 0) != 0 || upperPart.rfind("<", 0) != 0 || (parts >> extra)) {
        return Error("module_dependency_invalid_version_range", "Invalid module version range: " + range);
    }
    auto lower = parse_version(lowerPart.substr(2));
    if (!lower) {
        return lower.error();
    }
    auto upper = parse_version(upperPart.substr(1));
    if (!upper) {
        return upper.error();
    }
    return compare_versions(version.value(), lower.value()) >= 0
        && compare_versions(version.value(), upper.value()) < 0;
}

Result<ModuleSourceMetadata> loadModuleSourceMetadata(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        return Error("module_source_not_found", "Unable to open module source metadata: " + path.string());
    }

    ModuleSourceMetadata metadata;
    metadata.path = path;
    metadata.baseDir = path.parent_path().empty() ? std::filesystem::current_path() : std::filesystem::absolute(path.parent_path());

    std::string section;
    std::string dependencyKind;
    std::string sourceDependencySection;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        auto withoutComment = strip_comment(line);
        auto text = trim(withoutComment);
        if (text.empty()) {
            continue;
        }
        auto indent = indent_of(withoutComment);

        if (text.rfind("- ", 0) == 0) {
            auto value = trim(std::string_view(text).substr(2));
            auto colon = value.find(':');
            if (colon == std::string::npos) {
                return Error("module_source_parse_error", "Expected key: value at line " + std::to_string(lineNumber));
            }
            auto key = trim(std::string_view(value).substr(0, colon));
            auto fieldValue = trim(std::string_view(value).substr(colon + 1));
            if (section == "dependencies" && (dependencyKind == "required" || dependencyKind == "optional")) {
                ModuleDependencyRequirement dep;
                dep.kind = dependencyKind == "optional" ? ModuleDependencyKind::Optional : ModuleDependencyKind::Required;
                if (auto ok = assign_requirement_field(dep, key, fieldValue, lineNumber); !ok) {
                    return ok.error();
                }
                metadata.dependencies.push_back(std::move(dep));
            } else if (section == "sourceDependencies" && sourceDependencySection == "modules") {
                metadata.sourceDependencies.emplace_back();
                if (auto ok = assign_source_dependency_field(metadata.sourceDependencies.back(), key, fieldValue, lineNumber); !ok) {
                    return ok.error();
                }
            } else {
                return Error("module_source_unexpected_list", "Unexpected list item at line " + std::to_string(lineNumber));
            }
            continue;
        }

        auto colon = text.find(':');
        if (colon == std::string::npos) {
            return Error("module_source_parse_error", "Expected key/value at line " + std::to_string(lineNumber));
        }
        auto key = trim(std::string_view(text).substr(0, colon));
        auto value = trim(std::string_view(text).substr(colon + 1));

        if (indent == 0) {
            if (!known_source_key(key)) {
                return Error("module_source_unknown_key", "Unknown module source key at line " + std::to_string(lineNumber) + ": " + key);
            }
            section = key;
            dependencyKind.clear();
            sourceDependencySection.clear();
            if (key == "schema") metadata.schema = value;
            else if (key == "provides") metadata.provides = value;
            else if (key == "version") metadata.version = value;
            else if (key == "stableId") metadata.stableId = value;
            else if (key == "path") metadata.modulePath = value;
            continue;
        }

        if (section == "dependencies" && indent == 2) {
            if (key != "required" && key != "optional") {
                return Error("module_source_invalid_dependencies", "Expected required or optional at line " + std::to_string(lineNumber));
            }
            dependencyKind = key;
            continue;
        }
        if (section == "dependencies" && indent >= 6) {
            if (metadata.dependencies.empty()) {
                return Error("module_source_invalid_dependencies", "Dependency field outside list item at line " + std::to_string(lineNumber));
            }
            if (auto ok = assign_requirement_field(metadata.dependencies.back(), key, value, lineNumber); !ok) {
                return ok.error();
            }
            continue;
        }
        if (section == "sourceDependencies" && indent == 2) {
            if (key != "modules") {
                return Error("module_source_invalid_source_dependencies",
                    "Expected sourceDependencies.modules at line " + std::to_string(lineNumber));
            }
            sourceDependencySection = "modules";
            continue;
        }
        if (section == "sourceDependencies" && sourceDependencySection == "modules" && indent >= 6) {
            if (metadata.sourceDependencies.empty()) {
                return Error("module_source_invalid_source_dependency",
                    "Source dependency field outside list item at line " + std::to_string(lineNumber));
            }
            if (auto ok = assign_source_dependency_field(metadata.sourceDependencies.back(), key, value, lineNumber); !ok) {
                return ok.error();
            }
            continue;
        }

        return Error("module_source_parse_error", "Unexpected key at line " + std::to_string(lineNumber) + ": " + key);
    }

    if (metadata.schema != "wl2.module-source.v1") {
        return Error("module_source_invalid_schema", "Unsupported module source schema: " + metadata.schema);
    }
    if (!is_safe_name(metadata.provides)) {
        return Error("module_source_invalid_provides", "Module source metadata is missing provides");
    }
    if (metadata.version.empty()) {
        return Error("module_source_invalid_version", "Module source metadata is missing version");
    }
    for (const auto& dep : metadata.dependencies) {
        if (dep.name.empty()) {
            return Error("module_source_invalid_dependency", "Module dependency is missing name");
        }
        if (auto ok = moduleVersionSatisfies("0.0.0", dep.versionRange); !ok) {
            return ok.error();
        }
    }
    for (const auto& dep : metadata.sourceDependencies) {
        if (dep.name.empty() || dep.git.empty() || (dep.tag.empty() && dep.commit.empty())) {
            return Error("module_source_invalid_source_dependency",
                "Source dependency must include name, git, and tag or commit");
        }
    }
    return metadata;
}

Result<ModuleResolutionPlan> resolveModuleGraph(const ModuleResolutionRequest& request) {
    Resolver resolver(request);
    return resolver.run();
}

} // namespace wl2
