# App Install and Uninstall

`wl2 app install` is a source installer for examples and internal tools. It
clones a Git source, configures and builds it with CMake, installs the resulting
standalone executable into an app scope, and writes a launcher plus metadata.

```sh
wl2 app install /path/to/repo.git#v1.0.0:apps/hello --scope local
wl2 app list --scope all
wl2 app run hello
wl2 app uninstall hello --scope local
```

GitHub tree URLs are normalized to the same source shape:

```sh
wl2 app install https://github.com/example/tools/tree/v1.0.0/apps/hello --scope user
```

The installed metadata lives in `wl2.app.yml` (`schema: wl2.app.v1`) next to the
payload executable. Launchers are installed separately under each scope's
`bin/` directory.

## Scopes

| Scope    | App payloads and launchers                                    |
|----------|---------------------------------------------------------------|
| `local`  | `./.wl2/apps`                                                  |
| `user`   | `$XDG_DATA_HOME/wl2/apps` or the platform user data directory  |
| `system` | `/usr/local/share/wl2/apps`                                    |

Resolution order for `wl2 app run <name>` is `local`, then `user`, then
`system`. Passing an executable or launcher path runs that path directly before
name resolution. `wl2 app list --scope all` marks lower-priority apps as
`shadowed` when the same name is installed in an earlier scope.

## Uninstall

`wl2 app uninstall <name>` removes the app payload and launcher. It does not
remove shared modules installed under `.wl2/modules` or user/system module
scopes. Build caches under `.cache/<name>` are preserved by default:

```sh
wl2 app uninstall hello --scope local
wl2 app uninstall hello --scope local --purge-cache
```

When an app exists in more than one scope, pass `--scope` so the target is
unambiguous.

## Source Installer Limits

The source installer checks for Git, CMake, a compiler, and Ninja or Make before
writing app-scope payloads. The source project must be buildable with
`find_package(winglib2 CONFIG REQUIRED)` and should produce a standalone
executable. If Winglib2 is installed in a non-standard prefix, set
`WL2_PACKAGE_PREFIX` before installing so the app configure step can find it.

`wl2 app install` is meant for developer workflows, examples, and internal tools.
For non-technical users, prefer prebuilt standalone executables or platform
packages; they avoid requiring Git, CMake, a compiler, and local build
configuration on the user's machine.
