# Replay Lab Fixtures

Replay Lab now generates small deterministic AVI archives under `/tmp` with
`generateSyntheticFixture({ path })` during selftests and interactive runs.
Required fixture traits:

- timestamped names similar to archive security-video files
- burned-in frame clock
- configurable FPS, duration, and GOP length
- optional tone audio
- optional timestamp irregularities for diagnostics
