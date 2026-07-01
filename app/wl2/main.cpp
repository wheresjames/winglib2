#include "wl2/app_store.h"
#include "wl2/wl2.h"
#include "wl2/crash_report.h"
#include "wl2/manifest.h"
#include "wl2/module_deps.h"
#include "wl2/module_resolver.h"
#include "wl2/module_store.h"

#include <algorithm>
#include <set>

#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

void wl2_register_builtin_static_modules(wl2::RuntimeOptions& options);
void wl2_append_builtin_static_module_names(std::vector<std::string>& names);

namespace {

// Module provider assembly helpers, defined later in this file and used by the
// run/config/graph commands to build a resolver request.
void append_installed_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const std::filesystem::path& projectRoot);
wl2::Result<void> append_builtin_module_providers(std::vector<wl2::ModuleProvider>& providers);
wl2::Result<void> append_explicit_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const std::vector<std::filesystem::path>& paths);
void append_project_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const wl2::ResourceManifest& manifest);

enum class StackTraceMode {
    Auto,
    On,
    Off,
};

struct RunCommand {
    std::string script;
    std::filesystem::path manifestPath;
    std::vector<std::string> scriptArgs;
    std::vector<wl2::ResourceDirectoryMount> resourceMaps;
    std::vector<std::string> requiredModules;
    std::vector<std::string> optionalModules;
    std::vector<wl2::DynamicModuleSpec> dynamicModules;
    StackTraceMode stackTraces = StackTraceMode::Auto;
    bool traceResources = false;
    bool allowModuleShadow = false;
    bool watch = false;
    bool allowNetwork = false;
    std::vector<std::string> networkAllowList;
    bool allowListening = false;
    std::vector<std::string> listenAllowList;
    bool allowUi = false;
    bool allowGraphics = false;
    bool allowSharedMemory = false;
    bool interactivePermissions = true;
    std::vector<std::string> sharedMemoryAllowList;
    wl2::crash::CrashReportConfig crashReport;
};

// Full process argument vector, captured in main() so crash reports can record
// how the runner was invoked.
std::vector<std::string> g_processArgv;

struct ResourceCommand {
    std::string action;
    std::string path = "wl2:/";
    std::filesystem::path executablePath;
    std::filesystem::path manifestPath;
    std::filesystem::path outDir;
    std::vector<wl2::ResourceDirectoryMount> resourceMaps;
    bool raw = false;
};

struct TestCommand {
    std::filesystem::path manifestPath = "wl2.yml";
    std::string filter;
    bool json = false;
};

void usage(std::ostream& out = std::cerr) {
    out
        << "usage:\n"
        << "  wl2 run [--manifest wl2.yml] [--watch] [--stack-traces=auto|on|off] [--map-resource host:wl2:/prefix] [--trace-resources] [--load-module path] [--allow-module-shadow] [--allow ui,graphics,shared-memory[:prefix]] [--allow-network] [--network-allow host[:port]] [--allow-listen] [--listen-allow host[:port]] [--allow-ui] [--allow-graphics] [--allow-shared-memory] [--shared-memory-allow prefix] [--no-permission-prompt] [--crash-report=off|auto|<path>] [--crash-report-dir dir] [script] [-- script-args...]\n"
        << "  wl2 config [--manifest wl2.yml] [--json] [--map-resource host:wl2:/prefix] [--load-module path]\n"
        << "  wl2 resources <list|read|extract> [--manifest wl2.yml] [--map-resource host:wl2:/prefix] [executable] [path] [--out dir] [--raw]\n"
        << "  wl2 module validate <library-path>\n"
        << "  wl2 module install <library-path> --scope local|user|system\n"
        << "  wl2 module uninstall <name> [--scope local|user|system] [--force] [--purge-cache]\n"
        << "  wl2 module list [--scope all|local|user|system]\n"
        << "  wl2 module graph --manifest wl2.yml [--json] [--load-module path]\n"
        << "  wl2 module new <name>\n"
        << "  wl2 app install <repo[#ref][:path]> --scope local|user|system\n"
        << "  wl2 app list [--scope all|local|user|system]\n"
        << "  wl2 app run <name-or-path> [-- args...]\n"
        << "  wl2 app uninstall <name> [--scope local|user|system] [--purge-cache]\n"
        << "  wl2 deps <lock|fetch|build|install|status> [--manifest wl2.yml] [--prefix dir] [--generator name] [--build-type type]\n"
        << "  wl2 test [--manifest wl2.yml] [--filter text] [--json]\n"
        << "  wl2 init <name>\n"
        << "  wl2 showapi <module>\n"
        << "  wl2 graphics\n"
        << "  wl2 version\n";
}

bool is_planned_subcommand(const std::string& command) {
    (void)command;
    return false;
}

std::optional<StackTraceMode> parse_stack_traces(std::string_view arg) {
    constexpr std::string_view prefix = "--stack-traces=";
    if (arg.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    auto value = arg.substr(prefix.size());
    if (value == "auto") {
        return StackTraceMode::Auto;
    }
    if (value == "on") {
        return StackTraceMode::On;
    }
    if (value == "off") {
        return StackTraceMode::Off;
    }
    return std::nullopt;
}

bool apply_crash_report_value(wl2::crash::CrashReportConfig& config, std::string_view value) {
    if (value == "off") {
        config.mode = wl2::crash::CrashReportConfig::Mode::Off;
        return true;
    }
    if (value == "auto") {
        config.mode = wl2::crash::CrashReportConfig::Mode::Auto;
        return true;
    }
    if (value.empty()) {
        std::cerr << "--crash-report requires off, auto, or a file path\n";
        return false;
    }
    config.mode = wl2::crash::CrashReportConfig::Mode::File;
    config.file = std::filesystem::path(value);
    return true;
}

std::optional<wl2::ResourceDirectoryMount> parse_resource_map(const std::string& value) {
    auto marker = value.find(":wl2:/");
    if (marker == std::string::npos || marker == 0) {
        std::cerr << "invalid --map-resource value; expected host-dir:wl2:/prefix\n";
        return std::nullopt;
    }
    return wl2::ResourceDirectoryMount{
        std::filesystem::path(value.substr(0, marker)),
        value.substr(marker + 1),
    };
}

bool append_resource_map(std::vector<wl2::ResourceDirectoryMount>& maps, const std::string& value) {
    auto map = parse_resource_map(value);
    if (!map) {
        return false;
    }
    maps.push_back(std::move(*map));
    return true;
}

std::vector<std::string> split_csv(std::string_view value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = comma == std::string_view::npos ? value.size() : comma;
        std::string item(value.substr(start, end - start));
        const auto first = item.find_first_not_of(" \t\r\n");
        const auto last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            out.push_back(item.substr(first, last - first + 1));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

bool apply_allow_token(RunCommand& command, const std::string& token) {
    auto colon = token.find(':');
    const std::string name = colon == std::string::npos ? token : token.substr(0, colon);
    const std::string value = colon == std::string::npos ? std::string() : token.substr(colon + 1);
    if (name == "ui") {
        if (!value.empty()) {
            std::cerr << "--allow ui does not take a value\n";
            return false;
        }
        command.allowUi = true;
        return true;
    }
    if (name == "graphics") {
        if (!value.empty()) {
            std::cerr << "--allow graphics does not take a value\n";
            return false;
        }
        command.allowGraphics = true;
        return true;
    }
    if (name == "shared-memory" || name == "shared_memory") {
        command.allowSharedMemory = true;
        if (!value.empty()) {
            command.sharedMemoryAllowList.push_back(value);
        }
        return true;
    }
    if (name == "network") {
        command.allowNetwork = true;
        if (!value.empty()) {
            command.networkAllowList.push_back(value);
        }
        return true;
    }
    if (name == "listen" || name == "listening") {
        command.allowListening = true;
        if (!value.empty()) {
            command.listenAllowList.push_back(value);
        }
        return true;
    }
    std::cerr << "unknown --allow capability: " << token << '\n';
    return false;
}

bool apply_allow_value(RunCommand& command, std::string_view value) {
    auto tokens = split_csv(value);
    if (tokens.empty()) {
        std::cerr << "--allow requires a capability list\n";
        return false;
    }
    for (const auto& token : tokens) {
        if (!apply_allow_token(command, token)) {
            return false;
        }
    }
    return true;
}

bool stdin_is_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

bool should_print_details(StackTraceMode mode, const wl2::Error& error) {
    if (mode == StackTraceMode::Off) {
        return false;
    }
    return !error.details().empty();
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += "\\u00";
                    constexpr char hex[] = "0123456789abcdef";
                    out.push_back(hex[(ch >> 4) & 0x0f]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(ch);
                }
        }
    }
    return out;
}

std::string js_string_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string wl2_engine_name() {
#if WL2_JS_ENGINE_V8
    return "v8";
#else
    return "quickjs";
#endif
}

std::string sanitize_identifier(std::string_view value) {
    std::string out;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        } else if (ch == '-' || ch == '_' || ch == ':' || ch == '.') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out = "sample";
    }
    return out;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "unable to write " << path.string() << '\n';
        return false;
    }
    out << text;
    return true;
}

std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

int run_shell_command(const std::string& command) {
    return std::system(command.c_str());
}

bool command_available(const std::string& name) {
    return run_shell_command("command -v " + shell_quote(name) + " >/dev/null 2>&1") == 0;
}

bool executable_file(const std::filesystem::path& path) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return false;
    }
    auto perms = status.permissions();
    return (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none
        || (perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none
        || (perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
}


std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::optional<std::string> json_string_field(const std::string& text, const std::string& key) {
    std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch match;
    if (std::regex_search(text, match, pattern)) {
        return match[1].str();
    }
    return std::nullopt;
}

std::vector<std::string> json_string_array_field(const std::string& text, const std::string& key) {
    std::vector<std::string> values;
    std::regex arrayPattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
    std::smatch arrayMatch;
    if (!std::regex_search(text, arrayMatch, arrayPattern)) {
        return values;
    }
    std::string body = arrayMatch[1].str();
    std::regex valuePattern("\"([^\"]*)\"");
    for (std::sregex_iterator it(body.begin(), body.end(), valuePattern), end; it != end; ++it) {
        values.push_back((*it)[1].str());
    }
    return values;
}

int base64_vlq_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::optional<std::string> decode_base64(std::string_view text) {
    std::string out;
    int value = 0;
    int bits = -8;
    for (char ch : text) {
        if (ch == '=') {
            break;
        }
        int digit = base64_vlq_value(ch);
        if (digit < 0) {
            if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
                continue;
            }
            return std::nullopt;
        }
        value = (value << 6) + digit;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
}

bool decode_vlq_segment(std::string_view segment, std::vector<int>& values) {
    values.clear();
    int value = 0;
    int shift = 0;
    for (char ch : segment) {
        int digit = base64_vlq_value(ch);
        if (digit < 0) {
            return false;
        }
        bool continuation = (digit & 32) != 0;
        digit &= 31;
        value += digit << shift;
        if (continuation) {
            shift += 5;
            continue;
        }
        const bool negative = (value & 1) != 0;
        values.push_back(negative ? -(value >> 1) : (value >> 1));
        value = 0;
        shift = 0;
    }
    return shift == 0;
}

struct SourceMapMapping {
    int generatedLine = 1;
    int generatedColumn = 0;
    int sourceIndex = 0;
    int originalLine = 1;
    int originalColumn = 0;
};

std::optional<std::vector<SourceMapMapping>> parse_source_map_mappings(const std::string& mappings) {
    std::vector<SourceMapMapping> out;
    int source = 0;
    int originalLine = 0;
    int originalColumn = 0;
    int generatedLine = 1;
    size_t lineStart = 0;
    while (lineStart <= mappings.size()) {
        size_t lineEnd = mappings.find(';', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = mappings.size();
        }
        int generatedColumn = 0;
        size_t segmentStart = lineStart;
        while (segmentStart <= lineEnd) {
            size_t segmentEnd = mappings.find(',', segmentStart);
            if (segmentEnd == std::string::npos || segmentEnd > lineEnd) {
                segmentEnd = lineEnd;
            }
            if (segmentEnd > segmentStart) {
                std::vector<int> values;
                if (!decode_vlq_segment(std::string_view(mappings).substr(segmentStart, segmentEnd - segmentStart), values)) {
                    return std::nullopt;
                }
                if (!values.empty()) {
                    generatedColumn += values[0];
                    if (values.size() >= 4) {
                        source += values[1];
                        originalLine += values[2];
                        originalColumn += values[3];
                        out.push_back(SourceMapMapping{
                            generatedLine,
                            generatedColumn,
                            source,
                            originalLine + 1,
                            originalColumn});
                    }
                }
            }
            if (segmentEnd == lineEnd) {
                break;
            }
            segmentStart = segmentEnd + 1;
        }
        if (lineEnd == mappings.size()) {
            break;
        }
        ++generatedLine;
        lineStart = lineEnd + 1;
    }
    return out;
}

std::optional<std::string> remap_stack_location(const std::string& file, int line, int column) {
    if (file.rfind("wl2:/", 0) == 0 || file.rfind("file:", 0) == 0) {
        return std::nullopt;
    }
    auto generated = read_text_file(file);
    if (!generated) {
        return std::nullopt;
    }
    std::regex marker("//#\\s*sourceMappingURL=([^\\s]+)");
    std::smatch markerMatch;
    std::string::const_iterator searchStart(generated->cbegin());
    std::optional<std::string> mapName;
    while (std::regex_search(searchStart, generated->cend(), markerMatch, marker)) {
        mapName = markerMatch[1].str();
        searchStart = markerMatch.suffix().first;
    }
    if (!mapName) {
        return std::nullopt;
    }
    std::filesystem::path mapPath = std::filesystem::path(file).parent_path();
    std::optional<std::string> mapText;
    if (mapName->rfind("data:application/json;base64,", 0) == 0) {
        mapText = decode_base64(std::string_view(*mapName).substr(std::string_view("data:application/json;base64,").size()));
    } else {
        mapPath = mapPath / *mapName;
        mapText = read_text_file(mapPath);
    }
    if (!mapText) {
        return std::nullopt;
    }
    auto mappingsText = json_string_field(*mapText, "mappings");
    auto sources = json_string_array_field(*mapText, "sources");
    if (!mappingsText || sources.empty()) {
        return std::nullopt;
    }
    auto mappings = parse_source_map_mappings(*mappingsText);
    if (!mappings) {
        return std::nullopt;
    }
    const SourceMapMapping* best = nullptr;
    for (const auto& mapping : *mappings) {
        if (mapping.generatedLine != line) {
            continue;
        }
        if (mapping.generatedColumn <= column && (!best || mapping.generatedColumn >= best->generatedColumn)) {
            best = &mapping;
        }
    }
    if (!best || best->sourceIndex < 0 || static_cast<size_t>(best->sourceIndex) >= sources.size()) {
        return std::nullopt;
    }
    auto sourceBase = std::filesystem::is_directory(mapPath) ? mapPath : mapPath.parent_path();
    auto sourcePath = (sourceBase / sources[best->sourceIndex]).lexically_normal();
    const int mappedColumn = best->originalColumn + std::max(0, column - best->generatedColumn);
    return sourcePath.string() + ":" + std::to_string(best->originalLine) + ":" + std::to_string(mappedColumn);
}

std::string remap_stack_details(const std::string& details) {
    std::regex locationPattern("([^\\s\\(\\)]+\\.js):(\\d+):(\\d+)");
    std::string out;
    std::string::const_iterator searchStart(details.cbegin());
    std::smatch match;
    while (std::regex_search(searchStart, details.cend(), match, locationPattern)) {
        out.append(searchStart, match[0].first);
        const auto remapped = remap_stack_location(match[1].str(), std::stoi(match[2].str()), std::stoi(match[3].str()));
        out += remapped.value_or(match[0].str());
        searchStart = match[0].second;
    }
    out.append(searchStart, details.cend());
    return out;
}

void print_error(const wl2::Error& error, StackTraceMode stackTraces) {
    std::cerr << error.code() << ": " << error.message() << '\n';
    if (should_print_details(stackTraces, error)) {
        std::cerr << '\n' << remap_stack_details(error.details()) << '\n';
    }
}

bool apply_manifest(
    const std::filesystem::path& manifestPath,
    std::vector<wl2::ResourceDirectoryMount>& maps,
    std::string* script,
    wl2::ResourceManifest* loadedManifest = nullptr) {
    auto manifest = wl2::loadResourceManifest(manifestPath);
    if (!manifest) {
        print_error(manifest.error(), StackTraceMode::Auto);
        return false;
    }
    maps.push_back(wl2::ResourceDirectoryMount{
        manifest.value().resolvedRoot(),
        manifest.value().prefix,
        manifest.value().excludePatterns,
        manifest.value().compress.files,
        manifest.value().compress.directories});
    if (script && script->empty()) {
        *script = manifest.value().entrySpecifier();
    }
    if (loadedManifest) {
        *loadedManifest = std::move(manifest.value());
    }
    return true;
}

std::optional<RunCommand> parse_run_command(int argc, char** argv, int start, bool legacy) {
    RunCommand command;
    bool scriptArgs = false;
    for (int i = start; i < argc; ++i) {
        std::string arg(argv[i]);
        if (scriptArgs) {
            command.scriptArgs.push_back(std::move(arg));
            continue;
        }
        if (arg == "--") {
            scriptArgs = true;
            continue;
        }
        if (arg == "--trace-resources") {
            command.traceResources = true;
            continue;
        }
        if (arg == "--watch") {
            command.watch = true;
            continue;
        }
        if (arg == "--allow-module-shadow") {
            command.allowModuleShadow = true;
            continue;
        }
        if (arg == "--allow") {
            if (++i >= argc || !apply_allow_value(command, argv[i])) {
                return std::nullopt;
            }
            continue;
        }
        constexpr std::string_view allowPrefix = "--allow=";
        if (arg.rfind(allowPrefix, 0) == 0) {
            if (!apply_allow_value(command, arg.substr(allowPrefix.size()))) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--allow-network") {
            command.allowNetwork = true;
            continue;
        }
        if (arg == "--network-allow") {
            if (++i >= argc) {
                std::cerr << "--network-allow requires a host[:port] entry\n";
                return std::nullopt;
            }
            command.allowNetwork = true;
            command.networkAllowList.emplace_back(argv[i]);
            continue;
        }
        constexpr std::string_view networkAllowPrefix = "--network-allow=";
        if (arg.rfind(networkAllowPrefix, 0) == 0) {
            command.allowNetwork = true;
            command.networkAllowList.emplace_back(arg.substr(networkAllowPrefix.size()));
            continue;
        }
        if (arg == "--allow-listen") {
            command.allowListening = true;
            continue;
        }
        if (arg == "--listen-allow") {
            if (++i >= argc) {
                std::cerr << "--listen-allow requires a host[:port] entry\n";
                return std::nullopt;
            }
            command.allowListening = true;
            command.listenAllowList.emplace_back(argv[i]);
            continue;
        }
        constexpr std::string_view listenAllowPrefix = "--listen-allow=";
        if (arg.rfind(listenAllowPrefix, 0) == 0) {
            command.allowListening = true;
            command.listenAllowList.emplace_back(arg.substr(listenAllowPrefix.size()));
            continue;
        }
        if (arg == "--allow-ui") {
            command.allowUi = true;
            continue;
        }
        if (arg == "--allow-graphics") {
            command.allowGraphics = true;
            continue;
        }
        if (arg == "--allow-shared-memory") {
            command.allowSharedMemory = true;
            continue;
        }
        if (arg == "--no-permission-prompt" || arg == "--non-interactive") {
            command.interactivePermissions = false;
            continue;
        }
        if (arg == "--shared-memory-allow") {
            if (++i >= argc) {
                std::cerr << "--shared-memory-allow requires a name prefix\n";
                return std::nullopt;
            }
            command.allowSharedMemory = true;
            command.sharedMemoryAllowList.emplace_back(argv[i]);
            continue;
        }
        constexpr std::string_view sharedMemoryAllowPrefix = "--shared-memory-allow=";
        if (arg.rfind(sharedMemoryAllowPrefix, 0) == 0) {
            command.allowSharedMemory = true;
            command.sharedMemoryAllowList.emplace_back(arg.substr(sharedMemoryAllowPrefix.size()));
            continue;
        }
        if (arg == "--crash-report") {
            if (++i >= argc || !apply_crash_report_value(command.crashReport, argv[i])) {
                return std::nullopt;
            }
            continue;
        }
        constexpr std::string_view crashReportPrefix = "--crash-report=";
        if (arg.rfind(crashReportPrefix, 0) == 0) {
            if (!apply_crash_report_value(command.crashReport, arg.substr(crashReportPrefix.size()))) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--crash-report-dir") {
            if (++i >= argc) {
                std::cerr << "--crash-report-dir requires a directory\n";
                return std::nullopt;
            }
            command.crashReport.directory = argv[i];
            continue;
        }
        constexpr std::string_view crashReportDirPrefix = "--crash-report-dir=";
        if (arg.rfind(crashReportDirPrefix, 0) == 0) {
            command.crashReport.directory = std::string(arg.substr(crashReportDirPrefix.size()));
            continue;
        }
        if (arg == "--load-module") {
            if (++i >= argc) {
                std::cerr << "--load-module requires a library path\n";
                return std::nullopt;
            }
            command.dynamicModules.push_back(wl2::DynamicModuleSpec{argv[i], false});
            continue;
        }
        constexpr std::string_view loadModulePrefix = "--load-module=";
        if (arg.rfind(loadModulePrefix, 0) == 0) {
            command.dynamicModules.push_back(
                wl2::DynamicModuleSpec{std::string(arg.substr(loadModulePrefix.size())), false});
            continue;
        }
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return std::nullopt;
            }
            command.manifestPath = argv[i];
            continue;
        }
        constexpr std::string_view manifestPrefix = "--manifest=";
        if (arg.rfind(manifestPrefix, 0) == 0) {
            command.manifestPath = arg.substr(manifestPrefix.size());
            continue;
        }
        if (arg == "--map-resource") {
            if (++i >= argc || !append_resource_map(command.resourceMaps, argv[i])) {
                return std::nullopt;
            }
            continue;
        }
        constexpr std::string_view mapPrefix = "--map-resource=";
        if (arg.rfind(mapPrefix, 0) == 0) {
            if (!append_resource_map(command.resourceMaps, arg.substr(mapPrefix.size()))) {
                return std::nullopt;
            }
            continue;
        }
        if (auto mode = parse_stack_traces(arg)) {
            command.stackTraces = *mode;
            continue;
        }
        if (arg.rfind("--stack-traces=", 0) == 0) {
            std::cerr << "invalid --stack-traces value; expected auto, on, or off\n";
            return std::nullopt;
        }
        if (!command.script.empty()) {
            // Preserve old wl2 <script> arg1 arg2 behavior while `run --` becomes
            // the documented form.
            command.scriptArgs.push_back(std::move(arg));
            continue;
        }
        if (!legacy && arg.rfind("-", 0) == 0) {
            std::cerr << "unknown run option: " << arg << '\n';
            return std::nullopt;
        }
        command.script = std::move(arg);
    }
    std::filesystem::path projectRoot = std::filesystem::current_path();
    wl2::ResourceManifest manifest;
    bool hasManifest = false;
    if (!command.manifestPath.empty()) {
        if (!apply_manifest(command.manifestPath, command.resourceMaps, &command.script, &manifest)) {
            return std::nullopt;
        }
        command.requiredModules = manifest.requiredModules;
        command.optionalModules = manifest.optionalModules;
        command.allowUi = command.allowUi || manifest.allowUi;
        projectRoot = manifest.baseDir;
        hasManifest = true;
    }
    // Explicit --load-module overrides honor the global shadow flag.
    for (auto& spec : command.dynamicModules) {
        spec.allowShadow = command.allowModuleShadow;
    }
    // Resolve manifest-declared modules and their transitive module dependencies
    // into a dependency-first dynamic load order. The resolver selects one
    // provider per canonical name across explicit, project, installed, and
    // built-in sources. Built-in static modules are satisfied by the runtime, so
    // only installed/explicit providers contribute dynamic load specs.
    {
        std::vector<std::filesystem::path> explicitPaths;
        explicitPaths.reserve(command.dynamicModules.size());
        for (const auto& spec : command.dynamicModules) {
            explicitPaths.push_back(spec.path);
        }

        wl2::ModuleResolutionRequest request;
        for (const auto& name : command.requiredModules) {
            request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Required});
        }
        for (const auto& name : command.optionalModules) {
            request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Optional});
        }

        bool providersOk = true;
        if (auto ok = append_explicit_module_providers(request.providers, explicitPaths); !ok) {
            std::cerr << "error: " << ok.error().message() << '\n';
            return std::nullopt;
        }
        if (hasManifest) {
            append_project_module_providers(request.providers, manifest);
        }
        append_installed_module_providers(request.providers, projectRoot);
        if (auto ok = append_builtin_module_providers(request.providers); !ok) {
            providersOk = false;
        }

        std::set<std::filesystem::path> queued(explicitPaths.begin(), explicitPaths.end());
        bool applied = false;
        if (providersOk && !request.roots.empty()) {
            if (auto plan = wl2::resolveModuleGraph(request)) {
                // Project source dependencies load from their installed local
                // library once built; resolve them through the local scope.
                wl2::ModuleStore store(wl2::resolveModuleScopePaths(projectRoot));
                auto queueLibrary = [&](const std::filesystem::path& path) {
                    if (!path.empty() && queued.insert(path).second) {
                        command.dynamicModules.push_back(wl2::DynamicModuleSpec{path, true});
                    }
                };
                const auto installedRecords = store.list();
                // Report a verification failure, returning false (abort) only for a
                // required module; an optional one is skipped with a diagnostic.
                auto reportVerifyFailure = [](const wl2::Error& error, bool optional) -> bool {
                    if (optional) {
                        std::cerr << "warning: " << error.message()
                                  << "; skipping optional module\n";
                        return true;
                    }
                    std::cerr << "error: " << error.message() << '\n';
                    return false;
                };
                // Verify the exact installed library the resolver selected against
                // its recorded checksum before loading it, then queue it.
                auto verifySelectedLibrary = [&](const std::filesystem::path& path,
                                                 bool optional) -> bool {
                    for (const auto& record : installedRecords) {
                        if (record.libraryPath != path) {
                            continue;
                        }
                        if (auto ok = store.verifyInstalled(record); !ok) {
                            return reportVerifyFailure(ok.error(), optional);
                        }
                        break;
                    }
                    queueLibrary(path);
                    return true;
                };
                // Explicit --load-module specs are already queued; append the
                // installed providers the resolver selected, in topological order.
                for (const auto& resolved : plan.value().loadOrder) {
                    const auto& provider = resolved.provider;
                    switch (provider.source) {
                        case wl2::ModuleProvider::Source::Local:
                        case wl2::ModuleProvider::Source::User:
                        case wl2::ModuleProvider::Source::System:
                            if (!verifySelectedLibrary(provider.path, resolved.optional)) {
                                return std::nullopt;
                            }
                            break;
                        case wl2::ModuleProvider::Source::Project:
                            // Project source modules load from their installed local
                            // library; provider.path is metadata, not a library.
                            if (auto record = store.resolve(provider.info.name)) {
                                if (auto ok = store.verifyInstalled(*record); !ok) {
                                    if (!reportVerifyFailure(ok.error(), resolved.optional)) {
                                        return std::nullopt;
                                    }
                                } else {
                                    queueLibrary(record->libraryPath);
                                }
                            }
                            break;
                        default:
                            break;  // built-in/explicit need no new dynamic spec
                    }
                }
                applied = true;
            }
        }
        if (!applied) {
            // The graph could not be resolved (typically a required module is not
            // available). Fall back to direct installed-scope resolution so the
            // runtime still loads what is present and reports module_required_missing
            // for genuinely missing required modules.
            wl2::ModuleStore store(wl2::resolveModuleScopePaths(projectRoot));
            auto resolveName = [&](const std::string& name, bool optional) -> bool {
                if (auto record = store.resolve(name)) {
                    if (auto ok = store.verifyInstalled(*record); !ok) {
                        if (optional) {
                            std::cerr << "warning: " << ok.error().message()
                                      << "; skipping optional module\n";
                            return true;
                        }
                        std::cerr << "error: " << ok.error().message() << '\n';
                        return false;
                    }
                    if (queued.insert(record->libraryPath).second) {
                        command.dynamicModules.push_back(
                            wl2::DynamicModuleSpec{record->libraryPath, true});
                    }
                }
                return true;
            };
            for (const auto& name : command.requiredModules) {
                if (!resolveName(name, false)) {
                    return std::nullopt;
                }
            }
            for (const auto& name : command.optionalModules) {
                if (!resolveName(name, true)) {
                    return std::nullopt;
                }
            }
        }
    }
    if (command.script.empty()) {
        usage();
        return std::nullopt;
    }
    return command;
}

void install_crash_reporter(const RunCommand& command) {
    wl2::crash::CrashReportInfo info;
    if (!g_processArgv.empty()) {
        info.executable = g_processArgv.front();
        info.argv = g_processArgv;
    }
    std::error_code ec;
    info.cwd = std::filesystem::current_path(ec).string();
    info.engine = wl2_engine_name();
    if (!command.manifestPath.empty()) {
        info.manifest = command.manifestPath.string();
    }
    for (const auto& mount : command.resourceMaps) {
        info.resourceMaps.emplace_back(mount.root.string(), mount.prefix);
    }
    wl2_append_builtin_static_module_names(info.modules);
    for (const auto& name : command.requiredModules) {
        info.modules.push_back(name);
    }
    for (const auto& name : command.optionalModules) {
        info.modules.push_back(name);
    }
    wl2::crash::install(command.crashReport, info);
}

int run_script(wl2::RuntimeOptions options, const RunCommand& command) {
    install_crash_reporter(command);
    options.scriptArgs = command.scriptArgs;
    options.resourceDirectoryMounts = command.resourceMaps;
    options.requiredModules = command.requiredModules;
    options.optionalModules = command.optionalModules;
    options.dynamicModules = command.dynamicModules;
    options.traceResourceLookups = command.traceResources;
    options.allowNetwork = command.allowNetwork;
    options.networkAllowList = command.networkAllowList;
    options.allowListening = command.allowListening;
    options.listenAllowList = command.listenAllowList;
    options.allowUi = command.allowUi;
    options.allowGraphics = command.allowGraphics;
    options.allowSharedMemory = command.allowSharedMemory;
    options.sharedMemoryAllowList = command.sharedMemoryAllowList;
    options.interactivePermissions = command.interactivePermissions && stdin_is_terminal();
    wl2::Runtime runtime(std::move(options));
    auto result = runtime.runModule(command.script);
    if (!result) {
        print_error(result.error(), command.stackTraces);
        return 1;
    }
    return result.value();
}

using WatchSnapshot = std::map<std::filesystem::path, std::filesystem::file_time_type>;

void snapshot_file(WatchSnapshot& snapshot, const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        snapshot[std::filesystem::absolute(path, ec)] = std::filesystem::last_write_time(path, ec);
    }
}

void snapshot_tree(WatchSnapshot& snapshot, const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        snapshot_file(snapshot, root);
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            snapshot_file(snapshot, entry.path());
        }
    }
}

struct WatchInputs {
    WatchSnapshot rerun;
    WatchSnapshot native;
};

WatchInputs collect_watch_inputs(const RunCommand& command) {
    WatchInputs inputs;
    if (!command.manifestPath.empty()) {
        snapshot_file(inputs.rerun, command.manifestPath);
        const auto baseDir = command.manifestPath.parent_path().empty()
            ? std::filesystem::current_path()
            : std::filesystem::absolute(command.manifestPath.parent_path());
        snapshot_file(inputs.rerun, baseDir / "wl2.lock.yml");
    }
    if (!command.script.empty() && command.script.rfind("wl2:/", 0) != 0 && command.script.rfind("file:", 0) != 0) {
        snapshot_file(inputs.rerun, command.script);
    }
    for (const auto& mount : command.resourceMaps) {
        snapshot_tree(inputs.rerun, mount.root);
    }
    for (const auto& spec : command.dynamicModules) {
        snapshot_file(inputs.native, spec.path);
        snapshot_file(inputs.native, spec.path.parent_path() / "wl2.module.yml");
    }
    return inputs;
}

std::vector<std::filesystem::path> changed_paths(const WatchSnapshot& before, const WatchSnapshot& after) {
    std::vector<std::filesystem::path> changed;
    for (const auto& [path, time] : before) {
        auto found = after.find(path);
        if (found == after.end() || found->second != time) {
            changed.push_back(path);
        }
    }
    for (const auto& [path, time] : after) {
        if (!before.contains(path)) {
            changed.push_back(path);
        }
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    return changed;
}

int watch_limit_from_env() {
    const char* value = std::getenv("WL2_WATCH_EXIT_AFTER_RERUNS");
    if (!value || !*value) {
        return 0;
    }
    return std::max(0, std::atoi(value));
}

int watch_run(wl2::RuntimeOptions options, const RunCommand& command) {
    int runs = 0;
    int last = run_script(options, command);
    ++runs;
    std::cerr << "wl2 watch: running; polling for changes\n";
    auto inputs = collect_watch_inputs(command);
    const int limit = watch_limit_from_env();
    while (limit <= 0 || runs < limit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto next = collect_watch_inputs(command);
        auto nativeChanges = changed_paths(inputs.native, next.native);
        if (!nativeChanges.empty()) {
            for (const auto& path : nativeChanges) {
                std::cerr << "wl2 watch: rebuild-needed " << path.string() << '\n';
            }
            inputs = std::move(next);
            continue;
        }
        auto rerunChanges = changed_paths(inputs.rerun, next.rerun);
        if (rerunChanges.empty()) {
            continue;
        }
        for (const auto& path : rerunChanges) {
            std::cerr << "wl2 watch: changed " << path.string() << '\n';
        }
        inputs = std::move(next);
        last = run_script(options, command);
        ++runs;
    }
    return last;
}

wl2::Result<wl2::ResourceStore> make_resource_store(
    const std::vector<wl2::ResourceDirectoryMount>& maps,
    bool trace = false) {
    wl2::ResourceStore store;
    store.setTraceLookups(trace);
    for (const auto& map : maps) {
        auto mounted = store.mountDirectory(
            map.root,
            map.prefix,
            map.excludePatterns,
            map.compressedFiles,
            map.compressedDirectories);
        if (!mounted) {
            return mounted.error();
        }
    }
    return store;
}

std::optional<ResourceCommand> parse_resource_command(int argc, char** argv, int start) {
    ResourceCommand command;
    if (start >= argc) {
        usage();
        return std::nullopt;
    }
    command.action = argv[start++];
    std::vector<std::string> positionals;
    for (int i = start; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--raw") {
            command.raw = true;
            continue;
        }
        if (arg == "--map-resource") {
            if (++i >= argc || !append_resource_map(command.resourceMaps, argv[i])) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return std::nullopt;
            }
            command.manifestPath = argv[i];
            continue;
        }
        constexpr std::string_view manifestPrefix = "--manifest=";
        if (arg.rfind(manifestPrefix, 0) == 0) {
            command.manifestPath = arg.substr(manifestPrefix.size());
            continue;
        }
        constexpr std::string_view mapPrefix = "--map-resource=";
        if (arg.rfind(mapPrefix, 0) == 0) {
            if (!append_resource_map(command.resourceMaps, arg.substr(mapPrefix.size()))) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--out") {
            if (++i >= argc) {
                std::cerr << "--out requires a directory\n";
                return std::nullopt;
            }
            command.outDir = argv[i];
            continue;
        }
        if (arg.rfind("-", 0) == 0) {
            std::cerr << "unknown resources option: " << arg << '\n';
            return std::nullopt;
        }
        positionals.push_back(std::move(arg));
    }
    if (!positionals.empty()) {
        std::error_code ec;
        if (positionals[0].rfind("wl2:/", 0) != 0 && std::filesystem::is_regular_file(positionals[0], ec)) {
            command.executablePath = positionals[0];
            if (positionals.size() > 1) {
                command.path = positionals[1];
            }
        } else {
            command.path = positionals[0];
        }
    }
    if (!command.manifestPath.empty()) {
        wl2::ResourceManifest manifest;
        if (!apply_manifest(command.manifestPath, command.resourceMaps, nullptr, &manifest)) {
            return std::nullopt;
        }
        if (command.path == "wl2:/") {
            command.path = manifest.prefix;
        }
    }
    return command;
}

std::vector<std::byte> handle_bytes(const wl2::ResourceHandle& handle) {
    return std::vector<std::byte>(handle.bytes().begin(), handle.bytes().end());
}

struct ExecutableResourceRecord {
    wl2::ResourceEntry entry;
    std::vector<std::byte> storedBytes;
};

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<std::vector<std::byte>> decode_hex_bytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = hex_value(hex[i]);
        int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return bytes;
}

std::vector<std::byte> decompress_resource_rle(std::span<const std::byte> input, size_t expectedSize) {
    std::vector<std::byte> out;
    // expectedSize comes from the resource table, which is attacker-controlled
    // when inspecting an untrusted executable. The real output is bounded by the
    // input (each byte pair emits at most 255 bytes), so cap the reservation to
    // avoid a huge-allocation throw on a crafted originalSize.
    out.reserve(std::min<size_t>(expectedSize, input.size() * 256));
    for (size_t i = 0; i + 1 < input.size(); i += 2) {
        const auto count = static_cast<unsigned char>(input[i]);
        const auto value = input[i + 1];
        out.insert(out.end(), count, value);
    }
    return out;
}

std::vector<std::string> split_tab_line(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        auto tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

// Parse an unsigned size field from a resource table without throwing on
// malformed input. The fields come straight from the inspected executable, so a
// crafted binary must produce a clean diagnostic rather than an uncaught
// std::stoull exception.
std::optional<size_t> parse_table_size(const std::string& field) {
    if (field.empty() || field.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    try {
        return static_cast<size_t>(std::stoull(field));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Join an output directory with a resource's logical relative path, rejecting
// anything that would escape the output directory. Resource names embedded in an
// executable's table are attacker-controlled when inspecting an untrusted
// binary, so extraction must never write outside --out.
std::optional<std::filesystem::path> resolve_extract_path(
    const std::filesystem::path& outDir, const std::string& relative) {
    namespace fs = std::filesystem;
    if (relative.empty() || relative.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    fs::path rel(relative);
    if (rel.is_absolute()) {
        return std::nullopt;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    std::error_code ec;
    fs::path base = fs::weakly_canonical(outDir, ec);
    if (ec) {
        base = outDir.lexically_normal();
    }
    fs::path target = (base / rel).lexically_normal();
    fs::path relativeToBase = target.lexically_relative(base);
    if (relativeToBase.empty() || *relativeToBase.begin() == "..") {
        return std::nullopt;
    }
    return target;
}

wl2::Result<std::vector<ExecutableResourceRecord>> read_executable_resource_table(
    const std::filesystem::path& executable) {
    auto bytes = read_text_file(executable);
    if (!bytes) {
        return wl2::Error("resource_executable_not_found", "Unable to read executable: " + executable.string());
    }
    constexpr std::string_view begin = "WL2_RESOURCE_TABLE_V1\n";
    constexpr std::string_view end = "WL2_RESOURCE_TABLE_END\n";
    auto start = bytes->find(begin);
    if (start == std::string::npos) {
        return wl2::Error("resource_table_not_found", "Executable does not contain a wl2 resource table: " + executable.string());
    }
    start += begin.size();
    auto finish = bytes->find(end, start);
    if (finish == std::string::npos) {
        return wl2::Error("resource_table_invalid", "Executable resource table is missing its end marker: " + executable.string());
    }
    std::istringstream lines(bytes->substr(start, finish - start));
    std::string line;
    std::vector<ExecutableResourceRecord> records;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = split_tab_line(line);
        if (fields.size() != 6) {
            return wl2::Error("resource_table_invalid", "Executable resource table has a malformed record");
        }
        auto storedBytes = decode_hex_bytes(fields[5]);
        if (!storedBytes) {
            return wl2::Error("resource_table_invalid", "Executable resource table has invalid stored bytes");
        }
        auto originalSize = parse_table_size(fields[2]);
        auto storedSize = parse_table_size(fields[3]);
        if (!originalSize || !storedSize) {
            return wl2::Error("resource_table_invalid", "Executable resource table has a malformed size field");
        }
        ExecutableResourceRecord record;
        record.entry.name = fields[0];
        record.entry.compression = fields[1] == "rle" ? wl2::ResourceCompression::Rle : wl2::ResourceCompression::Stored;
        record.entry.originalSize = *originalSize;
        record.entry.storedSize = *storedSize;
        record.entry.contentHash = fields[4];
        record.storedBytes = std::move(*storedBytes);
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<std::byte> executable_resource_bytes(const ExecutableResourceRecord& record, bool raw) {
    if (raw || record.entry.compression == wl2::ResourceCompression::Stored) {
        return record.storedBytes;
    }
    return decompress_resource_rle(record.storedBytes, record.entry.originalSize);
}

const ExecutableResourceRecord* find_executable_resource(
    const std::vector<ExecutableResourceRecord>& records,
    const std::string& path) {
    for (const auto& record : records) {
        if (record.entry.name == path) {
            return &record;
        }
    }
    return nullptr;
}

int resources_command(const ResourceCommand& command) {
    if (!command.executablePath.empty()) {
        auto records = read_executable_resource_table(command.executablePath);
        if (!records) {
            print_error(records.error(), StackTraceMode::Auto);
            return 1;
        }
        if (command.action == "list") {
            for (const auto& record : records.value()) {
                std::cout << record.entry.name << "\t" << record.entry.originalSize
                          << "\t" << (record.entry.compression == wl2::ResourceCompression::Rle ? "rle" : "stored")
                          << "\t" << record.entry.contentHash << '\n';
            }
            return 0;
        }
        if (command.action == "read") {
            auto* record = find_executable_resource(records.value(), command.path);
            if (!record) {
                std::cerr << "resource not found in executable: " << command.path << '\n';
                return 1;
            }
            auto bytes = executable_resource_bytes(*record, command.raw);
            for (auto byte : bytes) {
                std::cout.put(std::to_integer<char>(byte));
            }
            return 0;
        }
        if (command.action == "extract") {
            if (command.outDir.empty()) {
                std::cerr << "resources extract requires --out <dir>\n";
                return 2;
            }
            for (const auto& record : records.value()) {
                auto relative = record.entry.name;
                if (relative.rfind("wl2:/", 0) == 0) {
                    relative = relative.substr(5);
                }
                auto outPath = resolve_extract_path(command.outDir, relative);
                if (!outPath) {
                    std::cerr << "refusing to extract resource outside --out: " << record.entry.name << '\n';
                    return 1;
                }
                std::filesystem::create_directories(outPath->parent_path());
                std::ofstream out(*outPath, std::ios::binary);
                auto bytes = executable_resource_bytes(record, command.raw);
                for (auto byte : bytes) {
                    out.put(std::to_integer<char>(byte));
                }
            }
            return 0;
        }
        std::cerr << "unknown resources action: " << command.action << '\n';
        return 2;
    }

    auto storeResult = make_resource_store(command.resourceMaps);
    if (!storeResult) {
        print_error(storeResult.error(), StackTraceMode::Auto);
        return 1;
    }
    auto& store = storeResult.value();
    if (command.action == "list") {
        for (const auto& entry : store.walk(command.path)) {
            std::cout << entry.path << "\t" << entry.size;
            if (!entry.sourcePath.empty()) {
                std::cout << "\t" << entry.sourcePath.string();
            }
            std::cout << '\n';
        }
        return 0;
    }
    if (command.action == "read") {
        auto opened = store.open(command.path);
        if (!opened) {
            print_error(opened.error(), StackTraceMode::Auto);
            return 1;
        }
        std::cout << opened.value().text();
        return 0;
    }
    if (command.action == "extract") {
        if (command.outDir.empty()) {
            std::cerr << "resources extract requires --out <dir>\n";
            return 2;
        }
        for (const auto& entry : store.walk(command.path)) {
            auto opened = store.open(entry.path);
            if (!opened) {
                print_error(opened.error(), StackTraceMode::Auto);
                return 1;
            }
            auto relative = entry.path;
            if (relative.rfind("wl2:/", 0) == 0) {
                relative = relative.substr(5);
            }
            auto outPath = resolve_extract_path(command.outDir, relative);
            if (!outPath) {
                std::cerr << "refusing to extract resource outside --out: " << entry.path << '\n';
                return 1;
            }
            std::filesystem::create_directories(outPath->parent_path());
            std::ofstream out(*outPath, std::ios::binary);
            for (auto byte : opened.value().bytes()) {
                out.put(std::to_integer<char>(byte));
            }
        }
        return 0;
    }
    std::cerr << "unknown resources action: " << command.action << '\n';
    return 2;
}

struct ConfigCommand {
    std::filesystem::path manifestPath;
    std::vector<wl2::ResourceDirectoryMount> maps;
    std::optional<wl2::ResourceManifest> manifest;
    std::vector<std::filesystem::path> dynamicModulePaths;
    bool json = false;
};

std::optional<ConfigCommand> parse_config_command(int argc, char** argv, int start) {
    ConfigCommand command;
    std::vector<wl2::ResourceDirectoryMount> maps;
    for (int i = start; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--json") {
            command.json = true;
            continue;
        }
        if (arg == "--map-resource") {
            if (++i >= argc || !append_resource_map(maps, argv[i])) {
                return std::nullopt;
            }
            continue;
        }
        constexpr std::string_view mapPrefix = "--map-resource=";
        if (arg.rfind(mapPrefix, 0) == 0) {
            if (!append_resource_map(maps, arg.substr(mapPrefix.size()))) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return std::nullopt;
            }
            command.manifestPath = argv[i];
            continue;
        }
        constexpr std::string_view manifestPrefix = "--manifest=";
        if (arg.rfind(manifestPrefix, 0) == 0) {
            command.manifestPath = arg.substr(manifestPrefix.size());
            continue;
        }
        if (arg == "--load-module") {
            if (++i >= argc) {
                std::cerr << "--load-module requires a library path\n";
                return std::nullopt;
            }
            command.dynamicModulePaths.emplace_back(argv[i]);
            continue;
        }
        constexpr std::string_view loadModulePrefix = "--load-module=";
        if (arg.rfind(loadModulePrefix, 0) == 0) {
            command.dynamicModulePaths.emplace_back(arg.substr(loadModulePrefix.size()));
            continue;
        }
        std::cerr << "unknown config option: " << arg << '\n';
        return std::nullopt;
    }
    command.maps = std::move(maps);
    if (!command.manifestPath.empty()) {
        wl2::ResourceManifest manifest;
        if (!apply_manifest(command.manifestPath, command.maps, nullptr, &manifest)) {
            return std::nullopt;
        }
        command.manifest = std::move(manifest);
    }
    return command;
}

std::filesystem::path config_project_root(const ConfigCommand& command) {
    return command.manifest ? command.manifest->baseDir : std::filesystem::current_path();
}

std::vector<wl2::DependencyStatus> config_dependency_status(const ConfigCommand& command) {
    if (!command.manifest) {
        return {};
    }
    const auto lockPath = command.manifest->baseDir / "wl2.lock.yml";
    const auto depsRoot = command.manifest->baseDir / ".wl2" / "deps";
    wl2::Lockfile lock;
    if (auto loaded = wl2::loadLockfile(lockPath)) {
        lock = loaded.value();
    }
    return wl2::dependencyStatus(command.manifest->moduleDependencies, lock, depsRoot);
}

void print_json_string_array(const std::vector<std::string>& values) {
    std::cout << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "\"" << json_escape(values[i]) << "\"";
    }
    std::cout << "]";
}

// Assemble a resolver request from a config/graph command's manifest roots,
// explicit module paths, and discoverable providers. Provider discovery is
// best-effort here: errors are ignored so config reporting never aborts.
wl2::ModuleResolutionRequest build_config_resolution_request(
    const std::optional<wl2::ResourceManifest>& manifest,
    const std::vector<std::filesystem::path>& explicitPaths,
    const std::filesystem::path& projectRoot) {
    wl2::ModuleResolutionRequest request;
    if (manifest) {
        for (const auto& name : manifest->requiredModules) {
            request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Required});
        }
        for (const auto& name : manifest->optionalModules) {
            request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Optional});
        }
    }
    (void)append_explicit_module_providers(request.providers, explicitPaths);
    if (manifest) {
        append_project_module_providers(request.providers, *manifest);
    }
    append_installed_module_providers(request.providers, projectRoot);
    (void)append_builtin_module_providers(request.providers);
    return request;
}

int config_json_command(const ConfigCommand& command, const wl2::ResourceStore& store) {
    const auto projectRoot = config_project_root(command);
    wl2::ModuleStore moduleStore(wl2::resolveModuleScopePaths(projectRoot));
    const auto installed = moduleStore.list();
    const auto depStatuses = config_dependency_status(command);

    // Resolve the module graph so config can report the selected provider order,
    // shadowing, and stable dependency diagnostics.
    const auto graphRequest =
        build_config_resolution_request(command.manifest, command.dynamicModulePaths, projectRoot);
    const auto graphPlan = wl2::resolveModuleGraph(graphRequest);

    std::cout << "{\n";
    std::cout << "  \"engine\":\"" << wl2_engine_name() << "\",\n";
    std::cout << "  \"manifest\":";
    if (command.manifest) {
        std::cout << "{\"path\":\"" << json_escape(command.manifest->path.string())
                  << "\",\"schema\":\"" << json_escape(command.manifest->schema)
                  << "\",\"entry\":\"" << json_escape(command.manifest->entrySpecifier())
                  << "\",\"root\":\"" << json_escape(command.manifest->resolvedRoot().string()) << "\"}";
    } else {
        std::cout << "null";
    }
    std::cout << ",\n  \"modules\":{\"require\":";
    if (command.manifest) print_json_string_array(command.manifest->requiredModules); else std::cout << "[]";
    std::cout << ",\"optional\":";
    if (command.manifest) print_json_string_array(command.manifest->optionalModules); else std::cout << "[]";
    std::cout << ",\"installed\":[";
    for (size_t i = 0; i < installed.size(); ++i) {
        const auto& record = installed[i];
        if (i) std::cout << ",";
        std::cout << "{\"name\":\"" << json_escape(record.name)
                  << "\",\"version\":\"" << json_escape(record.version)
                  << "\",\"build\":\"" << json_escape(record.build)
                  << "\",\"scope\":\"" << wl2::moduleScopeName(record.scope)
                  << "\",\"library\":\"" << json_escape(record.libraryPath.string())
                  << "\",\"shadowed\":" << (record.shadowed ? "true" : "false") << "}";
    }
    std::cout << "]},\n";
    std::cout << "  \"graph\":{\"selected\":[";
    if (graphPlan) {
        for (size_t i = 0; i < graphPlan.value().loadOrder.size(); ++i) {
            const auto& resolved = graphPlan.value().loadOrder[i];
            const auto& provider = resolved.provider;
            if (i) std::cout << ",";
            std::cout << "{\"name\":\"" << json_escape(provider.info.name)
                      << "\",\"version\":\"" << json_escape(provider.info.version)
                      << "\",\"build\":\"" << json_escape(provider.info.build)
                      << "\",\"source\":\"" << wl2::moduleProviderSourceName(provider.source)
                      << "\",\"optional\":" << (resolved.optional ? "true" : "false") << "}";
        }
    }
    std::cout << "]},\n";
    std::cout << "  \"resources\":[";
    const auto mounts = store.mounts();
    for (size_t i = 0; i < mounts.size(); ++i) {
        if (i) std::cout << ",";
        std::cout << "{\"prefix\":\"" << json_escape(mounts[i].prefix)
                  << "\",\"root\":\"" << json_escape(mounts[i].root.string()) << "\"}";
    }
    std::cout << "],\n";
    std::cout << "  \"filesystem\":{\"scriptLoading\":true,\"moduleReads\":false},\n";
    {
        // Capability policy is denied by default; the CLI does not enable host
        // capabilities, so it reports the default (effective) policy.
        const wl2::RuntimeOptions defaults;
        std::cout << "  \"capabilities\":{\"network\":{\"allow\":"
                  << (defaults.allowNetwork ? "true" : "false") << ",\"allowList\":";
        print_json_string_array(defaults.networkAllowList);
        std::cout << "},\"listen\":{\"allow\":"
                  << (defaults.allowListening ? "true" : "false") << ",\"allowList\":";
        print_json_string_array(defaults.listenAllowList);
        const bool uiAllowed = command.manifest ? command.manifest->allowUi : defaults.allowUi;
        std::cout << "},\"ui\":{\"allow\":"
                  << (uiAllowed ? "true" : "false") << "},\"graphics\":{\"allow\":"
                  << (defaults.allowGraphics ? "true" : "false")
                  << "},\"sharedMemory\":{\"allow\":"
                  << (defaults.allowSharedMemory ? "true" : "false") << ",\"allowList\":";
        print_json_string_array(defaults.sharedMemoryAllowList);
        std::cout << "}},\n";
    }
    std::cout << "  \"dependencies\":[";
    for (size_t i = 0; i < depStatuses.size(); ++i) {
        const auto& status = depStatuses[i];
        if (i) std::cout << ",";
        std::cout << "{\"name\":\"" << json_escape(status.name)
                  << "\",\"tag\":\"" << json_escape(status.tag)
                  << "\",\"lockedCommit\":\"" << json_escape(status.lockedCommit)
                  << "\",\"fetched\":" << (status.fetched ? "true" : "false") << "}";
    }
    std::cout << "],\n";
    std::cout << "  \"diagnostics\":[";
    bool firstDiagnostic = true;
    auto emitDiagnostic = [&](const std::string& text) {
        if (!firstDiagnostic) std::cout << ",";
        std::cout << "\"" << json_escape(text) << "\"";
        firstDiagnostic = false;
    };
    if (command.manifest && !command.manifest->moduleDependencies.empty()
        && !std::filesystem::is_regular_file(command.manifest->baseDir / "wl2.lock.yml")) {
        emitDiagnostic("lockfile missing; run wl2 deps lock");
    }
    if (graphPlan) {
        for (const auto& diagnostic : graphPlan.value().diagnostics) {
            emitDiagnostic(diagnostic.code + ": " + diagnostic.message);
        }
    } else {
        emitDiagnostic(graphPlan.error().code() + ": " + graphPlan.error().message());
    }
    if (firstDiagnostic) {
        std::cout << "\"ok\"";
    }
    std::cout << "]\n}\n";
    return 0;
}

int config_command(const ConfigCommand& command) {
    auto store = make_resource_store(command.maps);
    if (!store) {
        print_error(store.error(), StackTraceMode::Auto);
        return 1;
    }
    if (command.json) {
        return config_json_command(command, store.value());
    }
    std::cout << "engine: " << wl2_engine_name() << '\n';
    if (command.manifest) {
        std::cout << "manifest:\n";
        std::cout << "  path: " << command.manifest->path.string() << '\n';
        std::cout << "  schema: " << command.manifest->schema << '\n';
        std::cout << "  entry: " << command.manifest->entrySpecifier() << '\n';
        std::cout << "  root: " << command.manifest->resolvedRoot().string() << '\n';
    } else {
        std::cout << "manifest: none\n";
    }
    std::cout << "modules:\n";
    if (command.manifest) {
        if (!command.manifest->requiredModules.empty()) {
            std::cout << "  require:\n";
            for (const auto& name : command.manifest->requiredModules) {
                std::cout << "    " << name << '\n';
            }
        }
        if (!command.manifest->optionalModules.empty()) {
            std::cout << "  optional:\n";
            for (const auto& name : command.manifest->optionalModules) {
                std::cout << "    " << name << '\n';
            }
        }
    }
    std::cout << "resources:\n";
    for (const auto& mount : store.value().mounts()) {
        std::cout << "  " << mount.prefix << " -> " << mount.root.string() << '\n';
    }
    if (!command.dynamicModulePaths.empty()) {
        std::cout << "dynamic modules:\n";
        for (const auto& path : command.dynamicModulePaths) {
            auto info = wl2::ModuleLoader::inspectDynamicModule(path);
            if (!info) {
                std::cout << "  " << path.string() << " -> error: " << info.error().code() << '\n';
                continue;
            }
            std::cout << "  " << info.value().libraryPath.string()
                      << " -> " << info.value().name
                      << " (abi " << info.value().abiVersion
                      << ", version " << info.value().version << ")\n";
        }
    }

    // Installed modules across scopes, reporting any that are shadowed by a
    // higher-priority scope.
    std::filesystem::path projectRoot = config_project_root(command);
    wl2::ModuleStore moduleStore(wl2::resolveModuleScopePaths(projectRoot));
    auto installed = moduleStore.list();
    if (!installed.empty()) {
        std::cout << "  installed modules:\n";
        for (const auto& record : installed) {
            std::cout << "    " << record.name << " " << record.version
                      << " [" << wl2::moduleScopeName(record.scope) << "]";
            if (!record.build.empty()) {
                std::cout << " build=" << record.build;
            }
            if (record.shadowed) {
                std::cout << " (shadowed)";
            }
            std::cout << '\n';
        }
    }
    std::cout << "filesystem:\n";
    std::cout << "  script loading: enabled\n";
    std::cout << "  wl2:fs reads: disabled by default\n";
    {
        const wl2::RuntimeOptions defaults;
        std::cout << "capabilities:\n";
        std::cout << "  network connect: " << (defaults.allowNetwork ? "allowed" : "denied")
                  << " (default)\n";
        std::cout << "  network listen: " << (defaults.allowListening ? "allowed" : "denied")
                  << " (default)\n";
        const bool uiAllowed = command.manifest ? command.manifest->allowUi : defaults.allowUi;
        std::cout << "  ui: " << (uiAllowed ? "allowed" : "denied")
                  << (command.manifest ? " (manifest)" : " (default)") << '\n';
        std::cout << "  graphics: " << (defaults.allowGraphics ? "allowed" : "denied")
                  << " (default)\n";
        std::cout << "  shared memory: " << (defaults.allowSharedMemory ? "allowed" : "denied")
                  << " (default)\n";
    }
    std::cout << "dependencies:\n";
    auto depStatuses = config_dependency_status(command);
    if (depStatuses.empty()) {
        std::cout << "  none\n";
    } else {
        for (const auto& status : depStatuses) {
            std::cout << "  " << status.name
                      << " tag=" << (status.tag.empty() ? "-" : status.tag)
                      << " locked=" << (status.lockedCommit.empty() ? "-" : status.lockedCommit.substr(0, 12))
                      << " fetched=" << (status.fetched ? "yes" : "no") << '\n';
        }
    }
    std::cout << "diagnostics:\n";
    if (command.manifest && !command.manifest->moduleDependencies.empty()
        && !std::filesystem::is_regular_file(command.manifest->baseDir / "wl2.lock.yml")) {
        std::cout << "  lockfile missing; run wl2 deps lock\n";
    } else {
        std::cout << "  ok\n";
    }
    return 0;
}

std::optional<TestCommand> parse_test_command(int argc, char** argv, int start) {
    TestCommand command;
    for (int i = start; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return std::nullopt;
            }
            command.manifestPath = argv[i];
            continue;
        }
        if (arg.rfind("--manifest=", 0) == 0) {
            command.manifestPath = arg.substr(std::string_view("--manifest=").size());
            continue;
        }
        if (arg == "--filter") {
            if (++i >= argc) {
                std::cerr << "--filter requires a value\n";
                return std::nullopt;
            }
            command.filter = argv[i];
            continue;
        }
        if (arg.rfind("--filter=", 0) == 0) {
            command.filter = arg.substr(std::string_view("--filter=").size());
            continue;
        }
        if (arg == "--json") {
            command.json = true;
            continue;
        }
        std::cerr << "unknown test option: " << arg << '\n';
        return std::nullopt;
    }
    return command;
}

std::regex glob_regex(const std::string& pattern) {
    std::string text = "^";
    for (char ch : pattern) {
        switch (ch) {
            case '*': text += ".*"; break;
            case '?': text += "."; break;
            case '.': text += "\\."; break;
            case '\\': text += "\\\\"; break;
            default:
                if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '/') {
                    text.push_back(ch);
                } else {
                    text.push_back('\\');
                    text.push_back(ch);
                }
        }
    }
    text += "$";
    return std::regex(text);
}

std::vector<std::filesystem::path> discover_test_files(const wl2::ResourceManifest& manifest) {
    std::vector<std::filesystem::path> files;
    auto roots = manifest.testRoots;
    if (roots.empty()) {
        roots.push_back("tests");
    }
    auto pattern = glob_regex(manifest.testPattern.empty() ? "*.test.js" : manifest.testPattern);
    for (const auto& rootText : roots) {
        const auto root = manifest.baseDir / rootText;
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto rel = entry.path().lexically_relative(root).generic_string();
            if (std::regex_match(rel, pattern) || std::regex_match(entry.path().filename().generic_string(), pattern)) {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string build_test_harness(
    const std::vector<std::filesystem::path>& files,
    const std::string& filter,
    bool json) {
    std::ostringstream js;
    js << "const __wl2_tests = [];\n";
    js << "const __wl2_filter = \"" << js_string_escape(filter) << "\";\n";
    js << "const __wl2_json = " << (json ? "true" : "false") << ";\n";
    js << "globalThis.assert = function(condition, message) {\n";
    js << "  if (!condition) throw new Error(message || 'assertion failed');\n";
    js << "};\n";
    js << "globalThis.test = function(name, fn) {\n";
    js << "  if (typeof name !== 'string' || typeof fn !== 'function') throw new Error('test(name, fn) requires a name and function');\n";
    js << "  if (__wl2_filter && name.indexOf(__wl2_filter) < 0) return;\n";
    js << "  __wl2_tests.push({ name, fn });\n";
    js << "};\n";
    for (const auto& file : files) {
        auto source = read_text_file(file);
        js << "\n// wl2 test file: " << js_string_escape(file.string()) << "\n";
        if (source) {
            js << *source << "\n";
        }
    }
    js << "const __wl2_results = [];\n";
    js << "let __wl2_failed = 0;\n";
    js << "for (const __wl2_test of __wl2_tests) {\n";
    js << "  try {\n";
    js << "    await __wl2_test.fn();\n";
    js << "    __wl2_results.push({ name: __wl2_test.name, status: 'passed' });\n";
    js << "    if (!__wl2_json) console.log('ok ' + __wl2_test.name);\n";
    js << "  } catch (error) {\n";
    js << "    __wl2_failed++;\n";
    js << "    const message = error && error.message ? String(error.message) : String(error);\n";
    js << "    __wl2_results.push({ name: __wl2_test.name, status: 'failed', message });\n";
    js << "    if (!__wl2_json) { console.log('not ok ' + __wl2_test.name + ': ' + message); if (error && error.stack) console.log(error.stack); }\n";
    js << "  }\n";
    js << "}\n";
    js << "const __wl2_summary = { total: __wl2_results.length, passed: __wl2_results.length - __wl2_failed, failed: __wl2_failed, tests: __wl2_results };\n";
    js << "if (__wl2_json) console.log(JSON.stringify(__wl2_summary)); else console.log('tests ' + __wl2_summary.passed + '/' + __wl2_summary.total + ' passed');\n";
    js << "if (__wl2_failed) throw new Error(String(__wl2_failed) + ' test(s) failed');\n";
    return js.str();
}

int test_command(wl2::RuntimeOptions options, const TestCommand& command) {
    auto manifest = wl2::loadResourceManifest(command.manifestPath);
    if (!manifest) {
        print_error(manifest.error(), StackTraceMode::Auto);
        return 1;
    }
    const auto files = discover_test_files(manifest.value());
    if (files.empty()) {
        std::cerr << "no test files matched " << manifest.value().testPattern << '\n';
        return 1;
    }

    std::vector<wl2::ResourceDirectoryMount> maps;
    std::string script;
    wl2::ResourceManifest applied;
    if (!apply_manifest(command.manifestPath, maps, &script, &applied)) {
        return 1;
    }
    options.resourceDirectoryMounts = std::move(maps);
    options.requiredModules = applied.requiredModules;
    options.optionalModules = applied.optionalModules;
    wl2::ModuleStore store(wl2::resolveModuleScopePaths(applied.baseDir));
    auto resolveName = [&](const std::string& name) {
        if (auto record = store.resolve(name)) {
            options.dynamicModules.push_back(wl2::DynamicModuleSpec{record->libraryPath, true});
        }
    };
    for (const auto& name : options.requiredModules) {
        resolveName(name);
    }
    for (const auto& name : options.optionalModules) {
        resolveName(name);
    }

    const auto harnessPath = std::filesystem::temp_directory_path()
        / ("wl2-test-harness-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".js");
    if (!write_text_file(harnessPath, build_test_harness(files, command.filter, command.json))) {
        return 1;
    }
    wl2::Runtime runtime(std::move(options));
    auto result = runtime.runModule(harnessPath.string());
    std::error_code ec;
    std::filesystem::remove(harnessPath, ec);
    if (!result) {
        print_error(result.error(), StackTraceMode::Auto);
        return 1;
    }
    return result.value();
}

int init_command(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "wl2 init requires a <name>\n";
        return 2;
    }
    const std::string name = argv[2];
    const std::string target = sanitize_identifier(name);
    const auto root = std::filesystem::path(name);
    std::error_code ec;
    if (std::filesystem::exists(root, ec) && !std::filesystem::is_empty(root, ec)) {
        std::cerr << "target directory is not empty: " << root.string() << '\n';
        return 1;
    }
    std::filesystem::create_directories(root / "files", ec);
    std::filesystem::create_directories(root / "tests", ec);

    if (!write_text_file(root / "wl2.yml",
            "schema: wl2.project.v1\n"
            "prefix: wl2:/app\n"
            "root: files\n"
            "entry: main.js\n"
            "resources:\n"
            "  store:\n"
            "    files:\n"
            "      - main.js\n"
            "tests:\n"
            "  roots:\n"
            "    - tests\n"
            "  pattern: \"*.test.js\"\n")
        || !write_text_file(root / "files" / "main.js",
            "console.log(\"hello from " + target + "\");\n")
        || !write_text_file(root / "tests" / "main.test.js",
            "test(\"main sync\", () => {\n"
            "  assert(1 + 1 === 2, \"math still works\");\n"
            "});\n"
            "\n"
            "test(\"main async\", async () => {\n"
            "  const value = await Promise.resolve(\"ok\");\n"
            "  assert(value === \"ok\", \"promise resolved\");\n"
            "});\n")
        || !write_text_file(root / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(" + target + " LANGUAGES CXX)\n"
            "find_package(winglib2 CONFIG REQUIRED)\n"
            "enable_testing()\n"
            "wl2_add_javascript_executable(" + target + "\n"
            "    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/wl2.yml)\n"
            "add_test(NAME " + target + ".run COMMAND " + target + ")\n")
        || !write_text_file(root / "README.md",
            "# " + name + "\n\n"
            "Run during development:\n\n"
            "```sh\n"
            "wl2 run --manifest wl2.yml\n"
            "wl2 test --manifest wl2.yml\n"
            "```\n")
        || !write_text_file(root / ".gitignore",
            "build/\n"
            ".wl2/\n")) {
        return 1;
    }
    std::cout << "created app " << root.string() << '\n';
    return 0;
}

int module_new_command(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "wl2 module new requires a <name>\n";
        return 2;
    }
    const std::string rawName = argv[3];
    std::string slug = sanitize_identifier(rawName);
    if (slug.rfind("wl2_", 0) != 0) {
        slug = "wl2_" + slug;
    }
    const std::string moduleSpecifier = "wl2:" + slug.substr(4);
    const auto root = std::filesystem::path(slug);
    std::error_code ec;
    if (std::filesystem::exists(root, ec) && !std::filesystem::is_empty(root, ec)) {
        std::cerr << "target directory is not empty: " << root.string() << '\n';
        return 1;
    }
    std::filesystem::create_directories(root / "include" / slug, ec);
    std::filesystem::create_directories(root / "src", ec);
    std::filesystem::create_directories(root / "test", ec);

    const std::string headerGuard = slug + "_h";
    const std::string registerName = slug + "_register_module";
    const std::string header =
        "#pragma once\n\n"
        "#include \"wl2/module.h\"\n"
        "#include \"wl2/runtime.h\"\n\n"
        "wl2::ModuleInfo " + registerName + "(wl2::Runtime& runtime);\n\n"
        "extern \"C\" int wl2_module_get_info(wl2_module_info* out);\n"
        "extern \"C\" int wl2_module_register(const wl2_module_host* host);\n";

    const std::string source =
        "#include \"" + slug + "/" + slug + ".h\"\n\n"
        "#ifndef WL2_VERSION\n"
        "#define WL2_VERSION \"0.0.0\"\n"
        "#endif\n"
        "#ifndef WL2_BUILD\n"
        "#define WL2_BUILD \"0\"\n"
        "#endif\n\n"
        "namespace {\n"
        "constexpr const char* kSummary = \"Generated wl2 module scaffold.\";\n"
        "constexpr const char* kApi = \"No JavaScript exports yet.\";\n"
        "}\n\n"
        "#ifdef WL2_MODULE_STATIC\n"
        "wl2::ModuleInfo " + registerName + "(wl2::Runtime& runtime) {\n"
        "    (void)runtime;\n"
        "    return wl2::ModuleInfo{\n"
        "        .abiVersion = wl2::ModuleAbiVersion,\n"
        "        .name = \"" + moduleSpecifier + "\",\n"
        "        .version = WL2_VERSION,\n"
        "        .build = WL2_BUILD,\n"
        "        .stableId = \"" + slug + "-generated\",\n"
        "        .summary = kSummary,\n"
        "        .api = kApi,\n"
        "        .unloadSafe = true,\n"
        "    };\n"
        "}\n"
        "#else\n"
        "extern \"C\" int wl2_module_get_info(wl2_module_info* out) {\n"
        "    if (!out) return 1;\n"
        "    out->abi_version = wl2::ModuleAbiVersion;\n"
        "    out->name = \"" + moduleSpecifier + "\";\n"
        "    out->version = WL2_VERSION;\n"
        "    out->build = WL2_BUILD;\n"
        "    out->stable_id = \"" + slug + "-generated\";\n"
        "    out->summary = kSummary;\n"
        "    out->api = kApi;\n"
        "    out->unload_safe = 1;\n"
        "    out->required_wl2_version = WL2_VERSION;\n"
        "    return 0;\n"
        "}\n\n"
        "extern \"C\" int wl2_module_register(const wl2_module_host* host) {\n"
        "    (void)host;\n"
        "    return 0;\n"
        "}\n"
        "#endif\n";

    if (!write_text_file(root / "include" / slug / (slug + ".h"), header)
        || !write_text_file(root / "src" / (slug + ".cpp"), source)
        || !write_text_file(root / "test" / (slug + "_tests.cpp"),
            "#include \"" + slug + "/" + slug + ".h\"\n"
            "#include <iostream>\n\n"
            "int main() {\n"
            "    wl2::Runtime runtime;\n"
            "    auto info = " + registerName + "(runtime);\n"
            "    if (info.name != \"" + moduleSpecifier + "\") return 1;\n"
            "    std::cout << \"" + slug + " ok\\n\";\n"
            "    return 0;\n"
            "}\n")
        || !write_text_file(root / "CMakeLists.txt",
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(" + slug + " VERSION 0.1.0 LANGUAGES CXX)\n"
            "find_package(winglib2 CONFIG REQUIRED)\n"
            "enable_testing()\n"
            "set(WL2_BUILD_STATIC_MODULES ON)\n"
            "set(WL2_BUILD_SHARED_MODULES ON)\n"
            "wl2_add_module(" + slug + "\n"
            "    NO_INSTALL\n"
            "    NO_DYNAMIC_LINK_LIBRARIES\n"
            "    SOURCES src/" + slug + ".cpp\n"
            "    PUBLIC_LINK_LIBRARIES winglib2::wl2_core\n"
            "    DYNAMIC_INCLUDE_DIRS \"$<TARGET_PROPERTY:winglib2::wl2_core,INTERFACE_INCLUDE_DIRECTORIES>\"\n"
            "    STATIC_PRIVATE_COMPILE_DEFINITIONS WL2_MODULE_STATIC=1\n"
            "    DYNAMIC_PRIVATE_COMPILE_DEFINITIONS WL2_VERSION=\"${PROJECT_VERSION}\")\n"
            "add_executable(" + slug + "_tests test/" + slug + "_tests.cpp)\n"
            "target_link_libraries(" + slug + "_tests PRIVATE winglib2::wl2_core " + slug + "_static)\n"
            "add_test(NAME " + slug + ".tests COMMAND " + slug + "_tests)\n")
        || !write_text_file(root / "README.md",
            "# " + slug + "\n\n"
            "Generated native module scaffold for `" + moduleSpecifier + "`.\n\n"
            "```sh\n"
            "cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/winglib2/install\n"
            "cmake --build build\n"
            "ctest --test-dir build --output-on-failure\n"
            "```\n")
        || !write_text_file(root / ".gitignore", "build/\n")) {
        return 1;
    }
    std::cout << "created module " << root.string() << '\n';
    return 0;
}

void print_module_info(const wl2::ModuleInfo& info) {
    std::cout << "module: " << info.name << '\n';
    std::cout << "version: " << info.version << '\n';
    if (!info.build.empty()) {
        std::cout << "build: " << info.build << '\n';
    }
    std::cout << "abi: " << info.abiVersion << '\n';
    if (!info.stableId.empty()) {
        std::cout << "stableId: " << info.stableId << '\n';
    }
    if (!info.requiredWL2Version.empty()) {
        std::cout << "requires wl2: " << info.requiredWL2Version << '\n';
    }
    if (!info.libraryPath.empty()) {
        std::cout << "library: " << info.libraryPath.string() << '\n';
    }
    if (!info.dependencies.empty()) {
        std::cout << "dependencies:\n";
        for (const auto& dep : info.dependencies) {
            std::cout << "  " << dep.name
                      << (dep.kind == wl2::ModuleDependencyKind::Required ? " (required)" : " (optional)");
            if (!dep.versionRange.empty()) {
                std::cout << " " << dep.versionRange;
            }
            if (!dep.stableId.empty()) {
                std::cout << " stableId=" << dep.stableId;
            }
            std::cout << '\n';
        }
    }
}

// Module names referenced by the project's manifest and lockfile, used to guard
// uninstall. Best-effort: missing files are ignored.
std::set<std::string> referenced_module_names() {
    std::set<std::string> names;
    if (auto manifest = wl2::loadResourceManifest("wl2.yml")) {
        for (const auto& name : manifest.value().requiredModules) names.insert(name);
        for (const auto& name : manifest.value().optionalModules) names.insert(name);
        for (const auto& dep : manifest.value().moduleDependencies) names.insert(dep.name);
    }
    if (auto lock = wl2::loadLockfile("wl2.lock.yml")) {
        for (const auto& module : lock.value().modules) names.insert(module.name);
    }
    return names;
}

int module_validate(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "wl2 module validate requires a <library-path>\n";
        return 2;
    }
    auto info = wl2::ModuleLoader::inspectDynamicModule(argv[3]);
    if (!info) {
        print_error(info.error(), StackTraceMode::Auto);
        return 1;
    }
    print_module_info(info.value());
    std::cout << '\n' << info.value().summary << '\n';
    return 0;
}

int module_install(int argc, char** argv) {
    std::filesystem::path source;
    std::optional<wl2::ModuleScope> scope;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires local, user, or system\n";
                return 2;
            }
            scope = wl2::parseModuleScope(argv[i]);
            if (!scope) {
                std::cerr << "invalid scope: " << argv[i] << '\n';
                return 2;
            }
        } else if (arg.rfind("-", 0) == 0) {
            std::cerr << "unknown install option: " << arg << '\n';
            return 2;
        } else {
            source = arg;
        }
    }
    if (source.empty()) {
        std::cerr << "wl2 module install requires a <library-path>\n";
        return 2;
    }
    wl2::ModuleStore store(wl2::resolveModuleScopePaths());
    auto installed = store.install(source, scope.value_or(wl2::ModuleScope::Local));
    if (!installed) {
        print_error(installed.error(), StackTraceMode::Auto);
        return 1;
    }
    std::cout << "installed " << installed.value().name << " " << installed.value().version;
    if (!installed.value().build.empty()) {
        std::cout << " build=" << installed.value().build;
    }
    std::cout << " (scope " << wl2::moduleScopeName(installed.value().scope) << ")\n";
    std::cout << "  " << installed.value().libraryPath.string() << '\n';

    // Warn when a higher-priority scope already provides this name: the newly
    // installed copy is shadowed and will not be selected by the resolver.
    for (const auto& record : store.list()) {
        if (record.name == installed.value().name
            && record.scope == installed.value().scope
            && record.shadowed) {
            std::cerr << "warning: " << record.name
                      << " is shadowed by a higher-priority provider and will not be selected\n";
            break;
        }
    }
    return 0;
}

int module_uninstall(int argc, char** argv) {
    std::string name;
    std::optional<wl2::ModuleScope> scope;
    bool force = false;
    bool purgeCache = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires local, user, or system\n";
                return 2;
            }
            scope = wl2::parseModuleScope(argv[i]);
            if (!scope) {
                std::cerr << "invalid scope: " << argv[i] << '\n';
                return 2;
            }
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--purge-cache") {
            purgeCache = true;
        } else if (arg.rfind("-", 0) == 0) {
            std::cerr << "unknown uninstall option: " << arg << '\n';
            return 2;
        } else {
            name = arg;
        }
    }
    if (name.empty()) {
        std::cerr << "wl2 module uninstall requires a <name>\n";
        return 2;
    }
    wl2::ModuleStore store(wl2::resolveModuleScopePaths());
    auto rc = store.uninstall(name, scope, force, purgeCache, referenced_module_names());
    if (!rc) {
        print_error(rc.error(), StackTraceMode::Auto);
        return 1;
    }
    std::cout << "uninstalled " << name << '\n';
    return 0;
}

int module_list(int argc, char** argv) {
    std::optional<wl2::ModuleScope> scope;
    bool allScopes = true;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires all, local, user, or system\n";
                return 2;
            }
            std::string value(argv[i]);
            if (value == "all") {
                scope.reset();
                allScopes = true;
            } else {
                scope = wl2::parseModuleScope(value);
                if (!scope) {
                    std::cerr << "invalid scope: " << value << '\n';
                    return 2;
                }
                allScopes = false;
            }
            continue;
        }
        std::cerr << "unknown list option: " << arg << '\n';
        return 2;
    }

    wl2::ModuleStore store(wl2::resolveModuleScopePaths());
    auto modules = allScopes ? store.list() : store.scanScope(*scope);
    if (modules.empty()) {
        std::cout << "no installed modules\n";
        return 0;
    }
    for (const auto& record : modules) {
        std::cout << record.name << "  " << record.version
                  << "  [" << wl2::moduleScopeName(record.scope) << "]";
        if (!record.build.empty()) {
            std::cout << "  build=" << record.build;
        }
        if (record.shadowed) {
            std::cout << " (shadowed)";
        }
        std::cout << '\n';
    }
    return 0;
}

wl2::ModuleProvider::Source provider_source_for_scope(wl2::ModuleScope scope) {
    switch (scope) {
        case wl2::ModuleScope::Local: return wl2::ModuleProvider::Source::Local;
        case wl2::ModuleScope::User: return wl2::ModuleProvider::Source::User;
        case wl2::ModuleScope::System: return wl2::ModuleProvider::Source::System;
    }
    return wl2::ModuleProvider::Source::Local;
}

void append_installed_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const std::filesystem::path& projectRoot) {
    wl2::ModuleStore store(wl2::resolveModuleScopePaths(projectRoot));
    for (const auto& record : store.list()) {
        wl2::ModuleInfo info;
        info.name = record.name;
        info.version = record.version;
        info.build = record.build;
        info.abiVersion = record.abiVersion;
        info.stableId = record.stableId;
        info.libraryPath = record.libraryPath;
        info.dependencies = record.dependencies;
        providers.push_back(wl2::ModuleProvider{
            .info = std::move(info),
            .source = provider_source_for_scope(record.scope),
            .path = record.libraryPath,
        });
    }
}

wl2::Result<void> append_builtin_module_providers(std::vector<wl2::ModuleProvider>& providers) {
    wl2::RuntimeOptions options;
    wl2_register_builtin_static_modules(options);
    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return init.error();
    }
    for (const auto& module : runtime.modules().modules()) {
        providers.push_back(wl2::ModuleProvider{
            .info = module,
            .source = wl2::ModuleProvider::Source::Builtin,
        });
    }
    return {};
}

wl2::Result<void> append_explicit_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const std::vector<std::filesystem::path>& paths) {
    for (const auto& path : paths) {
        auto info = wl2::ModuleLoader::inspectDynamicModule(path);
        if (!info) {
            return info.error();
        }
        providers.push_back(wl2::ModuleProvider{
            .info = std::move(info.value()),
            .source = wl2::ModuleProvider::Source::Explicit,
            .path = path,
        });
    }
    return {};
}

void append_project_module_providers(
    std::vector<wl2::ModuleProvider>& providers,
    const wl2::ResourceManifest& manifest) {
    const auto depsRoot = manifest.baseDir / ".wl2" / "deps";
    for (const auto& dep : manifest.moduleDependencies) {
        const auto sourceRoot = depsRoot / dep.name / dep.path;
        const auto metadataPath = sourceRoot / "wl2.module.source.yml";
        std::error_code ec;
        if (!std::filesystem::is_regular_file(metadataPath, ec)) {
            continue;
        }
        auto metadata = wl2::loadModuleSourceMetadata(metadataPath);
        if (!metadata) {
            std::cerr << "warning: " << metadata.error().message() << '\n';
            continue;
        }
        wl2::ModuleInfo info;
        info.name = metadata.value().provides;
        info.version = metadata.value().version;
        info.stableId = metadata.value().stableId;
        info.dependencies = metadata.value().dependencies;
        providers.push_back(wl2::ModuleProvider{
            .info = std::move(info),
            .source = wl2::ModuleProvider::Source::Project,
            .path = metadataPath,
        });
    }
}

void print_module_graph_json(
    const wl2::ModuleResolutionRequest& request,
    const wl2::ModuleResolutionPlan& plan) {
    std::cout << "{\n";
    std::cout << "  \"roots\":{\"required\":[";
    bool first = true;
    for (const auto& root : request.roots) {
        if (root.kind != wl2::ModuleDependencyKind::Required) continue;
        if (!first) std::cout << ",";
        first = false;
        std::cout << "\"" << json_escape(root.name) << "\"";
    }
    std::cout << "],\"optional\":[";
    first = true;
    for (const auto& root : request.roots) {
        if (root.kind != wl2::ModuleDependencyKind::Optional) continue;
        if (!first) std::cout << ",";
        first = false;
        std::cout << "\"" << json_escape(root.name) << "\"";
    }
    std::cout << "]},\n";
    std::cout << "  \"modules\":[";
    for (size_t i = 0; i < plan.loadOrder.size(); ++i) {
        const auto& resolved = plan.loadOrder[i];
        const auto& provider = resolved.provider;
        if (i) std::cout << ",";
        std::cout << "{\"name\":\"" << json_escape(provider.info.name)
                  << "\",\"version\":\"" << json_escape(provider.info.version)
                  << "\",\"build\":\"" << json_escape(provider.info.build)
                  << "\",\"source\":\"" << wl2::moduleProviderSourceName(provider.source)
                  << "\",\"path\":\"" << json_escape(provider.path.string())
                  << "\",\"optional\":" << (resolved.optional ? "true" : "false") << "}";
    }
    std::cout << "],\n";
    std::cout << "  \"diagnostics\":[";
    for (size_t i = 0; i < plan.diagnostics.size(); ++i) {
        const auto& diagnostic = plan.diagnostics[i];
        if (i) std::cout << ",";
        std::cout << "{\"code\":\"" << json_escape(diagnostic.code)
                  << "\",\"message\":\"" << json_escape(diagnostic.message)
                  << "\",\"chain\":";
        print_json_string_array(diagnostic.chain);
        std::cout << "}";
    }
    std::cout << "]\n}\n";
}

void print_module_graph_text(const wl2::ModuleResolutionPlan& plan) {
    if (plan.loadOrder.empty()) {
        std::cout << "no module requirements\n";
    } else {
        std::cout << "module load order:\n";
        for (const auto& resolved : plan.loadOrder) {
            const auto& provider = resolved.provider;
            std::cout << "  " << provider.info.name;
            if (!provider.info.version.empty()) {
                std::cout << " " << provider.info.version;
            }
            if (!provider.info.build.empty()) {
                std::cout << " build=" << provider.info.build;
            }
            std::cout << " [" << wl2::moduleProviderSourceName(provider.source) << "]";
            if (resolved.optional) {
                std::cout << " optional";
            }
            if (!provider.path.empty()) {
                std::cout << " " << provider.path.string();
            }
            std::cout << '\n';
        }
    }
    for (const auto& diagnostic : plan.diagnostics) {
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}

int module_graph(int argc, char** argv) {
    std::filesystem::path manifestPath = "wl2.yml";
    std::vector<std::filesystem::path> explicitModules;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return 2;
            }
            manifestPath = argv[i];
        } else if (arg == "--load-module") {
            if (++i >= argc) {
                std::cerr << "--load-module requires a path\n";
                return 2;
            }
            explicitModules.emplace_back(argv[i]);
        } else if (arg == "--json") {
            json = true;
        } else {
            std::cerr << "unknown graph option: " << arg << '\n';
            return 2;
        }
    }

    auto manifest = wl2::loadResourceManifest(manifestPath);
    if (!manifest) {
        print_error(manifest.error(), StackTraceMode::Auto);
        return 1;
    }

    wl2::ModuleResolutionRequest request;
    for (const auto& name : manifest.value().requiredModules) {
        request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Required});
    }
    for (const auto& name : manifest.value().optionalModules) {
        request.roots.push_back({.name = name, .kind = wl2::ModuleDependencyKind::Optional});
    }

    if (auto ok = append_explicit_module_providers(request.providers, explicitModules); !ok) {
        print_error(ok.error(), StackTraceMode::Auto);
        return 1;
    }
    append_project_module_providers(request.providers, manifest.value());
    append_installed_module_providers(request.providers, manifest.value().baseDir);
    if (auto ok = append_builtin_module_providers(request.providers); !ok) {
        print_error(ok.error(), StackTraceMode::Auto);
        return 1;
    }

    auto plan = wl2::resolveModuleGraph(request);
    if (!plan) {
        print_error(plan.error(), StackTraceMode::Auto);
        return 1;
    }
    if (json) {
        print_module_graph_json(request, plan.value());
    } else {
        print_module_graph_text(plan.value());
    }
    return 0;
}

int module_command(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "wl2 module requires an action: validate, install, uninstall, list, graph, new\n";
        return 2;
    }
    std::string action(argv[2]);
    if (action == "new") {
        return module_new_command(argc, argv);
    }
    if (action == "validate") {
        return module_validate(argc, argv);
    }
    if (action == "install") {
        return module_install(argc, argv);
    }
    if (action == "uninstall") {
        return module_uninstall(argc, argv);
    }
    if (action == "list") {
        return module_list(argc, argv);
    }
    if (action == "graph") {
        return module_graph(argc, argv);
    }
    std::cerr << "unknown module action: " << action << '\n';
    return 2;
}

std::filesystem::path app_cache_root(wl2::ModuleScope scope) {
    return wl2::resolveAppScopePaths().forScope(scope) / ".cache";
}

std::optional<std::filesystem::path> find_built_executable(
    const std::filesystem::path& buildDir,
    const std::string& preferredName) {
    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(buildDir, ec)) {
        if (ec) {
            break;
        }
        if (!executable_file(entry.path())) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (filename == preferredName) {
            return entry.path();
        }
        if (entry.path().string().find("/CMakeFiles/") == std::string::npos) {
            candidates.push_back(entry.path());
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (!candidates.empty()) {
        return candidates.front();
    }
    return std::nullopt;
}

wl2::Result<void> check_app_install_prerequisites() {
    if (!command_available("git")) {
        return wl2::Error("app_prerequisite_missing", "Required tool not found on PATH: git");
    }
    if (!command_available("cmake")) {
        return wl2::Error("app_prerequisite_missing", "Required tool not found on PATH: cmake");
    }
    if (!command_available("ninja") && !command_available("make")) {
        return wl2::Error("app_prerequisite_missing", "Required build backend not found on PATH: ninja or make");
    }
    if (!command_available("c++") && !command_available("cc")) {
        return wl2::Error("app_prerequisite_missing", "Required C/C++ compiler not found on PATH: c++ or cc");
    }
    const char* prefix = std::getenv("WL2_PACKAGE_PREFIX");
    if (prefix && *prefix) {
        std::error_code ec;
        const auto packageDir = std::filesystem::path(prefix) / "lib" / "cmake" / "winglib2";
        if (!std::filesystem::is_directory(packageDir, ec)) {
            return wl2::Error("app_prerequisite_missing",
                "WL2_PACKAGE_PREFIX does not contain lib/cmake/winglib2: " + std::string(prefix));
        }
    }
    return {};
}

std::optional<wl2::ModuleScope> parse_required_scope(const std::string& value) {
    auto scope = wl2::parseModuleScope(value);
    if (!scope) {
        std::cerr << "invalid scope: " << value << '\n';
    }
    return scope;
}

int app_install(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "wl2 app install requires a <repo[#ref][:path]>\n";
        return 2;
    }
    std::string sourceArg;
    std::optional<wl2::ModuleScope> scope;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires local, user, or system\n";
                return 2;
            }
            scope = parse_required_scope(argv[i]);
            if (!scope) {
                return 2;
            }
        } else if (arg.rfind("-", 0) == 0) {
            std::cerr << "unknown app install option: " << arg << '\n';
            return 2;
        } else {
            sourceArg = arg;
        }
    }
    if (sourceArg.empty()) {
        std::cerr << "wl2 app install requires a <repo[#ref][:path]>\n";
        return 2;
    }
    auto prereqs = check_app_install_prerequisites();
    if (!prereqs) {
        print_error(prereqs.error(), StackTraceMode::Auto);
        return 1;
    }
    auto spec = wl2::normalizeAppSource(sourceArg);
    if (!spec) {
        std::cerr << "invalid app source: " << sourceArg << '\n';
        return 2;
    }

    const auto targetScope = scope.value_or(wl2::ModuleScope::Local);
    const auto slug = sanitize_identifier(std::filesystem::path(spec->path.empty() ? spec->repo : spec->path).filename().string());
    const auto cacheRoot = app_cache_root(targetScope) / slug;
    const auto checkoutDir = cacheRoot / "src";
    const auto buildDir = cacheRoot / "build";

    std::error_code ec;
    std::filesystem::remove_all(checkoutDir, ec);
    std::filesystem::create_directories(cacheRoot, ec);
    std::string clone = "git clone " + shell_quote(spec->repo) + " " + shell_quote(checkoutDir.string());
    if (run_shell_command(clone) != 0) {
        std::cerr << "git clone failed for " << spec->repo << '\n';
        return 1;
    }
    if (!spec->ref.empty()) {
        std::string checkout = "git -C " + shell_quote(checkoutDir.string()) + " checkout --quiet " + shell_quote(spec->ref);
        if (run_shell_command(checkout) != 0) {
            std::cerr << "git checkout failed for " << spec->ref << '\n';
            return 1;
        }
    }

    const auto sourceDir = spec->path.empty() ? checkoutDir : checkoutDir / spec->path;
    auto manifest = wl2::loadResourceManifest(sourceDir / "wl2.yml");
    if (!manifest) {
        print_error(manifest.error(), StackTraceMode::Auto);
        return 1;
    }
    if (!manifest.value().moduleDependencies.empty()) {
        std::cerr << "app install does not build manifest module dependencies yet; run wl2 deps first or install modules separately\n";
        return 1;
    }

    std::filesystem::remove_all(buildDir, ec);
    std::string configure = "cmake -S " + shell_quote(sourceDir.string()) + " -B " + shell_quote(buildDir.string());
    const char* packagePrefix = std::getenv("WL2_PACKAGE_PREFIX");
    if (packagePrefix && *packagePrefix) {
        configure += " -DCMAKE_PREFIX_PATH=" + shell_quote(packagePrefix);
    }
    if (run_shell_command(configure) != 0) {
        std::cerr << "cmake configure failed for app source " << sourceDir.string() << '\n';
        return 1;
    }
    std::string build = "cmake --build " + shell_quote(buildDir.string());
    if (run_shell_command(build) != 0) {
        std::cerr << "cmake build failed for app source " << sourceDir.string() << '\n';
        return 1;
    }

    const std::string appName = slug;
    auto executable = find_built_executable(buildDir, appName);
    if (!executable) {
        std::cerr << "no built executable found under " << buildDir.string() << '\n';
        return 1;
    }
    wl2::AppInstallPayload payload;
    payload.name = appName;
    payload.version = "0.1.0";
    payload.executablePath = *executable;
    payload.source = spec->repo;
    payload.ref = spec->ref;
    payload.path = spec->path;
    wl2::AppStore store(wl2::resolveAppScopePaths());
    auto installed = store.install(payload, targetScope);
    if (!installed) {
        print_error(installed.error(), StackTraceMode::Auto);
        return 1;
    }
    std::cout << "installed app " << installed.value().name
              << " (scope " << wl2::moduleScopeName(installed.value().scope) << ")\n";
    std::cout << "  launcher: " << installed.value().launcherPath.string() << '\n';
    return 0;
}

int app_list(int argc, char** argv) {
    std::optional<wl2::ModuleScope> scope;
    bool allScopes = true;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires all, local, user, or system\n";
                return 2;
            }
            std::string value(argv[i]);
            if (value == "all") {
                allScopes = true;
                scope.reset();
            } else {
                scope = parse_required_scope(value);
                if (!scope) {
                    return 2;
                }
                allScopes = false;
            }
        } else {
            std::cerr << "unknown app list option: " << arg << '\n';
            return 2;
        }
    }
    wl2::AppStore store(wl2::resolveAppScopePaths());
    auto apps = allScopes ? store.list() : store.scanScope(*scope);
    if (apps.empty()) {
        std::cout << "no installed apps\n";
        return 0;
    }
    for (const auto& app : apps) {
        std::cout << app.name << "  " << app.version << "  [" << wl2::moduleScopeName(app.scope) << "]";
        if (app.shadowed) {
            std::cout << " (shadowed)";
        }
        std::cout << '\n';
    }
    return 0;
}

int app_run(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "wl2 app run requires a <name-or-path>\n";
        return 2;
    }
    std::string target(argv[3]);
    std::vector<std::string> args;
    bool afterDash = false;
    for (int i = 4; i < argc; ++i) {
        std::string arg(argv[i]);
        if (!afterDash && arg == "--") {
            afterDash = true;
            continue;
        }
        args.push_back(std::move(arg));
    }

    std::filesystem::path executable = target;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(executable, ec)) {
        wl2::AppStore store(wl2::resolveAppScopePaths());
        auto record = store.resolve(target);
        if (!record) {
            std::cerr << "app not found: " << target << '\n';
            return 1;
        }
        executable = record->launcherPath;
    }
    std::string command = shell_quote(executable.string());
    for (const auto& arg : args) {
        command += " " + shell_quote(arg);
    }
    return run_shell_command(command) == 0 ? 0 : 1;
}

int app_uninstall(int argc, char** argv) {
    std::string name;
    std::optional<wl2::ModuleScope> scope;
    bool purgeCache = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--scope") {
            if (++i >= argc) {
                std::cerr << "--scope requires local, user, or system\n";
                return 2;
            }
            scope = parse_required_scope(argv[i]);
            if (!scope) {
                return 2;
            }
        } else if (arg == "--purge-cache") {
            purgeCache = true;
        } else if (arg.rfind("-", 0) == 0) {
            std::cerr << "unknown app uninstall option: " << arg << '\n';
            return 2;
        } else {
            name = arg;
        }
    }
    if (name.empty()) {
        std::cerr << "wl2 app uninstall requires a <name>\n";
        return 2;
    }
    wl2::AppStore store(wl2::resolveAppScopePaths());
    auto rc = store.uninstall(name, scope, purgeCache);
    if (!rc) {
        print_error(rc.error(), StackTraceMode::Auto);
        return 1;
    }
    std::cout << "uninstalled app " << name << '\n';
    return 0;
}

int app_command(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "wl2 app requires an action: install, list, run, uninstall\n";
        return 2;
    }
    std::string action(argv[2]);
    if (action == "install") {
        return app_install(argc, argv);
    }
    if (action == "list") {
        return app_list(argc, argv);
    }
    if (action == "run") {
        return app_run(argc, argv);
    }
    if (action == "uninstall") {
        return app_uninstall(argc, argv);
    }
    std::cerr << "unknown app action: " << action << '\n';
    return 2;
}

// Search a module build directory for a loadable wl2 dynamic module library.
// When expectedName is non-empty it disambiguates multiple matches.
wl2::Result<std::filesystem::path> find_built_module_library(
    const std::filesystem::path& buildDir, const std::string& expectedName) {
    std::error_code ec;
    if (!std::filesystem::is_directory(buildDir, ec)) {
        return wl2::Error("deps_not_built", "Build directory not found: " + buildDir.string());
    }
    std::vector<std::filesystem::path> matches;
    std::filesystem::path named;
    for (auto it = std::filesystem::recursive_directory_iterator(buildDir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto ext = it->path().extension().string();
        if (ext != ".so" && ext != ".dylib" && ext != ".dll") {
            continue;
        }
        auto info = wl2::ModuleLoader::inspectDynamicModule(it->path());
        if (!info) {
            continue;
        }
        matches.push_back(it->path());
        if (!expectedName.empty() && info.value().name == expectedName) {
            named = it->path();
        }
    }
    if (!named.empty()) {
        return named;
    }
    if (matches.empty()) {
        return wl2::Error("deps_not_built",
            "No built module library found under " + buildDir.string() + "; run 'wl2 deps build'");
    }
    if (matches.size() > 1) {
        return wl2::Error("deps_ambiguous_library",
            "Multiple module libraries found under " + buildDir.string() + "; module must declare provides");
    }
    return matches.front();
}

struct DepsBuildOptions {
    std::filesystem::path prefix;     // winglib2 package prefix (CMAKE_PREFIX_PATH)
    std::string generator;            // optional CMake generator
    std::string buildType;            // optional CMAKE_BUILD_TYPE / --config
};

int deps_command(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "wl2 deps requires an action: lock, fetch, build, install, status\n";
        return 2;
    }
    std::string action(argv[2]);
    std::filesystem::path manifestPath = "wl2.yml";
    DepsBuildOptions buildOptions;
    if (const char* envPrefix = std::getenv("CMAKE_PREFIX_PATH"); envPrefix && *envPrefix) {
        buildOptions.prefix = envPrefix;
    }
    for (int i = 3; i < argc; ++i) {
        std::string arg(argv[i]);
        auto takeValue = [&](std::filesystem::path& out) {
            if (++i >= argc) {
                return false;
            }
            out = argv[i];
            return true;
        };
        if (arg == "--manifest") {
            if (++i >= argc) {
                std::cerr << "--manifest requires a path\n";
                return 2;
            }
            manifestPath = argv[i];
        } else if (arg.rfind("--manifest=", 0) == 0) {
            manifestPath = arg.substr(std::string_view("--manifest=").size());
        } else if (arg == "--prefix") {
            if (!takeValue(buildOptions.prefix)) {
                std::cerr << "--prefix requires a path\n";
                return 2;
            }
        } else if (arg.rfind("--prefix=", 0) == 0) {
            buildOptions.prefix = arg.substr(std::string_view("--prefix=").size());
        } else if (arg == "--generator") {
            if (++i >= argc) {
                std::cerr << "--generator requires a value\n";
                return 2;
            }
            buildOptions.generator = argv[i];
        } else if (arg.rfind("--generator=", 0) == 0) {
            buildOptions.generator = arg.substr(std::string_view("--generator=").size());
        } else if (arg == "--build-type") {
            if (++i >= argc) {
                std::cerr << "--build-type requires a value\n";
                return 2;
            }
            buildOptions.buildType = argv[i];
        } else if (arg.rfind("--build-type=", 0) == 0) {
            buildOptions.buildType = arg.substr(std::string_view("--build-type=").size());
        } else {
            std::cerr << "unknown deps option: " << arg << '\n';
            return 2;
        }
    }

    auto manifest = wl2::loadResourceManifest(manifestPath);
    if (!manifest) {
        print_error(manifest.error(), StackTraceMode::Auto);
        return 1;
    }
    const auto baseDir = manifest.value().baseDir;
    const auto lockPath = baseDir / "wl2.lock.yml";
    const auto depsRoot = baseDir / ".wl2" / "deps";
    const auto buildRoot = baseDir / ".wl2" / "build" / "modules";

    if (action == "lock") {
        // Transitive lock fetches each dependency so its source metadata can be
        // read and its own source dependencies added to the closure.
        auto lock = wl2::lockModuleDependenciesTransitive(manifest.value().moduleDependencies, depsRoot);
        if (!lock) {
            print_error(lock.error(), StackTraceMode::Auto);
            return 1;
        }
        if (auto rc = wl2::writeLockfile(lockPath, lock.value()); !rc) {
            print_error(rc.error(), StackTraceMode::Auto);
            return 1;
        }
        std::cout << "wrote " << lockPath.string() << " (" << lock.value().modules.size() << " modules)\n";
        return 0;
    }
    if (action == "fetch") {
        auto lock = wl2::loadLockfile(lockPath);
        if (!lock) {
            std::cerr << "no lockfile at " << lockPath.string() << "; run 'wl2 deps lock' first\n";
            return 1;
        }
        for (const auto& module : lock.value().modules) {
            auto rc = wl2::fetchLockedModule(module, depsRoot);
            if (!rc) {
                print_error(rc.error(), StackTraceMode::Auto);
                return 1;
            }
            std::cout << "fetched " << module.name << " @ " << module.commit.substr(0, 12) << '\n';
        }
        return 0;
    }
    if (action == "build") {
        auto lock = wl2::loadLockfile(lockPath);
        if (!lock) {
            std::cerr << "no lockfile at " << lockPath.string() << "; run 'wl2 deps lock' first\n";
            return 1;
        }
        auto ordered = wl2::orderLockedModulesForBuild(lock.value(), depsRoot);
        if (!ordered) {
            print_error(ordered.error(), StackTraceMode::Auto);
            return 1;
        }
        for (const auto& module : ordered.value()) {
            // Refuse to build when the checkout does not match the lockfile.
            if (auto ok = wl2::verifyFetchedCommit(module, depsRoot); !ok) {
                print_error(ok.error(), StackTraceMode::Auto);
                return 1;
            }
            auto sourceDir = depsRoot / module.name;
            if (!module.path.empty()) {
                sourceDir /= module.path;
            }
            const auto moduleBuildDir = buildRoot / module.name;
            std::string configure = "cmake -S " + shell_quote(sourceDir.string())
                + " -B " + shell_quote(moduleBuildDir.string());
            if (!buildOptions.generator.empty()) {
                configure += " -G " + shell_quote(buildOptions.generator);
            }
            if (!buildOptions.prefix.empty()) {
                configure += " -DCMAKE_PREFIX_PATH=" + shell_quote(buildOptions.prefix.string());
            }
            if (!buildOptions.buildType.empty()) {
                configure += " -DCMAKE_BUILD_TYPE=" + shell_quote(buildOptions.buildType);
            }
            if (run_shell_command(configure) != 0) {
                std::cerr << "configure failed for " << module.name << '\n';
                return 1;
            }
            std::string build = "cmake --build " + shell_quote(moduleBuildDir.string());
            if (!buildOptions.buildType.empty()) {
                build += " --config " + shell_quote(buildOptions.buildType);
            }
            if (run_shell_command(build) != 0) {
                std::cerr << "build failed for " << module.name << '\n';
                return 1;
            }
            std::cout << "built " << module.name << '\n';
        }
        return 0;
    }
    if (action == "install") {
        auto lock = wl2::loadLockfile(lockPath);
        if (!lock) {
            std::cerr << "no lockfile at " << lockPath.string() << "; run 'wl2 deps lock' first\n";
            return 1;
        }
        auto ordered = wl2::orderLockedModulesForBuild(lock.value(), depsRoot);
        if (!ordered) {
            print_error(ordered.error(), StackTraceMode::Auto);
            return 1;
        }
        // Install into the project-local scope by default.
        wl2::ModuleStore store(wl2::resolveModuleScopePaths(baseDir));
        for (const auto& module : ordered.value()) {
            const auto moduleBuildDir = buildRoot / module.name;
            auto library = find_built_module_library(moduleBuildDir, module.provides);
            if (!library) {
                print_error(library.error(), StackTraceMode::Auto);
                return 1;
            }
            auto installed = store.install(library.value(), wl2::ModuleScope::Local);
            if (!installed) {
                print_error(installed.error(), StackTraceMode::Auto);
                return 1;
            }
            std::cout << "installed " << installed.value().name << " " << installed.value().version;
            if (!installed.value().build.empty()) {
                std::cout << " build=" << installed.value().build;
            }
            std::cout << " (local)\n";
        }
        return 0;
    }
    if (action == "status") {
        wl2::Lockfile lock;
        if (auto loaded = wl2::loadLockfile(lockPath)) {
            lock = loaded.value();
        }
        wl2::ModuleStore store(wl2::resolveModuleScopePaths(baseDir));
        for (auto status : wl2::dependencyStatus(manifest.value().moduleDependencies, lock, depsRoot)) {
            if (const auto* locked = lock.find(status.name)) {
                status.provides = locked->provides;
            }
            const auto moduleBuildDir = buildRoot / status.name;
            status.built = static_cast<bool>(find_built_module_library(moduleBuildDir, status.provides));
            if (!status.provides.empty()) {
                status.installed = static_cast<bool>(store.resolve(status.provides));
            }
            std::cout << status.name
                      << "  tag=" << (status.tag.empty() ? "-" : status.tag)
                      << "  locked=" << (status.lockedCommit.empty() ? "-" : status.lockedCommit.substr(0, 12))
                      << "  fetched=" << (status.fetched ? "yes" : "no")
                      << "  built=" << (status.built ? "yes" : "no")
                      << "  installed=" << (status.installed ? "yes" : "no") << '\n';
        }
        return 0;
    }
    std::cerr << "unknown deps action: " << action << '\n';
    return 2;
}

int show_api(wl2::RuntimeOptions options, const std::string& moduleName) {
    wl2::Runtime runtime(std::move(options));
    auto init = runtime.initialize();
    if (!init) {
        print_error(init.error(), StackTraceMode::Auto);
        return 1;
    }
    auto* module = runtime.modules().find(moduleName);
    if (!module) {
        std::cerr << "module not found: " << moduleName << '\n';
        return 1;
    }
    std::cout << module->name << " " << module->version << "\n\n";
    if (!module->build.empty()) {
        std::cout << "build: " << module->build << "\n\n";
    }
    std::cout << module->summary << "\n\n" << module->api << "\n";
    return 0;
}

// `wl2 graphics`: report the 3D renderer and, when a GPU is reachable, the device
// and GL version. Runs a small probe through the runtime (the renderer lives in
// the wl2:3d module, accessed via JS) rather than coupling the CLI to a module.
// Grants the graphics capability since the user explicitly asked to probe it.
int graphics_command(wl2::RuntimeOptions options) {
    options.allowGraphics = true;

    static const char kProbe[] =
        "const td = await import(\"wl2:3d\");\n"
        "const g = td.queryGraphics();\n"
        "const lines = [];\n"
        "lines.push(`renderer:   ${g.renderer}`);\n"
        "lines.push(`gpu:        ${g.available ? \"available\" : \"unavailable\"}`);\n"
        "if (g.renderer === \"magnum\" && !g.authorized) {\n"
        "  lines.push(\"note:       graphics capability not granted\");\n"
        "}\n"
        "if (g.device) lines.push(`device:     ${g.device}`);\n"
        "if (g.glVersion) lines.push(`gl-version: ${g.glVersion}`);\n"
        "console.log(lines.join(\"\\n\"));\n";

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        std::cerr << "wl2 graphics: no temp directory available: " << ec.message() << '\n';
        return 1;
    }
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path probe = dir / ("wl2-graphics-probe-" + std::to_string(unique) + ".js");
    if (!write_text_file(probe, kProbe)) {
        std::cerr << "wl2 graphics: could not write probe script to " << probe << '\n';
        return 1;
    }
    wl2::Runtime runtime(std::move(options));
    auto result = runtime.runModule(probe.string());
    std::filesystem::remove(probe, ec);
    if (!result) {
        print_error(result.error(), StackTraceMode::Off);
        return 1;
    }
    return result.value();
}

} // namespace

int main(int argc, char** argv) {
    g_processArgv.assign(argv, argv + argc);

    wl2::RuntimeOptions options;
    wl2_register_builtin_static_modules(options);

    if (argc < 2) {
        usage();
        return 2;
    }

    std::string command(argv[1]);
    if (command == "help" || command == "--help" || command == "-h") {
        usage(std::cout);
        return 0;
    }

    // A `--help`/`-h` anywhere in a subcommand's own arguments prints usage.
    // Stop scanning at `--` so script arguments are never intercepted.
    for (int i = 2; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--") {
            break;
        }
        if (arg == "--help" || arg == "-h") {
            usage(std::cout);
            return 0;
        }
    }

    if (command == "version" || command == "--version") {
        std::cout << WL2_VERSION << '\n';
        return 0;
    }

    if (command == "showapi" || command == "--showapi") {
        if (argc < 3) {
            usage();
            return 2;
        }
        return show_api(std::move(options), argv[2]);
    }

    if (command == "graphics") {
        return graphics_command(std::move(options));
    }

    if (command == "config") {
        auto config = parse_config_command(argc, argv, 2);
        if (!config) {
            return 2;
        }
        return config_command(*config);
    }

    if (command == "resources") {
        auto resources = parse_resource_command(argc, argv, 2);
        if (!resources) {
            return 2;
        }
        return resources_command(*resources);
    }

    if (command == "module") {
        return module_command(argc, argv);
    }

    if (command == "deps") {
        return deps_command(argc, argv);
    }

    if (command == "app") {
        return app_command(argc, argv);
    }

    if (command == "test") {
        auto test = parse_test_command(argc, argv, 2);
        if (!test) {
            return 2;
        }
        return test_command(std::move(options), *test);
    }

    if (command == "init") {
        return init_command(argc, argv);
    }

    if (is_planned_subcommand(command)) {
        std::cerr << "wl2 " << command << " is not implemented yet\n";
        return 2;
    }

    if (command == "run") {
        auto run = parse_run_command(argc, argv, 2, false);
        if (!run) {
            return 2;
        }
        if (run->watch) {
            return watch_run(std::move(options), *run);
        }
        return run_script(std::move(options), *run);
    }

    // Compatibility path for existing direct script invocations.
    auto run = parse_run_command(argc, argv, 1, true);
    if (!run) {
        return 2;
    }
    if (run->watch) {
        return watch_run(std::move(options), *run);
    }
    return run_script(std::move(options), *run);
}
