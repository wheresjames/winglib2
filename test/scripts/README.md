# Winglib2 Script Tests

These scripts are run by CTest through the built `wl2` CLI and can also be run
manually during native binding development.

Module-specific scripts live with their modules. For example,
`modules/wl2_curl/test/js/wl2_curl_smoke.js` covers the native `wl2:curl`
module API:

```js
import { get, post, CurlClient } from "wl2:curl";
```

`wl2_buffer_smoke.js` covers the host `wl2.Buffer` API:

```js
const buffer = wl2.buffer.fromText("hello");
buffer.text();
buffer.uint8Array();
```

`wl2_membus_smoke.js` covers the native `wl2:membus` module:

```js
import { SharedQueue, KeyValueStore, Selector } from "wl2:membus";
```

The curl script performs GET requests against `https://example.com/` when run
directly without overrides. The default CTest path uses the compiled
`wl2_curl_fixture` helper, which starts a local HTTP server and supplies
overrides through:

- `WL2_CURL_TEST_URL`
- `WL2_CURL_TEST_POST_URL`

The local fixture covers deterministic GET, headers, redirect, POST echo, and
timeout behavior without public network access.

Run it through CTest with:

```sh
ctest --test-dir winglib2/build --output-on-failure -R scripts.wl2_buffer_smoke
ctest --test-dir winglib2/build --output-on-failure -R scripts.wl2_curl_smoke
ctest --test-dir winglib2/build --output-on-failure -R scripts.wl2_membus_smoke
```
