# wl2:http showcase

Builds a single executable that starts a loopback HTTP server with:

- an HTML page at `/`
- a JSON endpoint at `/api/status`
- a POST echo endpoint at `/api/echo`
- a WebSocket echo endpoint at `/ws`

Run:

```sh
./build/bin/wl2_restinio_http_showcase_example
```

Then open `http://127.0.0.1:18081/`. Use `--port N` to choose a different port.
