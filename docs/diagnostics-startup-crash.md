# Startup crash investigation and acceptance boundary

The developer preview's Settings/export tests do not establish game startup.
An immediate process exit before any game rendering remains an unresolved
runtime failure until a device crash report or a controlled reproduction
identifies it. These changes improve diagnostic coverage; they are not evidence
that a particular device's startup crash has been repaired.

The subsequently confirmed preview abort is covered by the separate
[mapped VMA allocation fix](diagnostics-vma-startup-fix.md). The startup file
opener also rejects missing/revoked/provider-failed Base/Update/DLC descriptors,
records `CONTENT_OPEN_FAILED`, closes partial descriptor sets, and returns from
the Activity without starting native emulation or killing the application.
This SAF failure is independent of the GPU allocation assertion.
Both AppItem launches and ACTION_VIEW metadata fallback reject unavailable
content before a new native launch. Missing/empty display-name queries and null
descriptors are explicit file failures. A rejected early Activity also skips
emulation-only pause/resume/PiP/clock cleanup. URI replacement is validated again
before launching; its associations are rebuilt without retaining old DLC URIs.

## Execution order

At preview source `e9de48c1`, `JNI_ENTERED` was emitted after JVM/settings,
text logger and Perfetto setup. The existing try block then installed host
signal handlers and constructed `OS`. `OS::state` constructed `DeviceState`,
which loaded Vulkan, constructed GPU, then SOC, audio and input. Only afterward
could `OS_INITIALIZED`, controller initialization and `OS::Execute` occur.

GPU device information is printed inside `CreateDevice`, before `vkCreateDevice`
returns and before queue, memory, scheduler, presentation, texture, buffer,
descriptor, helper shader/cache and debug-buffer construction completes.
Consequently the last Vulkan log is not proof that a driver caused a crash,
that OS construction succeeded, or that the game's executable was loaded.

Compared with main `f665512f`, the preview did not reorder these initializers.
The new observations retain that order, do not change driver selection, and
do not suppress or replace exceptions. `STAGE_STARTED` identifies an operation
entered, `STAGE_COMPLETED` identifies its return, and `last_stage` remains the
most recently completed stage. `INITIALIZATION_UNWOUND` is unclassified unwind
information, not a crash: SIGINT/ExitException cancellation must not be labeled
an error. The existing catches still determine error classification.

The new `JNI_ENTERED` is at the start of the JNI body; JVM/settings/logger/tracing
now have their own phases. Startup and error records use the existing bounded
asynchronous sink. A bounded Flush is attempted only on ordinary-thread error
unwind/catch paths. Raw signal handlers do not serialize JSON, allocate, lock,
call JNI, flush or join diagnostic threads.

## Sudden process death

The pre-existing host exception handler covers SIGINT/ILL/TRAP/BUS/FPE/SEGV,
but does not make all faults catchable. SIGABRT, terminate/noexcept failure,
SIGKILL, or a fault while reporting/unwinding may bypass the JNI catch and
normal drain. `SESSION_SUMMARY` therefore cannot be guaranteed after a fatal
exit. No new global signal handler changes the emulator's behavior.

After restart on Android 30+, diagnostics requests at most four historical
exit records for its own package using
[ActivityManager.getHistoricalProcessExitReasons](https://developer.android.com/reference/android/app/ActivityManager#getHistoricalProcessExitReasons(java.lang.String,%20int,%20int)).
It preserves Android's numeric reason/status/timestamp, without collecting
descriptions, process IDs, other packages, or tombstones. A historical exit's
game, commit and diagnostic session are explicitly unknown; it must not be
attributed to the new session. A signaled exit is not automatically called a
crash. Android's history is bounded and may be unavailable; absence is not
proof of a normal exit. Android 29 reports the API limitation.
The one-shot query runs on daemon background threads, with a 1500 ms observer
deadline and explicit unavailable result on timeout/read failure. UI startup,
game execution and diagnostic shutdown never join it. A Binder call may ignore
interruption; at most one daemon query can remain until it returns. Results
are discarded if their requesting diagnostic session has changed or stopped.

Export now distinguishes current-session status counters from per-session
maximum observed cumulative `dropped_count` values. Their total is a lower
bound across the captured sessions, not a sum of every row's running counter.
A missing summary alone does not distinguish a live session, crash, kill,
rotation or dropped records. A sink summary means the logging session drained;
it does not mean the game started or the process exited normally. Zero observed
errors is not a no-crash claim.

## Metadata scanning versus execution

`RomFile.populate` scopes NCA observations as `loader_source=metadata_scan`.
Those NCA candidates/final selections are library metadata work, not proof an
update program executed. Native `OS::Execute` retains its base/update/DLC
scopes. UI association events explicitly say `execution_confirmed=false`;
their Title ID is a bounded structured field and can be filtered before native
loading. `JNI_ENTERED` inherits the UI-provided association until OS loading
can report an actual loaded identity. Neither a missing independent update FD
nor a discovered Patch Meta NCA settles what a merged NSP will execute.

## Minimal Windows capture for an immediate startup exit

Use official Android SDK Platform-Tools and an already authorized device. Do
not clear existing logs, reinstall, clear application data, or switch drivers
just to collect this evidence. Start the updated collector **before** tapping
the game. Do not set TitleId or SessionId while investigating startup:

```powershell
.\tools\pine-diag.ps1 -ListDevices
.\tools\pine-diag.ps1 -Serial YOUR_DEVICE -IncludeCrashLog -OutputDirectory .\pine-startup
```

Reproduce once and press Ctrl+C. The same ADB stream reads main/crash buffers,
PineDiag, AndroidRuntime, DEBUG and libc; JSONL remains separate and unchanged.
The additional local `pine-crash-*.log` preserves non-JSON fatal output and
pre-title events, with a 32 MiB bound. It is not automatically redacted and may
include other applications' crash context: review privately before sharing.
The script never uploads it. For the already-published older collector, the
minimal separate crash-buffer snapshot immediately after reproducing is:

```powershell
adb -s YOUR_DEVICE logcat -b crash -d -v threadtime > .\pine-crash-buffer.txt
```

Reopen Pine and export its diagnostic snapshot so a new build can include
Android's historical exit reason. Preserve whether the previous Pine build
could launch the same lawful content; do not infer that comparison from logs.

## Required acceptance

- Host constructor tests preserve non-movable values, call the operation once,
  rethrow the original exception, and do no logging/file creation when disabled.
- A controlled child process really dies by SIGABRT after a flushed startup
  marker. The marker survives and no normal end/summary is invented. This is a
  host fault test, not Android or game execution or a guarantee of queue drain.
- Synthetic multi-session exports retain historical drop counts even when the
  current session has zero; record maxima do not double count cumulative drops.
- Windows PS5.1/pwsh fake ADB tests retain native/Java fatal text and unknown-title
  startup events alongside unchanged JSONL; existing cancellation tests apply.
- Device acceptance still requires the new exact APK: capture startup, identify
  the actual fatal signal/exception and stack or system exit reason, then verify
  normal Base/Update/DLC execution, storage read and orderly stop as applicable.
  A Settings-only run, successful build, or synthetic test cannot replace this.

No private diagnostic ZIP, device identity, keys, tickets, game content or save
data belongs in repository fixtures. Regressions here use controlled inputs only.
