#pragma once

/**
 * @file crash_report.h
 * @brief Fatal-signal crash reporting for the wl2 runtime.
 *
 * The crash reporter installs POSIX signal handlers that, on a fatal signal,
 * write a human-readable report followed by a machine-readable JSON trailer.
 * All report content that depends on host state is rendered when install() is
 * called, so the signal handler itself only performs async-signal-safe work
 * (writing pre-rendered text, walking the crashing thread's C++ stack, and
 * emitting a few integers).
 *
 * Windows crash handling is intentionally deferred; install() is a no-op there.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wl2::crash {

/**
 * @brief How and where a crash report should be written.
 */
struct CrashReportConfig {
    /// Reporting mode.
    enum class Mode {
        /// Do not install crash handlers.
        Off,
        /// Write `crash-YYYYMMDD-HHMMSS-PID.log` under @ref directory.
        Auto,
        /// Write to the explicit @ref file path.
        File,
    };

    /// Selected reporting mode.
    Mode mode = Mode::Auto;

    /// Explicit report path used when mode is Mode::File.
    std::filesystem::path file;

    /// Output directory for auto-named reports. Empty means the current
    /// directory.
    std::filesystem::path directory;
};

/**
 * @brief Host context captured into a crash report.
 */
struct CrashReportInfo {
    /// Absolute or invoked path of the running executable.
    std::string executable;

    /// Full process argument vector.
    std::vector<std::string> argv;

    /// Current working directory at install time.
    std::string cwd;

    /// JavaScript engine name, for example `quickjs`.
    std::string engine;

    /// Module names available to the runtime.
    std::vector<std::string> modules;

    /// Manifest path driving the run, or empty when none.
    std::string manifest;

    /// Resource directory maps as `{host, logical}` pairs.
    std::vector<std::pair<std::string, std::string>> resourceMaps;
};

/**
 * @brief Register a wl2-managed thread so it appears in crash reports.
 *
 * @param name Stable, NUL-terminated thread path. The pointer must remain
 * valid until unregisterThread() is called with the same pointer.
 */
void registerThread(const char* name);

/**
 * @brief Remove a previously registered thread.
 * @param name The same pointer passed to registerThread().
 */
void unregisterThread(const char* name);

/**
 * @brief Install crash handlers and pre-render the report.
 *
 * Installing more than once keeps the first configuration. On a platform
 * without crash support, or when @p config disables reporting, this is a no-op.
 *
 * @param config Reporting mode and destination.
 * @param info Host context captured into the report.
 * @return The report path that will be written on a crash, or std::nullopt when
 * reporting is disabled or unsupported.
 */
std::optional<std::filesystem::path> install(const CrashReportConfig& config,
    const CrashReportInfo& info);

} // namespace wl2::crash
