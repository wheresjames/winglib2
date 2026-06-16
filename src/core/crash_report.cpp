#include "wl2/crash_report.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

#if !defined(_WIN32)
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace wl2::crash {
namespace {

/// Maximum number of wl2-managed threads tracked for crash reporting.
constexpr int kMaxThreads = 64;

/// Live thread name slots. Read from the signal handler with only atomic loads,
/// so registration must never block the handler.
std::atomic<const char*> g_threads[kMaxThreads];

/// Pre-rendered report text plus the resolved output path. Built once during
/// install() and only read from the signal handler.
struct PreRendered {
    std::atomic<bool> installed{false};
    std::atomic<bool> handling{false};
    char path[4096] = {0};
    // Human body from after the signal line up to and including "threads:\n".
    std::string humanHead;
    // Text between the live thread list and the C++ stack.
    std::string humanTail;
    // JSON assembled around the live signal number, signal name, thread list,
    // and stack frame count.
    std::string jsonHead;         // {"signal":
    std::string jsonAfterSignal;  // ,"signalName":"
    std::string jsonAfterName;    // ",<static fields>,"threads":[
    std::string jsonAfterThreads; // ],"stackFrameCount":
    std::string jsonTail;         // }
};

PreRendered g_state;

/// Stable storage for the host (main) thread name pointer.
std::string g_mainThreadName = "/main";

const char* signal_name(int sig) {
#if !defined(_WIN32)
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGBUS: return "SIGBUS";
    default: break;
    }
#endif
    (void)sig;
    return "SIGNAL";
}

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += c;
            }
        }
    }
    return out;
}

void append_json_string(std::string& out, const std::string& value) {
    out += '"';
    out += json_escape(value);
    out += '"';
}

void append_json_array(std::string& out, const std::vector<std::string>& values) {
    out += '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        append_json_string(out, values[i]);
    }
    out += ']';
}

#if !defined(_WIN32)
void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void write_cstr(int fd, const char* s) {
    if (s) {
        write_all(fd, s, std::strlen(s));
    }
}

void write_str(int fd, const std::string& s) {
    write_all(fd, s.data(), s.size());
}

void write_int(int fd, long value) {
    char buf[32];
    int i = sizeof(buf);
    bool negative = value < 0;
    unsigned long magnitude = negative
        ? static_cast<unsigned long>(-(value + 1)) + 1ul
        : static_cast<unsigned long>(value);
    if (magnitude == 0) {
        buf[--i] = '0';
    }
    while (magnitude > 0) {
        buf[--i] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    }
    if (negative) {
        buf[--i] = '-';
    }
    write_all(fd, buf + i, sizeof(buf) - static_cast<size_t>(i));
}

void handle_signal(int sig) {
    // A crash inside the handler must not loop: reset and re-raise.
    if (g_state.handling.exchange(true)) {
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        _exit(128 + sig);
    }

    int fd = ::open(g_state.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char* name = signal_name(sig);
        write_cstr(fd, "== wl2 crash report ==\n");
        write_cstr(fd, "signal: ");
        write_cstr(fd, name);
        write_cstr(fd, " (");
        write_int(fd, sig);
        write_cstr(fd, ")\n");
        write_str(fd, g_state.humanHead);
        for (int i = 0; i < kMaxThreads; ++i) {
            const char* t = g_threads[i].load(std::memory_order_acquire);
            if (t) {
                write_cstr(fd, "  ");
                write_cstr(fd, t);
                write_cstr(fd, "\n");
            }
        }
        write_str(fd, g_state.humanTail);

        void* frames[128];
        int frameCount = ::backtrace(frames, 128);
        ::backtrace_symbols_fd(frames, frameCount, fd);

        write_cstr(fd, "\n--- json ---\n");
        write_str(fd, g_state.jsonHead);
        write_int(fd, sig);
        write_str(fd, g_state.jsonAfterSignal);
        write_cstr(fd, name);
        write_str(fd, g_state.jsonAfterName);
        bool first = true;
        for (int i = 0; i < kMaxThreads; ++i) {
            const char* t = g_threads[i].load(std::memory_order_acquire);
            if (t) {
                if (!first) {
                    write_cstr(fd, ",");
                }
                write_cstr(fd, "\"");
                write_cstr(fd, t);
                write_cstr(fd, "\"");
                first = false;
            }
        }
        write_str(fd, g_state.jsonAfterThreads);
        write_int(fd, frameCount);
        write_str(fd, g_state.jsonTail);

        ::fsync(fd);
        ::close(fd);
    }

    // Restore default disposition and re-raise so the process still terminates
    // with normal crash semantics.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
#endif // !_WIN32

} // namespace

void registerThread(const char* name) {
    if (!name) {
        return;
    }
    for (int i = 0; i < kMaxThreads; ++i) {
        const char* expected = nullptr;
        if (g_threads[i].compare_exchange_strong(expected, name)) {
            return;
        }
    }
}

void unregisterThread(const char* name) {
    if (!name) {
        return;
    }
    for (int i = 0; i < kMaxThreads; ++i) {
        if (g_threads[i].load(std::memory_order_acquire) == name) {
            g_threads[i].store(nullptr, std::memory_order_release);
            return;
        }
    }
}

std::optional<std::filesystem::path> install(const CrashReportConfig& config,
    const CrashReportInfo& info) {
    if (config.mode == CrashReportConfig::Mode::Off) {
        return std::nullopt;
    }
#if defined(_WIN32)
    // Windows crash handling is deferred to a later plan.
    (void)info;
    return std::nullopt;
#else
    if (g_state.installed.exchange(true)) {
        // Keep the first installation; report the path it resolved to.
        return std::filesystem::path(g_state.path);
    }

    std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    char iso[32];
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &local);

    std::filesystem::path path;
    if (config.mode == CrashReportConfig::Mode::File) {
        path = config.file;
    } else {
        std::string fileName = std::string("crash-") + stamp + "-"
            + std::to_string(static_cast<long>(::getpid())) + ".log";
        std::filesystem::path dir = config.directory.empty()
            ? std::filesystem::path(".")
            : config.directory;
        path = dir / fileName;
    }
    std::string pathStr = path.string();
    std::strncpy(g_state.path, pathStr.c_str(), sizeof(g_state.path) - 1);

    const std::string pid = std::to_string(static_cast<long>(::getpid()));
    const std::string manifest = info.manifest.empty() ? "(none)" : info.manifest;

    // Human-readable body shared by every crash, rendered once.
    std::string& head = g_state.humanHead;
    head.clear();
    head += "time: ";
    head += iso;
    head += "\npid: ";
    head += pid;
    head += "\nexecutable: ";
    head += info.executable;
    head += "\ncwd: ";
    head += info.cwd;
    head += "\nengine: ";
    head += info.engine;
    head += "\nargv:\n";
    for (const auto& arg : info.argv) {
        head += "  ";
        head += arg;
        head += "\n";
    }
    head += "manifest: ";
    head += manifest;
    head += "\nresource maps:";
    if (info.resourceMaps.empty()) {
        head += " (none)\n";
    } else {
        head += "\n";
        for (const auto& [host, logical] : info.resourceMaps) {
            head += "  ";
            head += host;
            head += " -> ";
            head += logical;
            head += "\n";
        }
    }
    head += "modules:";
    if (info.modules.empty()) {
        head += " (none)\n";
    } else {
        head += "\n";
        for (const auto& mod : info.modules) {
            head += "  ";
            head += mod;
            head += "\n";
        }
    }
    head += "threads:\n";

    g_state.humanTail = "\nC++ stack (crashing thread):\n";

    // JSON trailer, assembled from static chunks plus the live signal number,
    // signal name, thread list, and stack frame count.
    g_state.jsonHead = "{\n  \"signal\": ";
    g_state.jsonAfterSignal = ",\n  \"signalName\": \"";

    std::string& mid = g_state.jsonAfterName;
    mid = "\",\n  \"time\": ";
    append_json_string(mid, iso);
    mid += ",\n  \"pid\": ";
    mid += pid;
    mid += ",\n  \"executable\": ";
    append_json_string(mid, info.executable);
    mid += ",\n  \"cwd\": ";
    append_json_string(mid, info.cwd);
    mid += ",\n  \"engine\": ";
    append_json_string(mid, info.engine);
    mid += ",\n  \"argv\": ";
    append_json_array(mid, info.argv);
    mid += ",\n  \"manifest\": ";
    if (info.manifest.empty()) {
        mid += "null";
    } else {
        append_json_string(mid, info.manifest);
    }
    mid += ",\n  \"resourceMaps\": [";
    for (size_t i = 0; i < info.resourceMaps.size(); ++i) {
        if (i != 0) {
            mid += ",";
        }
        mid += "{\"host\": ";
        append_json_string(mid, info.resourceMaps[i].first);
        mid += ", \"logical\": ";
        append_json_string(mid, info.resourceMaps[i].second);
        mid += "}";
    }
    mid += "],\n  \"modules\": ";
    append_json_array(mid, info.modules);
    mid += ",\n  \"threads\": [";

    g_state.jsonAfterThreads = "],\n  \"stackFrameCount\": ";
    g_state.jsonTail = "\n}\n";

    // Track the host thread alongside any wl2-managed script threads.
    registerThread(g_mainThreadName.c_str());

    // Force libgcc's unwinder to load now so the handler avoids lazy allocation.
    void* warmup[1];
    (void)::backtrace(warmup, 1);

    // Run handlers on an alternate stack so stack-overflow crashes still report.
    static char altStack[65536];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ss.ss_flags = 0;
    ::sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        ::sigaction(sig, &sa, nullptr);
    }

    return path;
#endif
}

} // namespace wl2::crash
