# Opt-in Guest/Host resource and rendering observations

This candidate extends the production Diagnostics sink from PR #19 (`aabd88a4`;
production code inherited from b25). It does not integrate PR #15's offline-only
watchdog or modify normal Loader behavior. Both Diagnostics and the separate
**Observe memory/render progress** setting default off. Select the `resource`
category when using an explicit category subset. Settings changes create a new
Session; an active owned Launch supplies its real identity, including Debug
builds where emulation shares the main Android process.

## What is actually observed

| Metric | Source and limitation |
| --- | --- |
| Guest policy cap | Existing `GetInfo` constant `0xF8000000` = 4,160,749,568 bytes = 3.875 GiB. This is static policy; the returned available value is `min(cap, heap region size)`. |
| Guest virtual Heap | Existing 39-bit region reserves `0x180000000` = 6 GiB virtual address range; this is not physical committed memory, Android RAM or an NVN buffer. The sampler records the actual `GetInfo(5)` return when requested; otherwise unavailable. |
| Guest Heap request/logical-size/error | Existing `SetHeapSize` inputs and successful logical processHeapSize assignment, with its existing InvalidSize returns counted. Logical size is not RSS. Other exceptions remain in the existing SVC error path. |
| Guest reported usage/system-resource limit | Reuses values already returned by `GetInfo(7/16/17)`; no background walk of memory mappings or guest thread objects. `GetInfo(9)` resource-limit handle is currently unimplemented and explicitly unavailable. |
| Guest thread progress | NCE and 32-bit JIT owning-thread SVC entry/normal-return counters (main ID 1 versus other threads), last observed SVC/thread. Exception counts cover the existing NCE non-normal catches only; 32-bit JIT propagation remains unchanged and is not counted as zero exceptions. This is API progress, not arbitrary CPU/Lua/world progress; ExitProcess/ExitThread need not return. |
| Driver FIFO | Latest successful GPFIFO allocation in entries, successful submitted entries/calls and pushbuffer words processed by the emulator. Not NVN command/control bytes, exact occupancy, remaining space or GPU completion. |
| Host GPU | Successful Vulkan command-buffer allocations, submit routine accepts/exceptions, the existing CycleWaiter `Wait(true)` wall duration/calls/errors. Separate entered/completed counts are independent lower bounds; disabled intervals can omit completions, so subtraction is not an exact in-flight count. Wall time includes host scheduling/dependency work and is not GPU execution time. No extra waits, retries, Poll calls or timeout changes. |
| Host VMA | Sum/count of successful existing `VmaAllocationInfo.size` outputs at four allocation sites. Cumulative, excludes imported buffers; not live memory, resident bytes or all driver allocations. |
| Host memory | Owned-launch worker reads current-process `Debug.MemoryInfo.getTotalPss` and bounded `/proc/self/status` VmRSS, KiB. Android API total PSS includes its swap-PSS accounting and is not pure resident PSS. Reads are non-atomic; begin/end/duration identify their interval. Not game-exclusive usage. |

Non-present counters are cumulative enabled observations within this process.
Gauges are process-level last observed API values and may predate the current
Launch; no unobserved zero or guaranteed per-launch origin is fabricated. Present
counters are separately scoped to Diagnostics-enabled observations of the
current Launch, independent of the resource toggle. Samples are independent
atomic snapshots, not an instantaneous transaction across threads.

NVN user command/control capacity, remaining bytes, growth requests and callback
outcomes are **unavailable**: there is no corresponding user API hook in the
current source. Vulkan budgets are **not queried**; no budget extension, allocator
flag or resource strategy is enabled by this observer. An NVN capacity/DrawElements
Abort is not evidence of memory exhaustion.

## Identity and comparison

Records retain the existing actual Session, Launch, Process run ID, title and
effective ExeFS identity. NSO headers contain a BuildID, but the existing Loader
does not retain it. This candidate respects the no-Loader-change boundary:
`guest_program_build_id=not_observed`, reason `existing_loader_does_not_retain_header`.
Neither NCA IDs, filenames nor the Android native ELF BuildID substitute for it.
A future narrow header observer is needed for actual loaded Program BuildID.

The optional `official` / `golden_mod` cohort is an explicit **user label**, not
content validation. It differentiates comparison runs without claiming their
files were authenticated or differ in a verified way. Use distinct Sessions and
keep the actual title/effective-content fields with both runs.

## Sampling, quiet episodes and persistence

Producer hooks only check atomics and update fixed counters. No formatting,
allocation, JSON, JNI or file IO is added to guest/render producer paths. Only
the already-existing fence wait is timed when the feature is enabled. Disabled
calls perform no clock read or allocation. Counter/gauge tests measure this
host path; no Android FPS result is claimed.

The ordinary diagnostic writer samples native counters about once per second.
A sample over 2 ms backs off to five seconds; allocation failure is isolated,
counted and backs off while the original sink continues. Host memory starts
with a ten-second delay, never catches up in a burst and backs off on reads over
100 ms or unavailable metrics, up to 60 seconds. A slow kernel query cannot be
safely forcibly interrupted; it stays on the single daemon, outside lifecycle
locks, and a stale Session/Launch is rejected at native admission.
An unexpected observer-daemon failure (including interruption/Error) is contained
inside that daemon and disables its epoch without logging, allocation or retry
in the catch path. A later explicit update can start a fresh delayed epoch; it
cannot leave a failed read permanently BUSY or become an Android uncaught crash.

A continuous, observable interval must include at least three new accepted
Present returns before a 30-second quiet episode is reported as
`RESOURCE_POST_PROGRESS_NO_NEW_PRESENT`. Unknown pause, explicit presentation
pause, stop/host-exit request, observed ExitProcess request and sampling gaps
suppress the classification or restart its observation interval. The existing
companion-static emulation thread is joined before prepareLaunch; if it remains
alive, the original activity exits that process. ExecuteLoadedContent and the
KProcess destructor kill/join old guest threads. The normal ExitProcess latch
relies on this existing serialized-launch invariant; it does not add scheduling
or joins. Recovery needs
a real accepted counter increment. These are observations, never declarations
of hang, OOM, NVN exhaustion, visible pixels or entry into a game world.

`resource-window.jsonl` stores up to 240 retained records, 128 KiB including
metadata and at most 60 seconds of **writer ingestion time**. Byte/count limits
may shorten that span. Metadata reports actual bounds and omissions; original
event timestamps remain untouched even when producers arrive out of timestamp
order. The writer saves about every five seconds and at observed exception,
quiet-episode, recovery and launch/session-end events. Raw signal handlers do
not allocate, serialize, lock, join or write this window. A fatal process can
only retain the last completed checkpoint; events since that checkpoint are
not guaranteed. Existing crash/signal/exit evidence is retained independently.

The window uses a fixed private filename, no-follow path/file checks, bounded
writes and atomic replacement. Failure preserves the last completed target,
counts a snapshot/window error and does not change game propagation. File fsync
is checked; full directory durability across power loss is not promised.

Only the fixed window name in the existing main/emulation/legacy scopes joins
the export allowlist. Export still sanitizes JSON/text, enforces its byte budget
and deduplicates existing Session/sequence identities across logs. Window
metadata describes the native pre-export checkpoint; archived duplicate records
may live in another included file. Keys, tickets, ROMs, saves, credentials and
recursive directory packing remain excluded.

## Reproduction and device acceptance

Host checks (no commercial content, emulator guest or device required):

```sh
python3 tools/tests/test-resource-model.py
python3 tools/tests/test-resource-window.py
python3 tools/tests/test-resource-observer.py
python3 tools/tests/test-resource-gpu-hooks.py
python3 tools/tests/test-resource-jit32-hook.py
python3 tools/tests/test-resource-stall.py
IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon clean
IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon \
  -PpineNativeDebugOptimization=2 -PpineSelectedContent=false testFullDebugUnitTest
```

The sink integration test uses an explicitly controlled resource-model clock;
it is not an APK run or a claim of waiting 30 real seconds. Production APKs do
not compile the host fault/clock hooks. Run full JVM checks for both Selected
flags when qualifying the final source, and retain exact commit/flags/results.

On an available authorized ARM64 device: enable Diagnostics, this resource
setting and the `resource` category; choose a cohort label, launch once, wait at
least ten seconds, then preview/export. Verify HOST_MEMORY_SAMPLE, native
resource samples, the real identifiers and the bounded window. Ordinary pause,
resume and stop must not become an exception. No repeated forest reproduction
is required. Capture a naturally occurring issue rather than intentionally
forcing an Abort on the owner's game.

Windows collection (already authorized USB or paired wireless ADB):

```powershell
.\tools\pine-diag.ps1 -ListDevices
.\tools\pine-diag.ps1 -Serial DEVICE_SERIAL -IncludeCrashLog
```

The script retains existing logs, saves UTF-8 JSONL and stops with Ctrl+C. Optional
SessionId/LaunchId/TitleId filters use the same event identity. The cloud has
x86_64, no KVM/DRI and no ADB target. Device APK execution, actual SAF provider,
real GPU budgets/driver behavior, game performance and commercial-game acceptance
remain unverified until an appropriate device is available.
