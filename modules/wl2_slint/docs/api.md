# wl2:slint API

The module is interpreter-based: `.slint` markup is compiled at runtime and a
reflective property/callback API is exposed to JavaScript.

## Exports

### `compile(source, options) -> Promise<Component>`

Compile `.slint` markup from a string. Resolves to a `Component` for the last
exported component in the source. Rejects with `slint_compile_failed` (carrying
`diagnostics`) on a markup error, or `slint_no_component` when nothing is
exported. `options` is reserved for future use.

### `compileFile(specifier, options) -> Promise<Component>`

Load markup from a `wl2:/` resource or a filesystem path (both subject to the
host resource/filesystem policy) and compile it. The component's `import`s
resolve relative to the markup: for a filesystem path, the file's directory; for
a `wl2:/` resource backed by a mounted host directory (e.g. `--map-resource`),
that directory. Purely embedded resources have no host directory, so a packaged
single-file UI works but its imports would also need to be embedded. Rejects with
`slint_invalid_argument` when the resource cannot be loaded.

### Native Dialog Functions

`openFileDialog(options) -> Promise<string|null>` opens a native file chooser.
`openFilesDialog(options) -> Promise<string[]|null>` opens a native multi-file
chooser. `saveFileDialog(options) -> Promise<string|null>` opens a native
save-file chooser. `pickFolderDialog(options) -> Promise<string|null>` opens a
native folder chooser.

All dialog calls require the UI capability and reject with
`slint_permission_denied` when denied. They resolve `null` when the user cancels.
If native dialogs were not built for the target, they reject with
`slint_unsupported`.

Dialog `options` is an object. Supported fields are `defaultPath`, `defaultName`
for save dialogs, and `filters: [{ name, extensions }]`, where `extensions` is
an array such as `["slint", "js"]` or a comma-separated string. `title`,
`acceptLabel`, and `cancelLabel` are accepted for forward compatibility but may
be ignored by the pinned nativefiledialog-extended backend.

## `Component`

### `create() -> Instance`

Instantiate the component. Instantiation never opens a window and needs no
capability.

### `run() -> Promise<void>`

Run the Slint event loop. Slint owns the loop and `run()` blocks the JS/main
thread until the last window closes or `quit()` is called, then resolves. While
the loop runs, a short recurring timer pumps the host async queue and pending JS
jobs on the UI thread, so other modules' promises keep settling and `await`s
resolve. **Requires the UI capability**; otherwise rejects with
`slint_permission_denied`. Rejects with `slint_invalid_argument` if a loop is
already running.

### `quit()`

Stop the event loop, causing the pending `run()` to resolve. A no-op if the loop
is not running.

## `Instance`

### `get(property) -> value`

Read an `in`/`in-out` property. Throws `slint_unknown_property` for an unknown
name.

### `set(property, value)`

Set an `in`/`in-out` property. Throws `slint_type_error` for an unsupported JS
value, or `slint_unknown_property` for an unknown name or a value the property
does not accept.

### `on(callback, fn)`

Register a JavaScript function as the handler for a component `callback`. The
handler receives the callback's arguments (marshaled to JS) and its return value
is marshaled back to Slint. Registering again for the same callback replaces the
handler. Throws `slint_unknown_callback` if the component has no such callback. A
throwing handler does not propagate into the event loop; it returns a void value.

### `invoke(callback, ...args) -> value`

Call a component callback synchronously, returning its value. Throws
`slint_unknown_callback` if the callback does not exist or the arguments do not
match, or `slint_type_error` for an unsupported argument value.

### `colorScheme() -> string`

Returns the active Slint widget style color scheme as `"unknown"`, `"dark"`, or
`"light"`. Applications with custom surfaces can use this for a System theme
mode that follows the backend style when it is available.

### `show()` / `hide()`

Open / close the component window. **Requires the UI capability** (`allowUi`),
which is denied by default; otherwise throws `slint_permission_denied`.

## Value marshaling

| JavaScript          | Slint                              | Direction |
| ------------------- | ---------------------------------- | --------- |
| `number`            | `Number` (f64; `int`/`float`)      | both      |
| `string`            | `string` (`SharedString`)          | both      |
| `boolean`           | `bool`                             | both      |
| plain object        | one-level `struct`                 | both      |
| array               | model (`[T]`); elements may be structs | both  |
| `"#rgb"`, `"#rrggbb"`, `"#rrggbbaa"` string | `brush` / `color` | JS -> Slint |
| `brush` / `color`   | `"#rrggbb"` / `"#rrggbbaa"` string | Slint -> JS |

Struct fields are scalars only (number/string/bool); deeper nesting in a struct
field raises `slint_type_error`. Arrays may contain structs or nested arrays.
Reading a `brush`/`color` property returns a CSS-style hex string (`#rrggbb`, or
`#rrggbbaa` when not fully opaque). Setting a `brush`/`color` property accepts
CSS-style hex color strings. Images remain a follow-up; unsupported conversions
raise `slint_type_error`.

## Error shape

Errors are `Error` objects with:

- `name`: `"SlintError"`
- `module`: `"wl2_slint"`
- `code`: one of `slint_unavailable`, `slint_compile_failed`,
  `slint_no_component`, `slint_unknown_property`, `slint_unknown_callback`,
  `slint_type_error`, `slint_permission_denied`, `slint_invalid_argument`,
  `slint_unsupported`
- `operation`: the API call that failed (e.g. `"compile"`, `"set"`, `"show"`)
- `message`: human-readable summary
- `component` / `property`: present when relevant
- `diagnostics`: array of `{ message, line, column }` for compile errors
- `cause`: underlying cause string when available
