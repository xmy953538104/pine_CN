# Signaled host exits and critical resource checkpoints

Fixed baseline: PR #21, `26c0abc7171ad6ab166d14d94bde3652e645fbfe`.
Monitoring remains off by default. Ordinary Loader, guest memory policy, GPU
allocation, signal dispatch, Kill, longjmp and thread cleanup are unchanged.

## Evidence and limits

The independently checked device exports contain resource windows with zero
observed persistence errors. The abnormal window retains 65 records, 129136 total
bytes and 35.339568 seconds: its byte budget was reached before the 60-second
age ceiling. Its last periodic checkpoint preceded Guest Break by about 2.58
seconds. A 60-second ceiling never promises a complete 60-second history.

The actual Guest Break was observed at local UTC+8 22:33:04.634, with termination
result 674 and a 17-frame Break stack. Android history reports reason 2/status 11
at 22:33:06.002, 1.368 seconds later. Resource/Present observations advanced in
between. That does not prove the guest main loop survived, identify the fatal
host instruction, or establish that both events have the same cause. The mixed
host/guest text printed while handling Break is not the final SIGSEGV stack.
PSS 3244204 / RSS 3475392 KiB does not establish RAM or NVN exhaustion.

These ZIP facts were independently verified by the parent task. This environment's
supported Library materialization failed with `download failed`; that route was
stopped without retry or alternate binary retrieval. The supplied 249-line text
log was readable through Library and confirms Break stack output, Kill(HOS-1),
then the main-thread all-thread kill/join request. No private path or raw user
log is committed here. This round does not claim to have reopened the ZIP bytes.

## P0: opportunistic host trace

The previous observer attempted a stream only for `REASON_CRASH_NATIVE` (5),
skipping the supplied `REASON_SIGNALED` (2)/status 11. It now also considers
reason 2 with fatal signal status 4, 6, 7, 8 or 11. SIGINT, SIGKILL, SIGTERM and
normal exit reasons are not promoted to native crashes. Fatal signal history is
reported separately as `PREVIOUS_PROCESS_FATAL_SIGNAL`, with host-process scope,
original Android exit time and the prior verified association. A status refinement
remains `PREVIOUS_PROCESS_EXIT_REFINED`, not a new crash.

[Android's API contract](https://developer.android.com/reference/android/app/ApplicationExitInfo#getTraceInputStream())
promises protobuf native tombstones from API 31 for reason 5, not reason 2.
It also documents potentially stale ANR traces and overwritten/null streams.
The [AOSP getter](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/app/ApplicationExitInfo.java)
uses the available retriever without testing the reason and may return null.
Thus the new reason-2 attempt is opportunistic, not a platform guarantee.

The production observer retains the existing own-UID/package/PID filters, four
history records, at most one read per process query and three persisted starts
per exit identity. API below 31, null, unsupported/error, deadline, non-protobuf,
partial/malformed and mismatched ownership remain explicit unavailable/partial
results. The streaming parser's 8 MiB scan, operation/time/field/frame bounds
and positive PID/UID/fault-TID/thread-map identity checks are unchanged. It never
exports raw tombstones, registers, memory, paths or abort messages. Only completed
allowlisted frames from the verified fault thread can be emitted.

`PREVIOUS_NATIVE_TRACE` retains 24 producer fields, including historical Title,
session/process/launch/commit, `reason_code`, `status`, `exit_wall_clock_ms` and
`trace_scope=host_process_candidate_not_guest`. `signal_number` comes from the
protobuf; it is never fabricated from the exit status. `wall_clock_ms` on the
row is the later observer time, not the original fault time. Constant raw-export
policy (always false) and attempt ceiling (always three) are documented here
instead of occupying fields. Critical-state metadata is separate; the legacy
compatibility `observed_event` makes 25 snapshot fields without dropping any
producer field. The real native writer/rotation fixture tests that exact shape.

The exporter now reports `historical_fatal_signal_observations` separately from
`historical_crash_observations` and current-session errors. It understands older
`PREVIOUS_PROCESS_EXIT` reason-2/status-11 rows, deduplicates valid exit IDs across
classification/refinement rows, and never counts trace/snapshot rows as new
host deaths. Zero imported crashes does not prove no fatal signal. The stable ID
still includes reason: an unproven platform change of the same exit from reason
2 to 5 would need explicit future correlation; this change does not invent one.

`GUEST_BREAK_REQUEST` is captured before the existing formatter. It proves only
the request: formatter return, Kill, longjmp and cleanup completion remain unknown
unless independently observed. The unchanged Break path formats a stack, requests
Kill(false) and then longjmps; the main OS path later requests all-thread join.
The existing deferred `GUEST_SIGNAL` now labels its scope as a handled guest-thread
signal and its timestamp as normal-thread drain after longjmp, not original fault
time or a final host exit. SIGINT stays excluded. No new work runs in raw handlers.
Existing raw-handler formatting, exception construction and cleanup hazards are
not repaired or asserted safe here; their role in this device exit is unknown.

## P1: bounded critical retention and asynchronous Break checkpoint

The existing writer owns both classes; guest/render producers gain no new
allocation, I/O, mutex wait, stack access or checkpoint API call. All rows keep
their original timestamp, identity and admission order. Limits remain 240 records,
129024 data bytes plus at most 2048 metadata bytes (128 KiB file), and 60 seconds
of writer-ingestion age. Critical records are limited to 16 rows / 32 KiB. Ordinary
rows borrow unused critical space; global pressure evicts ordinary rows first,
then critical pressure evicts the oldest critical row. Age and clock rollback
still apply to both. Per-class usage/capacity/eviction metadata exposes the loss.

Actual non-notification Break, exceptions, launch end and existing critical
resource events receive critical retention. Dense ordinary samples can still
span less than 60 seconds. A full critical budget, age, admission loss or sudden
termination can still remove a critical record; this is bounded priority, not
indefinite retention.

An admitted `GUEST_BREAK_REQUEST` with `notification_only=false` is itself the
asynchronous request. When the ordinary writer consumes it, it checkpoints the
resource window (`reason=guest_break_observed`) if observation/history is available,
then forces pending critical-state persistence. Notification-only and malformed
requests do not trigger it. Resource observation off from startup creates no
resource-window file; diagnostics fully off admits no request. The guest never
waits for this work and the original formatter/Kill/longjmp semantics are unchanged.
Two independently written files are not a transaction: no crash-atomic or
power-loss guarantee is made. Existing fixed persistence errors and recovery
records distinguish checkpoint failure from game I/O failure.

## P2: NVN boundary

The active HLE symbol-hook table has zero entries. There is no typed NVN client
command/control capacity, remaining-space, growth-request or callback hook.
Capabilities therefore continue to say `unavailable_no_user_api_hook`. Driver
FIFO capacity is an entry count; submitted entries and processed words have
different units and cumulative scopes. Host VMA bytes accumulate selected
successful allocations without subtracting frees or covering imported buffers.
None is NVN occupancy/free memory, live RAM usage, or proof of NVN OOM. No guest
buffer strategy, capacity, GPU behavior or game assets are changed.

## Offline reproduction

No commercial game, forced crash, keys or tickets are needed:

```sh
python3 tools/tests/test-break-checkpoint.py
PINE_DIAG_SANITIZE=1 python3 tools/tests/test-break-checkpoint.py
bash tools/tests/run-resource-window.sh
PINE_DIAG_SANITIZE=1 bash tools/tests/run-resource-window.sh
python3 tools/tests/test-resource-persist.py
python3 tools/tests/test-svc-break-observation.py
bash tools/tests/run-native-diagnostics.sh
./gradlew -PpineNativeDebugOptimization=2 -PpineSelectedContent=false testFullDebugUnitTest
./gradlew -PpineNativeDebugOptimization=2 -PpineSelectedContent=true testFullDebugUnitTest
```

Host native tests exercise real JSONL/critical/window files, a paused actual
writer, queue overflow and a real rejected temporary inode. JVM tests exercise
the production system-record observer/store with controlled streams, identity,
read/deadline faults and real ZIP export. Neither is Android APK/device execution.
The cloud is x86_64 with no `/dev/kvm` or `/dev/dri`; ARM64 app/game and real
Android reason-2 retriever behavior remain unrun. Existing PR21 artifacts remain
the rollback. Exact-source CI artifact identity belongs in the final handoff.
