#include "wl2/wl2.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int wl2_resource_tests_entry() {
    wl2::ResourceStore store;
    const unsigned char bytes[] = {'o', 'k'};
    store.add("wl2:/app/main.js", bytes, sizeof(bytes));
    assert(store.contains("wl2:/app/main.js"));
    auto res = store.get("wl2:/app/main.js");
    assert(res);
    assert(res->bytes.size() == 2);

    auto opened = store.open("wl2:/app/main.js");
    assert(opened);
    assert(opened.value().size() == 2);
    assert(opened.value().text() == "ok");
    assert(opened.value().entry().compression == wl2::ResourceCompression::Stored);

    const unsigned char storedBytes[] = {'a', 'b', 'c'};
    wl2::ResourceEntry storedEntry;
    storedEntry.name = "wl2:/app/assets/plain.txt";
    storedEntry.contentHash = "plain-hash";
    store.addResource(storedEntry, storedBytes, sizeof(storedBytes));
    auto plain = store.open("wl2:/app/assets/plain.txt");
    assert(plain);
    assert(plain.value().text() == "abc");
    assert(plain.value().data() == plain.value().bytes().data());

    const unsigned char compressed[] = {
        3, static_cast<unsigned char>('x'),
        2, static_cast<unsigned char>('y'),
        1, static_cast<unsigned char>('z'),
    };
    wl2::ResourceEntry compressedEntry;
    compressedEntry.name = "wl2:/app/assets/compressed.txt";
    compressedEntry.originalSize = 6;
    compressedEntry.compression = wl2::ResourceCompression::Rle;
    compressedEntry.contentHash = "compressed-hash";
    store.addResource(compressedEntry, compressed, sizeof(compressed));

    auto inflatedA = store.open("wl2:/app/assets/compressed.txt");
    auto inflatedB = store.open("wl2:/app/assets/compressed.txt");
    assert(inflatedA);
    assert(inflatedB);
    assert(inflatedA.value().text() == "xxxyyz");
    assert(inflatedA.value().data() == inflatedB.value().data());
    assert(inflatedA.value().entry().storedSize == sizeof(compressed));
    assert(inflatedA.value().entry().originalSize == 6);

    assert(store.exists("wl2:/app/assets"));
    assert(store.isDirectory("wl2:/app/assets"));
    auto listed = store.list("wl2:/app");
    assert(!listed.empty());
    bool foundAssets = false;
    for (const auto& entry : listed) {
        if (entry.path == "wl2:/app/assets" && entry.directory) {
            foundAssets = true;
        }
    }
    assert(foundAssets);

    auto walked = store.walk("wl2:/app/assets");
    assert(walked.size() == 2);
    assert(walked[0].path == "wl2:/app/assets/compressed.txt");
    assert(walked[1].path == "wl2:/app/assets/plain.txt");

    auto missing = store.open("wl2:/app/missing.txt");
    assert(!missing);

    auto tempRoot = std::filesystem::temp_directory_path()
        / ("wl2_resource_map_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempRoot / "nested");
    {
        std::ofstream out(tempRoot / "config.json", std::ios::binary);
        out << "{\"name\":\"mapped\"}";
    }
    {
        std::ofstream out(tempRoot / "nested" / "info.txt", std::ios::binary);
        out << "nested";
    }
    const unsigned char embeddedConfig[] = {'e', 'm', 'b', 'e', 'd'};
    store.add("wl2:/mapped/config.json", embeddedConfig, sizeof(embeddedConfig));
    auto mounted = store.mountDirectory(tempRoot, "wl2:/mapped");
    assert(mounted);
    auto mappedConfig = store.open("wl2:/mapped/config.json");
    assert(mappedConfig);
    assert(mappedConfig.value().text() == "{\"name\":\"mapped\"}");
    assert(store.exists("wl2:/mapped/nested"));
    auto mappedList = store.list("wl2:/mapped");
    assert(mappedList.size() == 2);
    auto mappedWalk = store.walk("wl2:/mapped");
    assert(mappedWalk.size() == 2);
    assert(!mappedWalk[0].sourcePath.empty());
    auto escaped = store.open("wl2:/mapped/../config.json");
    assert(!escaped);

    auto manifestRoot = tempRoot / "manifest";
    std::filesystem::create_directories(manifestRoot / "files");
    {
        std::ofstream out(manifestRoot / "files" / "main.js", std::ios::binary);
        out << "console.log('manifest');\n";
    }
    {
        std::ofstream out(manifestRoot / "files" / "config.json", std::ios::binary);
        out << "{}\n";
    }
    {
        std::ofstream out(manifestRoot / "resources.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\n"
            << "prefix: wl2:/manifest\n"
            << "root: files\n"
            << "entry: main.js\n"
            << "resources:\n"
            << "  store:\n"
            << "    files:\n"
            << "      - main.js\n"
            << "      - config.json\n";
    }
    auto manifest = wl2::loadResourceManifest(manifestRoot / "resources.yml");
    assert(manifest);
    assert(manifest.value().prefix == "wl2:/manifest");
    assert(manifest.value().entrySpecifier() == "wl2:/manifest/main.js");
    assert(manifest.value().store.files.size() == 2);

    {
        std::ofstream out(manifestRoot / "bad_schema.yml", std::ios::binary);
        out << "schema: wl2.bad.v1\nroot: files\nentry: main.js\n";
    }
    assert(!wl2::loadResourceManifest(manifestRoot / "bad_schema.yml"));
    {
        std::ofstream out(manifestRoot / "unknown.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nroot: files\nentry: main.js\nsurprise: true\n";
    }
    assert(!wl2::loadResourceManifest(manifestRoot / "unknown.yml"));
    {
        std::ofstream out(manifestRoot / "escape.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nroot: files\nentry: ../main.js\n";
    }
    assert(!wl2::loadResourceManifest(manifestRoot / "escape.yml"));
    {
        std::ofstream out(manifestRoot / "absolute.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "resources:\n  store:\n    files:\n      - /etc/passwd\n";
    }
    auto absolute = wl2::loadResourceManifest(manifestRoot / "absolute.yml");
    assert(!absolute && absolute.error().code() == "manifest_invalid_path");
    {
        std::ofstream out(manifestRoot / "missing.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "resources:\n  store:\n    files:\n      - main.js\n      - ghost.js\n";
    }
    auto missingFile = wl2::loadResourceManifest(manifestRoot / "missing.yml");
    assert(!missingFile && missingFile.error().code() == "manifest_missing_file");
    {
        std::ofstream out(manifestRoot / "missing_dir.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "resources:\n  auto:\n    directories:\n      - ghosts\n";
    }
    auto missingDir = wl2::loadResourceManifest(manifestRoot / "missing_dir.yml");
    assert(!missingDir && missingDir.error().code() == "manifest_missing_directory");
    {
        std::ofstream out(manifestRoot / "duplicate.yml", std::ios::binary);
        out << "schema: wl2.resources.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "resources:\n  store:\n    files:\n      - main.js\n  compress:\n    files:\n      - main.js\n";
    }
    auto duplicate = wl2::loadResourceManifest(manifestRoot / "duplicate.yml");
    assert(!duplicate && duplicate.error().code() == "manifest_duplicate_path");
    {
        std::ofstream out(manifestRoot / "modules.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "modules:\n  require:\n    - wl2:echo\n  optional:\n    - wl2:curl\n";
    }
    auto modules = wl2::loadResourceManifest(manifestRoot / "modules.yml");
    assert(modules);
    assert(modules.value().requiredModules.size() == 1
        && modules.value().requiredModules[0] == "wl2:echo");
    assert(modules.value().optionalModules.size() == 1
        && modules.value().optionalModules[0] == "wl2:curl");
    assert(!modules.value().allowUi);
    {
        std::ofstream out(manifestRoot / "capabilities.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "capabilities:\n  ui: true\n";
    }
    auto capabilities = wl2::loadResourceManifest(manifestRoot / "capabilities.yml");
    assert(capabilities);
    assert(capabilities.value().allowUi);
    {
        std::ofstream out(manifestRoot / "bad_capabilities.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "capabilities:\n  ui: maybe\n";
    }
    auto badCapabilities = wl2::loadResourceManifest(manifestRoot / "bad_capabilities.yml");
    assert(!badCapabilities && badCapabilities.error().code() == "manifest_invalid_capabilities");
    {
        std::ofstream out(manifestRoot / "modules_dup.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "modules:\n  require:\n    - wl2:echo\n  optional:\n    - wl2:echo\n";
    }
    auto modulesDup = wl2::loadResourceManifest(manifestRoot / "modules_dup.yml");
    assert(!modulesDup && modulesDup.error().code() == "manifest_duplicate_module");
    {
        std::ofstream out(manifestRoot / "deps.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "dependencies:\n  modules:\n"
            << "    - name: wl2_echo\n      git: https://example.com/wl2_echo.git\n      tag: v1.0.0\n"
            << "    - name: wl2_other\n      git: https://example.com/wl2_other.git\n      commit: abc123\n";
    }
    auto deps = wl2::loadResourceManifest(manifestRoot / "deps.yml");
    assert(deps);
    assert(deps.value().moduleDependencies.size() == 2);
    assert(deps.value().moduleDependencies[0].name == "wl2_echo");
    assert(deps.value().moduleDependencies[0].git == "https://example.com/wl2_echo.git");
    assert(deps.value().moduleDependencies[0].tag == "v1.0.0");
    assert(deps.value().moduleDependencies[1].commit == "abc123");
    {
        std::ofstream out(manifestRoot / "deps_branch.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "dependencies:\n  modules:\n"
            << "    - name: wl2_echo\n      git: https://example.com/wl2_echo.git\n      branch: main\n";
    }
    auto depsBranch = wl2::loadResourceManifest(manifestRoot / "deps_branch.yml");
    assert(!depsBranch && depsBranch.error().code() == "manifest_floating_branch");
    {
        std::ofstream out(manifestRoot / "deps_unpinned.yml", std::ios::binary);
        out << "schema: wl2.project.v1\nprefix: wl2:/manifest\nroot: files\nentry: main.js\n"
            << "dependencies:\n  modules:\n"
            << "    - name: wl2_echo\n      git: https://example.com/wl2_echo.git\n";
    }
    auto depsUnpinned = wl2::loadResourceManifest(manifestRoot / "deps_unpinned.yml");
    assert(!depsUnpinned && depsUnpinned.error().code() == "manifest_invalid_dependency");
    std::filesystem::remove_all(tempRoot);

#if WL2_HAVE_QUICKJS
    wl2::Runtime runtime;
    runtime.resources().add("wl2:/app/assets/plain.txt", storedBytes, sizeof(storedBytes));
    runtime.resources().addResource(compressedEntry, compressed, sizeof(compressed));

    const std::string script = R"(
function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

assert(wl2.resources.exists("wl2:/app/assets"), "assets directory should exist");
assert(wl2.resources.exists("wl2:/app/assets/plain.txt"), "plain resource should exist");
assert(!wl2.resources.exists("wl2:/app/assets/missing.txt"), "missing resource should not exist");

const dirStat = wl2.resources.stat("wl2:/app/assets");
assert(dirStat.directory === true, "assets should stat as a directory");

const plainStat = wl2.resources.stat("wl2:/app/assets/plain.txt");
assert(plainStat.directory === false, "plain resource should stat as a file");
assert(plainStat.name === "plain.txt", `unexpected plain name: ${plainStat.name}`);
assert(plainStat.size === 3, `unexpected plain size: ${plainStat.size}`);
assert(plainStat.compressed === false, "plain resource should not be compressed");

const compressedStat = wl2.resources.stat("wl2:/app/assets/compressed.txt");
assert(compressedStat.size === 6, `unexpected compressed original size: ${compressedStat.size}`);
assert(compressedStat.compressed === true, "compressed resource should report compressed");

const listed = wl2.resources.list("wl2:/app");
assert(listed.length === 1 && listed[0].path === "wl2:/app/assets" && listed[0].directory,
  "list should return the assets directory as a direct child");

const walked = wl2.resources.walk("wl2:/app/assets");
assert(walked.length === 2, `unexpected walk count: ${walked.length}`);
assert(walked[0].path === "wl2:/app/assets/compressed.txt", "walk should be sorted");
assert(walked[1].path === "wl2:/app/assets/plain.txt", "walk should include plain file");

assert(wl2.resources.readText("wl2:/app/assets/plain.txt") === "abc", "readText plain failed");
assert(wl2.resources.readText("wl2:/app/assets/compressed.txt") === "xxxyyz", "readText compressed failed");

const bytes = wl2.resources.readBytes("wl2:/app/assets/plain.txt");
assert(wl2.buffer.isBuffer(bytes), "readBytes should return wl2.Buffer");
assert(bytes.text() === "abc", "readBytes buffer text failed");

const handle = wl2.resources.open("wl2:/app/assets/compressed.txt");
assert(handle.path === "wl2:/app/assets/compressed.txt", "handle path mismatch");
assert(handle.size === 6, "handle size mismatch");
assert(handle.compressed === true, "handle compressed mismatch");
assert(handle.text() === "xxxyyz", "handle text failed");
assert(wl2.buffer.isBuffer(handle.bytes()), "handle bytes should return wl2.Buffer");
assert(handle.uint8Array() instanceof Uint8Array, "handle uint8Array should return Uint8Array");
handle.close();
assert(handle.closed === true, "handle should report closed");

let closedThrew = false;
try {
  handle.text();
} catch (err) {
  closedThrew = true;
}
assert(closedThrew, "closed handle access should throw");

let missingThrew = false;
try {
  wl2.resources.open("wl2:/app/assets/missing.txt");
} catch (err) {
  missingThrew = err.code === "resource_not_found";
}
assert(missingThrew, "opening a missing resource should throw ResourceError");
)";
    runtime.resources().add("wl2:/tests/resources.js",
        reinterpret_cast<const unsigned char*>(script.data()), script.size());
    auto jsResult = runtime.runModule("wl2:/tests/resources.js");
    assert(jsResult);
#endif

    std::cout << "resources ok\n";
    return 0;
}
