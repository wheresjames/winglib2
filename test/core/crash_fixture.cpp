// Crash report test fixture.
//
// Configures and installs the wl2 crash reporter from command-line arguments,
// then deliberately crashes so tests can inspect the resulting report. This
// mirrors how the wl2 runner wires CrashReportInfo, but lets a test control the
// captured metadata and the crash signal.
//
// Usage:
//   wl2_crash_fixture --report <off|auto|PATH> [--dir DIR] [--manifest PATH]
//                    [--module NAME]... [--thread NAME]... [--map HOST:LOGICAL]...
//                    [--crash <abort|segv>]

#include "wl2/crash_report.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    wl2::crash::CrashReportConfig config;
    wl2::crash::CrashReportInfo info;
    info.executable = argc > 0 ? argv[0] : "wl2_crash_fixture";
    info.engine = "fixture-engine";
    std::vector<std::string> extraThreads;
    std::string crashKind = "abort";

    for (int i = 0; i < argc; ++i) {
        info.argv.emplace_back(argv[i]);
    }

    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << argv[i] << "\n";
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--report") {
            std::string value = next(i);
            if (value == "off") {
                config.mode = wl2::crash::CrashReportConfig::Mode::Off;
            } else if (value == "auto") {
                config.mode = wl2::crash::CrashReportConfig::Mode::Auto;
            } else {
                config.mode = wl2::crash::CrashReportConfig::Mode::File;
                config.file = value;
            }
        } else if (arg == "--dir") {
            config.directory = next(i);
        } else if (arg == "--manifest") {
            info.manifest = next(i);
        } else if (arg == "--module") {
            info.modules.push_back(next(i));
        } else if (arg == "--thread") {
            extraThreads.push_back(next(i));
        } else if (arg == "--map") {
            std::string value = next(i);
            auto colon = value.find(':');
            if (colon == std::string::npos) {
                std::cerr << "invalid --map value: " << value << "\n";
                return 2;
            }
            info.resourceMaps.emplace_back(value.substr(0, colon), value.substr(colon + 1));
        } else if (arg == "--crash") {
            crashKind = next(i);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    info.cwd = ".";

    // Register extra threads so they appear alongside the host thread. The
    // backing strings outlive the crash, so their pointers stay valid.
    for (const auto& name : extraThreads) {
        wl2::crash::registerThread(name.c_str());
    }

    wl2::crash::install(config, info);

    if (crashKind == "segv") {
        volatile int* pointer = nullptr;
        *pointer = 1;
    } else {
        std::abort();
    }

    // Unreachable: the crash above terminates the process.
    return 0;
}
