# JavaScript Resources API {#resources_javascript}

The native `wl2::ResourceStore` is exposed to QuickJS scripts as
`wl2.resources`.

## API

```js
const text = wl2.resources.readText("wl2:/app/config.json");
const bytes = wl2.resources.readBytes("wl2:/app/logo.bin");
const stat = wl2.resources.stat("wl2:/app");

for (const entry of wl2.resources.list("wl2:/app")) {
  console.log(entry.path, entry.directory);
}

for (const entry of wl2.resources.walk("wl2:/app/assets")) {
  console.log(entry.path, entry.size);
}
```

Available functions:

- `exists(path)`
- `stat(path)`
- `list(path)`
- `walk(path)`
- `open(path)`
- `readText(path)`
- `readBytes(path)`

`readBytes()` returns a `wl2.Buffer`. `open()` returns a resource handle that
keeps the native resource view alive until the handle is closed or garbage
collected:

```js
const resource = wl2.resources.open("wl2:/app/data.bin");
try {
  console.log(resource.path, resource.size, resource.compressed);
  const bytes = resource.bytes();
  const view = resource.uint8Array();
} finally {
  resource.close();
}
```

Compressed resources use the same API as stored resources. The native handle
owns the lifetime of decompressed data while JavaScript reads from it.

## Development Maps

The `wl2` runner can map a host directory into the logical resource namespace so
JavaScript-only apps can be tested without rebuilding embedded resources:

```sh
wl2 run \
  --map-resource examples/js/resources/files:wl2:/resources \
  examples/js/resources/files/main.js
```

Mapped resources are visible through the same `wl2.resources` API:

```js
const config = JSON.parse(wl2.resources.readText("wl2:/resources/config.json"));
```

Use `--trace-resources` to print lookup hits and misses:

```sh
wl2 run \
  --trace-resources \
  --map-resource examples/js/resources/files:wl2:/resources \
  examples/js/resources/files/main.js
```

Resource maps can also be inspected without running the app:

```sh
wl2 config --map-resource examples/js/resources/files:wl2:/resources
wl2 resources list --map-resource examples/js/resources/files:wl2:/resources wl2:/resources
wl2 resources read --map-resource examples/js/resources/files:wl2:/resources wl2:/resources/config.json
wl2 resources extract --map-resource examples/js/resources/files:wl2:/resources wl2:/resources --out extracted
```

Mapped paths are confined to the mapped host root. `..` traversal and absolute
paths inside the logical resource path are rejected.

## Resource Manifests

Resource-only projects can use a `resources.yml` manifest with schema
`wl2.resources.v1`:

```yaml
schema: wl2.resources.v1
prefix: wl2:/resources
root: files
entry: main.js

resources:
  store:
    files:
      - main.js
      - config.json
      - empty.txt
  compress:
    files:
      - repeated.txt
  auto:
    directories:
      - assets

exclude:
  - "*.tmp"
```

Run the manifest directly:

```sh
wl2 run --manifest examples/js/resources/resources.yml
```

Inspect the expanded resource map:

```sh
wl2 config --manifest examples/js/resources/resources.yml
wl2 config --manifest examples/js/resources/resources.yml --json
wl2 resources list --manifest examples/js/resources/resources.yml
wl2 resources read --manifest examples/js/resources/resources.yml wl2:/resources/config.json
```

`wl2 config` is the main tracing command for the development loop. Human output
contains the manifest path/schema/entry, active engine, module requirements,
installed modules, resource maps, filesystem policy, dependency state, and
diagnostics. `--json` exposes the same stable sections for tooling.

During JavaScript/resource iteration, `wl2 run --manifest ... --watch` polls the
manifest, lockfile, mapped resource files, and script files and reruns the app
after changes. Native module libraries and module metadata are watched too, but
changes are reported as `rebuild-needed`; the watcher does not rebuild native
code.

If a generated JavaScript file fails and ends with a `//# sourceMappingURL=...`
comment, `wl2` remaps stack locations through that map when possible. External
map files and base64 JSON inline maps are supported. Missing or unsupported maps
fall back to the generated location.

Use the same manifest for embedded JavaScript executables:

```cmake
wl2_add_javascript_executable(wl2_resources_js
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources.yml)
```

Development-time manifest maps read source files directly from disk. Compression
policy is preserved in resource metadata, but files are not compressed before
being read by the development runner.
