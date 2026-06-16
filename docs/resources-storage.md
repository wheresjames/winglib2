# Resource Storage Model {#resources_storage}

The native resource storage model supports mixed stored and compressed embedded
files.

## Resource Handles

`wl2::ResourceStore::open()` returns a `wl2::ResourceHandle`. The handle owns the
lifetime of the read-only byte view:

```cpp
auto opened = runtime.resources().open("wl2:/app/config.json");
if (!opened) {
    return opened.error();
}

const void* raw = opened.value().data();
size_t size = opened.value().size();
```

For stored resources, the pointer refers directly to the immutable stored
resource bytes. For compressed resources, the pointer refers to an immutable
decompression cache entry. Repeated opens share the cached decompressed buffer
while it remains alive.

## Compression

The generator supports resource-set and per-file policies:

```cmake
wl2_add_embedded_resources(my_app
  ROOT ${CMAKE_CURRENT_SOURCE_DIR}/files
  PREFIX wl2:/app
  COMPRESSION AUTO
  STORE_FILES config.json
  COMPRESS_FILES repeated.txt)
```

The initial built-in compression backend is RLE. It is intentionally hidden
behind `wl2::ResourceCompression` and `ResourceHandle` so a later zstd/zlib
backend can replace it without changing resource consumers.

## Directory Embedding

Resource sets can embed whole directory trees recursively:

```cmake
wl2_add_embedded_resources(my_app
  ROOT ${CMAKE_CURRENT_SOURCE_DIR}/files
  PREFIX wl2:/app
  AUTO_DIRECTORIES assets
  EXCLUDE_PATTERNS "*.tmp"
  STORE_FILES config.json)
```

Directory paths are relative to `ROOT`, and embedded resource paths remain
relative to that same root. The generated manifest is sorted for deterministic
builds. `DIRECTORIES` follows the resource set `COMPRESSION` policy, while
`STORE_DIRECTORIES`, `COMPRESS_DIRECTORIES`, and `AUTO_DIRECTORIES` assign a
policy to files found under those trees. Explicit `STORE_FILES`,
`COMPRESS_FILES`, and `AUTO_FILES` entries override directory defaults.

## Tree Navigation

Resource paths can be explored like a read-only filesystem:

- `exists(path)`
- `isDirectory(path)`
- `list(path)`
- `walk(path)`
- `entry(path)`
- `open(path)`

Directories are synthetic and derived from resource paths.

## Example

`examples/resources` embeds one stored JSON file, one compressed text file, and
a nested asset directory. It demonstrates raw pointer access, decompression
cache reuse, directory listing, excludes, and nested resource lookup.
