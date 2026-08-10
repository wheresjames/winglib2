# Security

Networking is denied by default. The native candidate-authorizer bridge checks
the original hostname and every resolved address through
`Runtime::authorizeNetworkConnect()` before `wlonvif` pins and connects to an
approved address. Native endpoint policy remains an independent check.

Credentials are converted during `connect()` into a native credential provider.
Operation leases are move-only and erase owned buffers on destruction on a
best-effort basis. Errors and module diagnostics never include credential values.

Redirects and unsafe TLS are unavailable. Connections use verified platform trust.
Continuous-movement cancellation, failure, session close, and runtime shutdown
use the native session movement ledger and bounded Stop cleanup.

Event subscription endpoints returned by a device remain subject to the same
candidate authorization and endpoint policy as the device service. Event queues
are bounded, reconnect continuity loss and dropped records are observable, and
topic filters are rejected until upstream can preserve their exact semantics.
