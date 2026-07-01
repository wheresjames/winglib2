# Winglib2 API Reference

Winglib2 is a small embeddable JavaScript runtime prototype with native
modules, embedded resources, copy-on-write buffers, and explicit thread-tree
message routing.

Start with:

- `wl2::Runtime` for embedding and running scripts.
- `wl2::Buffer` for in-process byte payloads.
- `wl2::ResourceStore` for embedded resources.
- `wl2::ModuleLoader` and `wl2::ModuleInfo` for native modules.
- `wl2::ThreadTree` for addressable mailbox routing.

Implementation notes:

- @ref foundations covers the shared membus/resources/thread-tree decisions.
- @ref membus_cpp covers the libmembus v2.1 C++ wrappers.
- @ref membus_javascript covers the JavaScript `wl2:membus` bindings.
- @ref resources_storage covers mixed stored/compressed resources, directory
  embedding, and tree navigation.
- @ref resources_javascript covers the JavaScript `wl2.resources` API.
- @ref thread_javascript covers the JavaScript `wl2.thread` API.
- @ref wl2_membus covers libmembus-backed IPC wrappers.
- @ref wl2_resources covers embedded resources.
- @ref wl2_threading covers thread-tree messaging.

Example:

```cpp
#include <wl2/wl2.h>

int main() {
    wl2::Runtime runtime;
    auto result = runtime.runModule("app.js");
    return result ? result.value() : 1;
}
```
