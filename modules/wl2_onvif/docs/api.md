# `wl2:onvif` API

```js
import { connect, discover, localNetworks, OnvifError, version } from "wl2:onvif";
```

`connect(url, options)` returns a promise for a `DeviceSession`. The direct-client API accepts
automatic authentication, optional username/password credentials, a positive
`timeoutMs`, verified platform TLS trust, and an already-aborted signal. Explicit
authentication modes and custom TLS material reject with `onvif_unsupported`.

Sessions expose `getDeviceInformation()`, `getSystemDateAndTime()`, `getScopes()`,
`getServices()`, `getCapabilities()`, `media`, `ptz`, `events`, and `close()`.

The Media client exposes profiles, stream URIs, and snapshot URIs. The PTZ client
exposes capabilities, status, continuous/relative/absolute movement, Stop,
emergency Stop, presets, and preset navigation. PTZ vectors use normalized values
in `[-1, 1]`. `ptz.getCapabilities(profileToken)` returns legacy `pan`, `tilt`,
and `zoom` ranges plus `absolutePanTilt`, `absoluteZoom`, `relativePanTilt`,
`relativeZoom`, `continuousPanTilt`, and `continuousZoom` booleans. Use the
operation-specific booleans to enable controls; a legacy fallback range does not
prove that an operation is advertised.

`discover({ networks, maximumHosts, timeoutMs, signal })` performs a bounded
snapshot scan of explicitly supplied IPv4 CIDRs. It returns an async-iterable
`DiscoverySession`; each item contains the candidate address, authorized Device
service URL, evidence, safe diagnostic text, and WS-Discovery metadata when
available. `close()` discards unread results. Implicit interface enumeration and
continuous Hello/Bye observation remain unavailable.

`localNetworks()` returns the active, directly connected non-loopback IPv4
networks visible to the host. It requires the discovery UDP-bind permission and
is intended to populate an explicit user choice; calling it does not itself scan
or contact any address. New code should import the generic `localNetworks()`
from `wl2:uv`; this ONVIF export remains for compatibility.

## PullPoint events

`device.events.createPullPoint(options)` creates an explicit unfiltered
subscription. `options` accepts `topics: []`, `lifetimeMs`, `timeoutMs`, and an
`AbortSignal`. A subscription provides `pull({ timeoutMs, messageLimit, signal })`,
`renew({ lifetimeMs, timeoutMs, signal })`, `info()`, `state()`, and `close()`.
Event records contain `kind`, `topic`, `utcTime`, `source`, `data`, and `sequence`.

`device.events.subscribe(options)` creates a bounded managed stream. It accepts
`topics: []`, `reconnect: true`, `queueLimit`, `timeoutMs`, and `signal`, and can
be consumed with `for await`. Reconnect is always enabled. Besides normal event
records, iteration may yield `{ kind: "continuityLost" }` after reconnection or
`{ kind: "eventsDropped" }` when the bounded native queue overflows. Calling
`close()`, iterator `return()`, session close, runtime shutdown, or garbage
collection schedules deterministic native cleanup.

Non-empty topic filters, `getEventProperties()`, and `reconnect: false` reject
with `onvif_unsupported`.
