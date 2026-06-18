// wl2:slint showcase example.
//
// Run in-tree with:
//   wl2 run --allow-ui --map-resource modules/wl2_slint/examples/showcase:wl2:/showcase \
//     wl2:/showcase/scripts/main.js
//
// Or build and run wl2_slint_showcase_example. Passing --selftest opens the
// window briefly, drives a few callbacks from a Slint Timer, and exits.
import {
  compileFile,
  openFileDialog,
  openFilesDialog,
  pickFolderDialog,
  saveFileDialog,
} from "wl2:slint";

let ui;
try {
  ui = await compileFile("wl2:/showcase/ui/app.slint");
} catch (error) {
  if (Array.isArray(error.diagnostics)) {
    for (const diagnostic of error.diagnostics) {
      console.log(`${diagnostic.line}:${diagnostic.column}: ${diagnostic.message}`);
    }
  }
  throw error;
}

const compileOnly = (wl2.runtime.argv || []).includes("--compile-only");
if (compileOnly) {
  console.log("wl2:slint showcase compiled");
  globalThis.__showcaseCompiled = true;
} else {
const win = ui.create();

const themeArg = (wl2.runtime.argv || []).find((arg) => arg.startsWith("--theme="));
const initialTheme = themeArg ? themeArg.slice("--theme=".length) : "System";
let systemScheme = "unknown";

function syncSystemTheme() {
  systemScheme = typeof win.colorScheme === "function" ? win.colorScheme() : "unknown";
  win.set("system-dark", systemScheme === "dark");
}

let tasks = [
  { title: "Triage crash report", owner: "Mira", done: false, priority: 9 },
  { title: "Review module manifest", owner: "Jon", done: true, priority: 5 },
  { title: "Package Slint example", owner: "Ada", done: false, priority: 7 },
  { title: "Update dependency docs", owner: "Lee", done: false, priority: 4 },
];

let nextOwner = 0;
const owners = ["Ada", "Mira", "Jon", "Lee"];
const files = [
  { name: "app.slint", path: "wl2:/showcase/ui/app.slint", size: "14 KB" },
  { name: "main.js", path: "wl2:/showcase/scripts/main.js", size: "5 KB" },
  { name: "theme-preview.json", path: "wl2:/showcase/data/theme-preview.json", size: "2 KB" },
  { name: "release-notes.md", path: "wl2:/docs/release-notes.md", size: "9 KB" },
];
const colors = [
  { name: "Blue", value: "#2563eb" },
  { name: "Emerald", value: "#059669" },
  { name: "Violet", value: "#7c3aed" },
  { name: "Rose", value: "#e11d48" },
  { name: "Amber", value: "#d97706" },
];
const dialogFilters = [
  { name: "Slint and JavaScript", extensions: ["slint", "js", "json", "md"] },
  { name: "All text", extensions: "txt,md,json,slint,js" },
];

function statsFor(items) {
  const done = items.filter((task) => task.done).length;
  const open = items.length - done;
  const score = items.reduce((sum, task) => sum + (task.done ? 0 : task.priority), 0);
  return { open, done, score };
}

function sync() {
  win.set("tasks", tasks);
  win.set("stats", statsFor(tasks));
}

win.set("operator", "Winglib2 Ops");
win.set("status", "Ready");
win.set("threshold", 72);
win.set("notifications", true);
syncSystemTheme();
win.set("theme-mode", ["System", "Light", "Dark"].includes(initialTheme) ? initialTheme : "System");
win.set("files", files);
win.set("colors", colors);
win.set("accent", colors[0].value);
win.set("selected-color", colors[0].name);
sync();

win.on("add-task", (title) => {
  const clean = String(title || "").trim();
  if (!clean) {
    win.set("status", "Enter a task title first");
    return;
  }
  const owner = owners[nextOwner++ % owners.length];
  tasks = [{ title: clean, owner, done: false, priority: 6 }, ...tasks];
  win.set("draft", "");
  win.set("status", `Added "${clean}" for ${owner}`);
  sync();
});

win.on("toggle-task", (index) => {
  if (index < 0 || index >= tasks.length) {
    return;
  }
  tasks = tasks.map((task, i) => i === index ? { ...task, done: !task.done } : task);
  win.set("status", `${tasks[index].done ? "Completed" : "Reopened"}: ${tasks[index].title}`);
  sync();
});

win.on("select-section", (section) => {
  win.set("selected-section", section);
  win.set("status", `Showing ${section}`);
});

win.on("set-threshold", (value) => {
  win.set("threshold", value);
  win.set("status", `Risk threshold set to ${Math.round(value)}`);
});

win.on("set-notifications", (enabled) => {
  win.set("notifications", enabled);
  win.set("status", enabled ? "Notifications enabled" : "Notifications muted");
});

win.on("refresh", () => {
  const now = new Date().toLocaleTimeString();
  win.set("last-refresh", now);
  win.set("status", `Refreshed at ${now}`);
});

win.on("set-theme", (mode) => {
  const next = ["System", "Light", "Dark"].includes(mode) ? mode : "System";
  win.set("theme-mode", next);
  win.set("status", next === "System"
    ? `Theme follows the ${systemScheme} system color scheme`
    : `${next} theme selected`);
});

win.on("open-popup", (mode) => {
  win.set("popup-mode", mode);
  win.set("status", `${mode} popup opened`);
});

win.on("close-popup", () => {
  win.set("popup-mode", "");
  win.set("status", "Popup closed");
});

win.on("choose-file", (index) => {
  const file = files[index];
  if (!file) {
    return;
  }
  win.set("selected-file", `${file.name} (${file.path})`);
  win.set("popup-mode", "");
  win.set("status", `Selected ${file.name}`);
});

win.on("choose-color", (index) => {
  const color = colors[index];
  if (!color) {
    return;
  }
  win.set("accent", color.value);
  win.set("selected-color", color.name);
  win.set("popup-mode", "");
  win.set("status", `${color.name} accent selected`);
});

function reportDialogError(action, error) {
  const code = error && error.code ? error.code : "error";
  win.set("status", `${action} failed: ${code}`);
}

win.on("native-open-file", () => {
  win.set("status", "Opening native file dialog");
  openFileDialog({
    title: "Open a showcase file",
    filters: dialogFilters,
  }).then((path) => {
    if (path === null) {
      win.set("status", "Native open canceled");
      return;
    }
    win.set("selected-file", path);
    win.set("status", `Native open selected ${path}`);
  }).catch((error) => reportDialogError("Native open", error));
});

win.on("native-open-files", () => {
  win.set("status", "Opening native multi-file dialog");
  openFilesDialog({
    title: "Open multiple files",
    filters: dialogFilters,
  }).then((paths) => {
    if (paths === null) {
      win.set("status", "Native multi-open canceled");
      return;
    }
    win.set("selected-file", `${paths.length} native files selected`);
    win.set("status", paths.length ? paths.join(", ") : "No native files selected");
  }).catch((error) => reportDialogError("Native multi-open", error));
});

win.on("native-save-file", () => {
  win.set("status", "Opening native save dialog");
  saveFileDialog({
    title: "Save showcase export",
    defaultName: "wl2-showcase.json",
    filters: [{ name: "JSON", extensions: ["json"] }],
  }).then((path) => {
    if (path === null) {
      win.set("status", "Native save canceled");
      return;
    }
    win.set("selected-file", path);
    win.set("status", `Native save path ${path}`);
  }).catch((error) => reportDialogError("Native save", error));
});

win.on("native-pick-folder", () => {
  win.set("status", "Opening native folder dialog");
  pickFolderDialog({
    title: "Choose an output folder",
  }).then((path) => {
    if (path === null) {
      win.set("status", "Native folder pick canceled");
      return;
    }
    win.set("selected-file", path);
    win.set("status", `Native folder selected ${path}`);
  }).catch((error) => reportDialogError("Native folder", error));
});

win.on("selftest-tick", () => {
  win.invoke("add-task", "Self-test generated task");
  win.invoke("toggle-task", 0);
  win.invoke("select-section", "Settings");
  win.invoke("set-theme", "Dark");
  win.invoke("choose-color", 2);
  win.invoke("choose-file", 0);
  ui.quit();
});

const selftest = (wl2.runtime.argv || []).includes("--selftest");
win.set("selftest", selftest);

win.show();
syncSystemTheme();
await ui.run();

console.log(`wl2:slint showcase exited with ${win.get("tasks").length} tasks`);
}
