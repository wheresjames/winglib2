# Executable Resource Inspection

Standalone executables built with `wl2_add_javascript_executable()` include a wl2
resource inspection table. The `wl2 resources` command can inspect that table
without running the executable:

```sh
wl2 resources list ./build/examples/js/resources/wl2_resources_js
wl2 resources read ./build/examples/js/resources/wl2_resources_js wl2:/resources/config.json
wl2 resources extract ./build/examples/js/resources/wl2_resources_js --out extracted
```

`list` prints each logical path, original size, stored compression mode, and
content hash. `read` writes the resource bytes to stdout. `extract` writes the
logical `wl2:/...` tree under the output directory.

Compressed resources are decompressed by default for `read` and `extract`, which
matches what JavaScript sees at runtime. Pass `--raw` to read or extract the
stored bytes exactly as embedded:

```sh
wl2 resources read --raw ./app wl2:/resources/repeated.txt > repeated.stored
wl2 resources extract --raw ./app --out raw-resources
```

Executables that do not contain a wl2 resource table fail with
`resource_table_not_found`.
