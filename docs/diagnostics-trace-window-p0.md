# Trace and resource-window P0 repair

This is a small follow-up to PR #20, fixed baseline
`cd7966c40a87b470e4feebf1ec5bb6a6b4c6540e`. Diagnostics and resource observation
remain opt-in. The ordinary Loader, guest 3.875 GiB policy cap, virtual heap,
Vulkan allocation strategy, scheduler and game resources are unchanged.

## Evidence and scope

The reported Android run has one native exit, first queried with status 0 and
later with status 6. Those are two observations of the same timestamp/reason/PID
and process, not two crashes. Resource-window failures rose to 35 in that process
and to 62 in a different subsequent process. The export lacked a resource-window
member, and both imported native-trace attempts reported `over_byte_limit` with
zero frames. The later normal-run emulation log is not a host tombstone.

The old counters contain no stage or errno. They cannot identify whether the
historical device failure occurred while opening, writing, checking size or
renaming, nor independently prove that export was fault-free. Main JSONL
`disk_errors=0` does not establish successful optional checkpoint persistence.
The changes below repair reproducible source defects and make a future naturally
occurring failure distinguishable; they do not establish that original device's
unique cause or a game's memory requirements.

## Actual changes

The old native writer opened every directory ancestor with `O_RDONLY`. A real
UID-1000 test reproduced `EACCES` with a search-only ancestor: opening the private
leaf worked, but persistence failed. Linux/Android traversal now uses `O_PATH`
(or `O_SEARCH` where supported) with `O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC` and the
existing leaf ownership checks. Symlink, special-file, hardlink and changed-inode
rejections remain enforced. The same non-root reproduction now writes a complete
window. This is a demonstrated defect and a candidate explanation, rather than
confirmed historical Android errno.

Every persistence failure now has fixed stage/reason/errno details. The existing
writer emits the first failure and at most one per minute until recovery. Old
completed files survive write/sync/close/rename failures, stale own temporary
files can recover, and one successful later checkpoint emits a recovery record.
There is no new file sampling or producer/signal-handler I/O.
At normal drain, producers have already closed; final checkpoint status and fixed
last-failure codes are written directly into the existing Session summary. A
terminal-only failure or recovery therefore does not depend on admitting a new
event into the closed queue. Sudden death can still prevent that summary.

The old trace reader rejected the whole stream above 256 KiB before parsing any
summary. The new parser streams past bounded unknown fields. It preserves only
completed allowlisted frames after matching positive PID/UID/fault TID/map key/
`Thread.id`, with explicit complete/partial/unavailable status. Buffer, candidate
text, frames, bytes scanned, operations and parsing time are all bounded. Raw
tombstones, maps, memory, registers, command lines, paths and abort text remain
excluded. A blocked platform call can still yield `observer_deadline`; no trace
is manufactured when Android no longer has it.

Stable exit IDs exclude mutable Android status. Refinements are informational,
and a bounded migration bridge links already-written legacy 0/6 observations
without emitting a third crash or resetting the trace-read budget.
Already-written legacy rows retain their original identities and status. The
export's exact-ID observation counter can therefore include both legacy aliases;
the new bridge makes their correlation explicit, rather than claiming that the
old rows represent distinct crashes.

## SvcBreak interpretation

`GUEST_BREAK_REQUEST` records the actual SVC request before the existing stack
formatter. The notification bit and termination-request branch are explicit.
This separates a guest request, a formatter exception and a later Android host
exit. The observed `Span size not aligned ... /0x18` is an emulator C++ exception.
In the unchanged Loader source, `ResolveSymbol` reads `executable->symbols`
before checking `executable != executables.end()`, and an outer SVC exception
path can attempt another stack format. These are source-level failure candidates;
without a usable host stack they do not establish the cause of SIGABRT. This P0
does not change that ordinary Loader behavior or infer NVN/RAM exhaustion.

## Reproduction and verification

No commercial content, keys or tickets are needed:

```sh
bash tools/tests/run-resource-window.sh
PINE_DIAG_SANITIZE=1 bash tools/tests/run-resource-window.sh
python3 tools/tests/test-resource-persist.py
PINE_DIAG_SANITIZE=1 python3 tools/tests/test-resource-persist.py
python3 tools/tests/test-svc-break-observation.py
./gradlew -PpineNativeDebugOptimization=2 testFullDebugUnitTest
```

The window tests use actual files, search-only and denied permissions,
short writes/EINTR, ENOSPC, sync/close/rename errors, inode substitution and
recovery. The real `diagnostics.cpp` integration verifies eight failed
checkpoints, one rate-limited privacy-safe report, preservation, exact per-writer
counter, successful recovery, terminal-only failure/recovery details and
resource-off absence. Eight quick failures verify suppression within the first
minute; this test does not claim a real-time 60-second re-report observation.
The SVC test compiles
the production Break function against controlled state and compares its unchanged
flow with the fixed baseline, including real `setjmp`/`longjmp` and unchanged
formatter exceptions.

`ResourceWindowExportTest` runs the actual production POSIX writer through a
host-only JNI fixture, captures real files with `DiagnosticExport`, creates a
real ZIP, parses its rows and checks redaction. It covers search-only ancestors,
write/rename failures, recovery and stale temporary files. Trace tests cover
large protobufs, partial reads and limits, invalid encoding/tags, thread identity
conflicts and persistent legacy migration. These are host/JVM tests, not an APK
or physical Android execution claim.

On an authorized device, ordinary use is sufficient: enable Diagnostics and the
resource category/observer, preview and export, then inspect the fixed
`main/resource-window.jsonl` or `emulation/resource-window.jsonl` when available.
The metadata should describe the bounded retained window, and errors use the
codes above. Existing historical exits can be imported without deliberately
crashing or repeatedly entering the forest. Future traces may be complete,
partial or unavailable depending on Android's retained data. Do not require an
intentional crash for this repair's acceptance.

## Rollback

The baseline [O2 Debug run](https://github.com/xmy953538104/pine_CN/actions/runs/39368200193)
and [APK artifact](https://github.com/xmy953538104/pine_CN/actions/runs/39368200193/artifacts/12240406523)
remain available. Its APK SHA-256 is
`abdf871abae9417f726f99bd33ad57a63fe9c53e426a5cf45e2a6578f82ce8fa`,
certificate SHA-256
`70edcdc802bb16afe9665ecd3d342ea74b217a79b6efb4dbdfbb3ea3eddf16c1`.
The new draft PR records its exact source, terminal CI and separately verified
Debug artifact. No merge or formal release is authorized by this task.
