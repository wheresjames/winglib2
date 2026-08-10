# Security

Interface enumeration reveals local addresses and topology. Until Winglib2 has a
dedicated network-inspection capability, both exported functions conservatively
require authorization to listen on `0.0.0.0:0`. No socket is opened.

For command-line use, grant the temporary capability with:

```text
--allow-listen --listen-allow=0.0.0.0:0
```

The recommended follow-up is a boolean `networkInspection` permission carried
through runtime options, script declarations, interactive approval, and the
trust store. That change should be made centrally; the module must not infer
permission from unrelated filesystem, outbound-network, or UI grants.

The default excludes internal interfaces. Callers must explicitly request them.
No DNS queries, connections, binds, or packets are performed.
