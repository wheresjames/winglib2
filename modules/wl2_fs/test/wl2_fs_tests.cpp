#include "wl2/wl2.h"
#include "wl2_fs/wl2_fs.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

int fail(const std::string& message) {
    std::cerr << "wl2_fs test failed: " << message << '\n';
    return 1;
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// Build a temporary read root with a known layout, then exercise wl2:fs through
// the engine with the runtime filesystem policy enabled. The temp directory is
// removed before returning so no source-tree files are touched.
int run_fs_tests() {
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / ("wl2_fs_test_" + std::to_string(::getpid()));
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub", ec);
    if (ec) {
        return fail("could not create temp directory");
    }

    write_file(root / "hello.txt", "hello fs\n");           // 9 bytes
    write_file(root / "data.bin", std::string("\x00\x01\x02\x03", 4));
    write_file(root / "sub" / "nested.txt", "nested\n");

    // A sibling directory outside the read root, reachable only via symlinks
    // placed inside the root. Used to confirm symlink escape is denied.
    fs::path outside = root.string() + "_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    write_file(outside / "secret.txt", "secret\n");
    std::error_code linkEc;
    fs::create_directory_symlink(outside, root / "outlink", linkEc);
    const bool dirLinkOk = !linkEc;
    fs::create_symlink(outside / "secret.txt", root / "secretlink.txt", linkEc);
    const bool fileLinkOk = !linkEc;

    wl2::RuntimeOptions options;
    options.allowFilesystemReads = true;
    options.filesystemReadRoots = {root};
    options.staticModules.push_back(wl2_fs_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        fs::remove_all(root, ec);
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();

    const std::string rootLiteral = (root).generic_string();
    const std::string source = std::string(R"JS(
import { readText, readBytes, exists, stat, list, walk } from "wl2:fs";

const root = ")JS") + rootLiteral + "\";\n"
        + "const dirLinkOk = " + (dirLinkOk ? "true" : "false") + ";\n"
        + "const fileLinkOk = " + (fileLinkOk ? "true" : "false") + ";\n"
        + R"JS(
function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

// readText returns exact file contents.
assert(readText(root + "/hello.txt") === "hello fs\n", "readText content mismatch");

// readBytes returns a wl2.Buffer of the right length.
const bytes = readBytes(root + "/data.bin");
assert(wl2.buffer.isBuffer(bytes), "readBytes did not return a wl2.Buffer");
assert(bytes.byteLength === 4, "readBytes length mismatch: " + bytes.byteLength);

// exists distinguishes present and missing paths.
assert(exists(root + "/hello.txt") === true, "exists(present) should be true");
assert(exists(root + "/missing.txt") === false, "exists(missing) should be false");

// stat reports type and size; missing paths report exists:false.
const fileStat = stat(root + "/hello.txt");
assert(fileStat.exists && fileStat.isFile && !fileStat.isDirectory, "file stat flags wrong");
assert(fileStat.size === 9, "file stat size wrong: " + fileStat.size);
const dirStat = stat(root + "/sub");
assert(dirStat.exists && dirStat.isDirectory && !dirStat.isFile, "dir stat flags wrong");
assert(stat(root + "/missing.txt").exists === false, "missing stat should report exists:false");

// list enumerates direct children only (not recursively).
const names = list(root).map((e) => e.name);
for (const expected of ["data.bin", "hello.txt", "sub"]) {
  assert(names.includes(expected), "list missing " + expected + ": " + JSON.stringify(names));
}
assert(!names.includes("nested.txt"), "list should not recurse into subdirectories");

// walk descends recursively and returns paths relative to the walked root.
const walked = walk(root).map((e) => e.path);
assert(walked.includes("sub/nested.txt"), "walk missing nested file: " + JSON.stringify(walked));

// Policy: paths outside the root are rejected, including traversal attempts.
let outsideBlocked = false;
try { readText("/etc/hostname"); } catch (e) { outsideBlocked = e.code === "fs_permission_denied"; }
assert(outsideBlocked, "reading outside the root should be denied");

let escapeBlocked = false;
try { readText(root + "/../escape.txt"); } catch (e) { escapeBlocked = e.code === "fs_permission_denied"; }
assert(escapeBlocked, "path traversal out of the root should be denied");

// Policy: a symlink inside the root that resolves outside the root is denied,
// both for a symlinked directory and a symlinked file.
if (dirLinkOk) {
  let viaDirLink = false;
  try { readText(root + "/outlink/secret.txt"); } catch (e) { viaDirLink = e.code === "fs_permission_denied"; }
  assert(viaDirLink, "reading through a symlinked directory that escapes the root should be denied");
}
if (fileLinkOk) {
  let viaFileLink = false;
  try { readText(root + "/secretlink.txt"); } catch (e) { viaFileLink = e.code === "fs_permission_denied"; }
  assert(viaFileLink, "reading a symlink that points outside the root should be denied");
}

// Error shape carries stable diagnostics.
let missingErr = null;
try { readText(root + "/missing.txt"); } catch (e) { missingErr = e; }
assert(missingErr && missingErr.code === "fs_not_found", "missing read should be fs_not_found");
assert(missingErr.module === "wl2_fs" && missingErr.operation === "readText", "error metadata wrong");

// Reading a directory as a file is rejected.
let notFileBlocked = false;
try { readText(root + "/sub"); } catch (e) { notFileBlocked = e.code === "fs_not_a_file"; }
assert(notFileBlocked, "reading a directory should be fs_not_a_file");
)JS";

    auto result = engine->runModule(runtime, "wl2-fs-test.js", source);
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "wl2_fs ok\n";
    return 0;
}

} // namespace

int main() {
    return run_fs_tests();
}
