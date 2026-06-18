// Native, headless tests for wl2:slint. Drives the JavaScript engine with the
// slint module registered and the UI capability denied (the default), so no
// window is ever opened. Covers: compile -> create -> property round-trip for
// number/string/bool/struct, compile-error diagnostics, and show() denial.
#include "wl2/wl2.h"
#include "wl2_slint/wl2_slint.h"

#include <iostream>
#include <string>

namespace {

std::string js_escape(const std::string& value) {
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

int fail(const std::string& message) {
    std::cerr << "wl2_slint test failed: " << message << '\n';
    return 1;
}

int run_slint_tests() {
    wl2::RuntimeOptions options;
    // allowUi is left false (the default): show()/hide() must be denied.
    options.staticModules.push_back(wl2_slint_register_module);

    wl2::Runtime runtime{std::move(options)};
    if (auto init = runtime.initialize(); !init) {
        return fail("runtime initialize failed: " + init.error().message());
    }

    auto engine = wl2::createConfiguredJsEngine();

    const std::string fixturePath =
        std::string(WL2_SLINT_FIXTURE_DIR) + "/headless.slint";
    const std::string source = std::string(R"JS(
import { compile, compileFile, openFileDialog } from "wl2:slint";

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

// --- compile + instantiate + property round-trip (headless, no window) --------
const ui = await compile(`
  export component App {
    in-out property <string> name: "world";
    in-out property <int> count: 0;
    in-out property <bool> active: false;
    in-out property <{ x: int, y: int }> point: { x: 1, y: 2 };
    in-out property <[int]> items: [];
    in-out property <[{ id: int, label: string }]> rows: [];
    in-out property <brush> accent: #336699;
    callback add(int, int) -> int;
    callback bump();
  }
`);

const win = ui.create();

// defaults read back through the bridge
assert(["unknown", "dark", "light"].includes(win.colorScheme()),
  "unexpected color scheme: " + win.colorScheme());
assert(win.get("name") === "world", "default string mismatch: " + win.get("name"));
assert(win.get("count") === 0, "default number mismatch: " + win.get("count"));
assert(win.get("active") === false, "default bool mismatch: " + win.get("active"));
const p0 = win.get("point");
assert(p0.x === 1 && p0.y === 2, "default struct mismatch: " + JSON.stringify(p0));

// set then get for each supported scalar type
win.set("name", "winglib2");
assert(win.get("name") === "winglib2", "string round-trip failed: " + win.get("name"));

win.set("count", win.get("count") + 41);
assert(win.get("count") === 41, "number round-trip failed: " + win.get("count"));

win.set("active", true);
assert(win.get("active") === true, "bool round-trip failed: " + win.get("active"));

win.set("point", { x: 5, y: 6 });
const p1 = win.get("point");
assert(p1.x === 5 && p1.y === 6, "struct round-trip failed: " + JSON.stringify(p1));

// arrays <-> models, including arrays of structs
win.set("items", [10, 20, 30]);
const items = win.get("items");
assert(Array.isArray(items) && items.length === 3 && items[0] === 10 && items[2] === 30,
  "model round-trip failed: " + JSON.stringify(items));

win.set("rows", [{ id: 1, label: "a" }, { id: 2, label: "b" }]);
const rows = win.get("rows");
assert(rows.length === 2 && rows[1].id === 2 && rows[1].label === "b",
  "model-of-structs round-trip failed: " + JSON.stringify(rows));

// CSS hex strings can set brush/color properties.
win.set("accent", "#123456");
assert(win.get("accent") === "#123456", "brush hex round-trip failed: " + win.get("accent"));

// --- callbacks: on() + invoke(), arg/return marshaling, JS-driven property -----
// At this point count === 41.
win.on("add", (a, b) => a + b);
assert(win.invoke("add", 2, 3) === 5, "callback arg/return marshaling failed");

let fired = 0;
win.on("bump", () => { fired = win.get("count") + 100; win.set("count", 7); });
const bumpRet = win.invoke("bump");
assert(fired === 141, "callback did not fire with the expected state: " + fired);
assert(win.get("count") === 7, "JS-driven property change not reflected: " + win.get("count"));
assert(bumpRet === undefined, "void callback should return undefined");

// re-registering a handler replaces it
win.on("add", (a, b) => a * b);
assert(win.invoke("add", 4, 5) === 20, "callback re-registration failed");

// invoking an unknown callback is a stable error
let cbErr = null;
try { win.invoke("nope"); } catch (e) { cbErr = e; }
assert(cbErr && cbErr.code === "slint_unknown_callback", "unknown callback code wrong");

// registering an unknown callback is a stable error
let onErr = null;
try { win.on("nope", () => {}); } catch (e) { onErr = e; }
assert(onErr && onErr.code === "slint_unknown_callback", "unknown on() code wrong");

// unknown property surfaces a stable error
let unknownErr = null;
try { win.get("nope"); } catch (e) { unknownErr = e; }
assert(unknownErr && unknownErr.code === "slint_unknown_property", "unknown property code wrong");
assert(unknownErr.module === "wl2_slint" && unknownErr.name === "SlintError", "error metadata wrong");

// unsupported value type (a function) is a type error
let typeErr = null;
try { win.set("count", () => {}); } catch (e) { typeErr = e; }
assert(typeErr && typeErr.code === "slint_type_error", "type error code wrong: " + (typeErr && typeErr.code));

// --- compile error carries diagnostics ----------------------------------------
let compileErr = null;
try {
  await compile(`export component Broken { Text { text: ; } }`);
} catch (e) {
  compileErr = e;
}
assert(compileErr !== null, "broken markup should fail to compile");
assert(compileErr.code === "slint_compile_failed", "compile error code wrong: " + compileErr.code);
assert(Array.isArray(compileErr.diagnostics) && compileErr.diagnostics.length > 0,
  "compile error should carry diagnostics");
assert(typeof compileErr.diagnostics[0].message === "string", "diagnostic message missing");

// --- show() denied by default (UI capability) ---------------------------------
let denied = null;
try { win.show(); } catch (e) { denied = e; }
assert(denied !== null, "show() should be denied without the UI capability");
assert(denied.code === "slint_permission_denied", "show denial code wrong: " + denied.code);
assert(denied.module === "wl2_slint", "show denial module wrong");

let dialogDenied = null;
try { await openFileDialog({ title: "Denied" }); } catch (e) { dialogDenied = e; }
assert(dialogDenied !== null, "native dialogs should be denied without the UI capability");
assert(dialogDenied.code === "slint_permission_denied",
  "dialog denial code wrong: " + dialogDenied.code);

// compileFile() must not use the permissive script-loader path for host files.
// Native module reads are denied by default unless RuntimeOptions explicitly
// grants allowFilesystemReads and a read root.
let fsDenied = null;
try { await compileFile(")JS") + js_escape(fixturePath) + R"JS("); } catch (e) { fsDenied = e; }
assert(fsDenied && fsDenied.code === "slint_permission_denied",
  "host compileFile should be denied by filesystem read policy: " + (fsDenied && fsDenied.code));

globalThis.__slintTestOk = true;
)JS";

    auto result = engine->runModule(runtime, "wl2-slint-test.js", source);
    if (!result) {
        return fail(result.error().code() + ": " + result.error().message());
    }

    std::cout << "wl2_slint ok\n";
    return 0;
}

} // namespace

int main() {
    return run_slint_tests();
}
