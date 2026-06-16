#pragma once

/**
 * @file app_store.h
 * @brief Installed app scopes, metadata, listing, resolution, and uninstall.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wl2/errors.h"
#include "wl2/module_store.h"

namespace wl2 {

/// Filesystem roots for each app install scope.
struct AppScopePaths {
    std::filesystem::path local;   ///< Project-local scope (e.g. `.wl2/apps`).
    std::filesystem::path user;    ///< Per-user scope.
    std::filesystem::path system;  ///< System-wide scope.

    /// Return the directory for @p scope.
    const std::filesystem::path& forScope(ModuleScope scope) const;
};

/// Resolve app scope directories. Honors `XDG_DATA_HOME` for user installs and
/// `WL2_SYSTEM_APP_DIR` for system-scope tests/overrides.
AppScopePaths resolveAppScopePaths(const std::filesystem::path& projectRoot = std::filesystem::current_path());

/// Metadata describing one installed app.
struct InstalledAppRecord {
    std::string name;                       ///< App name.
    std::string version;                    ///< Installed version.
    std::filesystem::path executablePath;   ///< Path to the app executable.
    std::filesystem::path launcherPath;     ///< Path to the generated launcher.
    std::filesystem::path payloadPath;      ///< Path to the installed payload metadata.
    ModuleScope scope = ModuleScope::Local; ///< Scope the app is installed in.
    bool shadowed = false;                  ///< True when a higher-priority scope hides this record.
};

/// Inputs describing an app to install.
struct AppInstallPayload {
    std::string name;                      ///< App name.
    std::string version = "0.1.0";         ///< Version to record.
    std::filesystem::path executablePath;  ///< Source executable to install.
    std::string source;                    ///< Origin descriptor (e.g. repo spec), if any.
    std::string ref;                       ///< Source ref (branch/tag/commit), if any.
    std::string path;                      ///< Subpath within the source, if any.
};

/// Parsed `<repo[#ref][:path]>` app source specification.
struct AppSourceSpec {
    std::string repo;  ///< Repository URL or shorthand.
    std::string ref;   ///< Ref (branch, tag, or commit), if any.
    std::string path;  ///< Subpath within the repository, if any.
};

/// Normalize `<repo[#ref][:path]>` and GitHub `/tree/<ref>/<path>` URLs.
std::optional<AppSourceSpec> normalizeAppSource(std::string_view input);

/// Installs, lists, resolves, and removes apps across the scope directories.
class AppStore {
public:
    /// Construct a store backed by the given scope @p paths.
    explicit AppStore(AppScopePaths paths);

    /// Install @p payload into @p scope, returning the resulting record.
    Result<InstalledAppRecord> install(const AppInstallPayload& payload, ModuleScope scope);
    /// Remove app @p name; @p scope narrows the search, @p purgeCache also drops cached data.
    Result<void> uninstall(const std::string& name, std::optional<ModuleScope> scope, bool purgeCache);
    /// List installed apps across all scopes (highest-priority scope wins).
    std::vector<InstalledAppRecord> list() const;
    /// Resolve the effective record for app @p name, if installed.
    std::optional<InstalledAppRecord> resolve(const std::string& name) const;
    /// List apps installed in a single @p scope.
    std::vector<InstalledAppRecord> scanScope(ModuleScope scope) const;

private:
    AppScopePaths paths_;
};

/// Normalize an app name into a filesystem-safe slug.
std::string appSlug(std::string_view name);

} // namespace wl2
