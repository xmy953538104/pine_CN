# Diagnostics v1 validation and owner checks

The authoritative binary identity is `build-identity.json` beside the Debug APK.
It contains the full source commit, APK/ARM64 ELF SHA-256 values, application ID,
ABI, JVM test totals and verification of the release emulation-process manifest.
Install **Pine Diagnostics Preview** (`io.github.pine.emu.diagnostics.preview`), version
`2.1.2-diagnostics-v1`, alongside existing Pine and old diagnostics installations.
The new ID avoids overwriting the differently signed e9 Debug app. Old data is
not cleared or migrated; select local content/grants in the new preview. A
stable development signer for continuous Debug updates is not configured yet.

The current startup fixes are described in [the VMA regression](diagnostics-vma-startup-fix.md)
and [safe URI/startup checks](diagnostics-startup-crash.md). The latest exact-commit JVM totals, build identity and runtime result are recorded
in the draft PR and matching artifacts; older totals below are historical evidence.

## Independent review corrections

The current candidate also tests retryable historical trace failures followed by
success across process starts, success-preserving export deduplication, and an
actual 16-field native trace surviving ordinary-log rotation into a snapshot and
export. The retry limit is three persisted attempts, not an unbounded loop.

Controlled host JNI tests use distinct CNMT/Program/NACP save-owner IDs through
the real setters, RomFile initialization and UI field mapping. No-Meta resolution
uses Program rather than a shared save owner, and a generation-owned native
callback updates the exact process-summary codec. Old untyped PDX1 titles become
unknown. The original AppEntry serialization and content selection remain intact.
Activity tests separately execute destruction followed by the production native
return/finally path and settings reconfiguration, and reject stale generation A's
attempt to clear B. These are JVM/JNI/host regressions, not device execution.

Debug shares the main process and can reconfigure its live sink. Release uses a
separate emulation process and observes settings at its next start; no live
cross-process settings IPC is implemented.

## Automated evidence

| Check | Actual result at implementation review | Boundary |
| --- | --- | --- |
| Native core, concurrency, disk failure, bounds, UTF-8, rotation, retention, final summary and active-game identity across off/live reconfiguration | Passed host production-sink tests; new identity regression fails unchanged old implementation and passes all 7 corrected lifecycle scenarios | Not game FPS validation |
| Two independent process sinks with concurrent rotation | Passed real fork/exec test | Android process lifecycle still needs runtime checks |
| Native ASan/UBSan and core ThreadSanitizer | Passed local review runs | Host tools, not an Android sanitizer APK |
| NSP selection/catch policy, production Backing/IStorage/PatchManager and BKTR/CTR/Region/OS compositions | Passed controlled host tests | Crypto keys, full NSP parsing and commercial files are not exercised |
| Android/JVM exporter, selection, process/settings and game-generation ownership | 43/43 passed the full local Gradle task: 17 exporter, 4 selection, 3 process/settings, 13 generation and 6 real Activity wiring cases. Previous `538ba8af` CI's 34 tests did not exercise actual Activity destruction | Robolectric substitutes JNI boundaries and records navigation/kill; it is not an APK/game run. Local dependency/native classpath helpers are supplemental; exact-commit CI remains authoritative |
| Whole Android app/test Kotlin and Java compilation | Passed locally | Final exact-commit CI remains authoritative |
| Complete ARM64 native link and supplemental APK | Passed 983-step build, 229 changed-dependency objects, final static-STL link and APK packaging | The local supplemental native build uses debug compilation; the final exact-commit CI APK remains authoritative |
| Windows PS5.1/pwsh ADB | Full suite passed at `86ae1d47`, including native EOF, complete shared-file records and a real Win32 CTRL_C_EVENT in an isolated console; self-exit 47/62 ms, no failure cleanup | Actual USB/wireless devices and physical keyboard mapping remain manual acceptance; actual POSIX PTY Ctrl+C passed separately |
| Actual POSIX PTY Ctrl+C, fake ADB and native JSONL replay | Passed | These controlled inputs are not an Android/device run |
| Preview publication guards | Passed offline tests for rejected commits/jobs, tampered APK/evidence, wrong ABI and extra APK | Test ZIPs are deliberately non-executable fixtures |
| Android observer safeguards | CI at `86ae1d47` passed 31 smoke, 4 passive first-boot and 11 startup-probe tests; the subsequent cleanup/SAF observer correction passed 40 smoke tests locally using controlled subprocesses | They pass none of the 18 actual APK acceptance checks; current exact-commit CI is authoritative |

Reproduce independent native checks without the Android SDK:

```sh
bash tools/tests/run-native-diagnostics.sh
bash tools/tests/run-content-diagnostics.sh
python3 tools/tests/test-preview-gates.py
python3 tools/tests/test-android-smoke-deadlines.py
python3 tools/tests/test-android-first-boot.py
python3 tools/tests/test-android-startup-probe.py
```

Android build and JVM checks use Java17, Gradle8.6, NDK27.3.13750724, SDK35:

```sh
git submodule update --init --recursive --depth 1 --jobs 8
./gradlew testFullDebugUnitTest processFullReleaseMainManifest assembleFullDebug
```

The dedicated Diagnostics Debug workflow performs these checks and runs the ADB
suite in Windows PowerShell5.1 and pwsh. The `ci` PR label enables APK upload.

`EmulationActivityCompletionTest` invokes the real attached Activity's
`onDestroy` and public completion entry. It initializes AndroidX saved-state and
fragment prerequisites without starting a ROM; only the native stop/drain
boundaries are substituted, and Robolectric records navigation and process-kill
calls. It distinguishes destruction before completion acceptance from an already
accepted ending that must still drain and exit. Helper cancellation tests remain
separate and do not stand in for Activity destruction. This JVM wiring evidence
does not demonstrate real native game shutdown.

The initial three-case run against `538ba8af` lifecycle/acceptance logic plus the
neutral JNI-stop test seam produced two assertion failures: delayed completion
after destruction and completion called synchronously by destruction's native
stop both accepted a new ending. The already-accepted-before-destruction case
passed. AndroidX test setup failures were resolved before recording that red
baseline and are not counted as reproductions of the defect.

```sh
./gradlew testFullDebugUnitTest --tests emu.skyline.emulation.EmulationActivityCompletionTest
```

## Actual Android execution

The [`538ba8af` independent Intel Mac run](https://github.com/xmy953538104/pine_CN/actions/runs/38984441717/job/117252508085)
passed **18/18 actual Settings/log/export/Stop checks** against APK SHA-256
`29a1b5e7c7c460df959c0073610f1b73263193d3b124335d91b2b8f428792fda`.
Its [runtime evidence artifact](https://github.com/xmy953538104/pine_CN/actions/runs/38984441717/artifacts/12083510739)
records the exact App/observer checkout separately from the retry workflow.
Actual ARM64 native JSONL and a DocumentsUI-saved ZIP with matching preview bytes
were verified on the official API35 x86_64 Google APIs image/native bridge.
No Wait/Close/Back recovery was used. The original same-APK Mac and Ubuntu runs
remain 5 passed / 13 blocked by system ANRs; their reports were not replaced.
The successful retry does not erase historical real Pine startup ANRs.

That APK predates the separate destruction-first Activity fix and is a superseded
review candidate. Those 18 checks do not exercise native game completion or
Activity destruction order. Build and runtime evidence for the replacement APK
must carry its own exact source and hash; consult the draft PR's current result.

The [`69826dda` Intel Mac attempt](https://github.com/xmy953538104/pine_CN/actions/runs/38964404733/job/117189621941)
installed and hash-verified the actual ARM64 APK on an official API35 x86_64
image with native translation. Settings, Diagnostics, default off, enable/session
and native-active observations passed. Four actual native `PineDiag` JSONL rows
also passed supplemental checks, including exact-commit metadata and completed
session summaries. Its original report remains 8 passed / 10 blocked: it did not
verify local JSONL bytes or export. Concrete directory-listing and rendered-label
observer defects were subsequently fixed and tested. The matching latest APK
runtime report determines whether those remaining flows have passed.

The subsequent [`6fe018a7` Intel Mac attempt](https://github.com/xmy953538104/pine_CN/actions/runs/38975708728/job/117226034703)
passed **15/18 actual checks**, including native JSONL file bytes, PineDiag,
level/retention/categories, the real export preview and its three private snapshot
files. SAF reached Downloads but its filename observation timed out; no saved ZIP
or clean Stop-summary pass was obtained. The matching Ubuntu run has a real
supplemental Pine startup ANR from DropBox, even though its original UI-based
report says 5 passed / 13 blocked. Its main thread was sampled in native translation
inside `System.loadLibrary`, before diagnostic setup. Original reports are not
rewritten. Both APKs predate the independently reviewed active-title/generation
fixes and are superseded review builds, not final candidates. New exact-commit
build and runtime evidence is required for the corrected App.

Readback of that retained SAF evidence showed the original app-provided filename
was valid, but synthetic delete/type commands left only `pine-diag-s`. The observer
now preserves and verifies the exact current-session default filename in the
official DocumentsUI provider before pressing Save. Cleanup can recognize an
already visible own Diagnostics screen after launching Settings; initial Settings
entry still requires the real preference path. Actual saved ZIP bytes, matching
snapshot contents and a native Stop summary remain required for those checks.

The [same-AVD main comparison](https://github.com/xmy953538104/pine_CN/actions/runs/38968004469/job/117206287625)
installed and hash-verified tracked-clean main `f665512f`, but its Settings launch
exceeded the 120-second observer deadline. The subsequent target `199bbccd`
reached Diagnostics/default-off/enable (8 passed / 10 blocked with the older
directory observer). This does not isolate a Diagnostics regression or establish
baseline Settings success; baseline-first order and shared system warming remain
confounds. Earlier failed/blocked reports are preserved below and in the runtime
evidence document.

See [environment evidence](diagnostics-android-runtime.md). This cloud host is
x86_64 with no `/dev/kvm` or graphics device. Official Android30 and Android34
Google APIs images advertise ARM64 native translation. Local installation attempts
failed during documented Android framework failures. A separate CI Android34 run
successfully installed the exact ARM64 APK and verified its hash, then the Pine
process crashed: `ndk_translation` rejected `0xd53be008` (`MRS CNTFRQ_EL0`).
The unchanged baseline clock initializer uses that instruction during library
loading. The runtime report remains **failed**; Settings, native logging and
export are not runtime passes. Later attempts have separate per-APK reports.

The source `00811378` supplemental APK passed signature, label/package/version,
all packaged ARM64 library headers, embedded DEX commit and release-process
manifest verification. Its SHA-256 is
`aea2cb73b5dfbca9f1341e8b964c34bd9e1735f01657377356f4e4445c4dd808`.
This is a local build with separately recorded flags and pinned-dependency
provenance, not the CI binary. The runtime report identifies which actual APK was
attempted; build success alone is not installation or UI evidence.

No physical ARM64, commercial-game or FPS/audio acceptance is claimed here.
Use the matching CI artifact and runtime report. Any published preview must carry
annotated verification evidence with the attempted APK, runtime outcome and
remaining limitations for that exact commit.

## Owner reproduction

1. Install the supplied Debug APK. Check its SHA-256 against `build-identity.json`.
   Existing Pine remains a separate app and keeps its own files.
2. Open Settings → Developer diagnostics. Confirm diagnostics starts off. Enable it
   and note the session, native active flag, written/dropped counts and disk errors.
   Ordinary and detailed modes sample successful DLC storage attempts every128
   and16 respectively; known short reads and exceptions are not sampled.
3. Change retention/categories. Each process owns a fixed directory and session;
   lifecycle events remain available. Release emulation uses `:emulationProcess`,
   and changed settings apply at its next process start. The screen labels current
   process statistics and the other scope's last observed session separately.
4. Choose Preview and export. Review the exact bounded snapshot list, then select a
   destination in Android's document picker. The ZIP must contain only metadata,
   fixed JSONL filenames in `main/`, `emulation/` or the legacy root, and optionally
   redacted `emulation.log`. It never recursively packages app storage.
   Recognized private locations are redacted through the line end, including quoted
   filenames with spaces; preceding context and independent JSON fields survive,
   but later same-line detail is omitted.
5. Parse each exported JSONL line. Invalid, oversized, incomplete or duplicate
   records are omitted with counts in metadata. Snapshots are live and can omit
   concurrent rotation/queued records. Exported logs are capped40MiB plus1MiB
   metadata; two cached snapshots at most. Guest text can still contain personal
   prose, so inspect a package before sharing it.
6. On Windows with authorized USB or paired wireless ADB, follow
   [the collector instructions](../tools/README-diagnostics.md). Example:

   ```powershell
   .\tools\pine-diag.ps1 -ListDevices
   .\tools\pine-diag.ps1 -Serial YOUR_DEVICE -OutputDirectory .\pine-diag
   .\tools\pine-diag.ps1 -Serial YOUR_DEVICE -TitleId 0100751007ADA000
   ```

   Ctrl+C should stop streaming, flush local UTF-8 files and reap the collector's
   ADB child. Existing device logs are not cleared. Follow your computer's normal
   script policy; the tool does not change it or save ADB authorization material.
7. Use your own lawful Base/Update/DLC. Verify actual `PATCH_SELECTED`, NCA candidate/
   overwrite/final choices, CTR/BKTR section branch, `PATCH_ROMFS_OPENED`, DLC
   registration, Data ID match/open and observed read events. BKTR byte counts
   marked `unknown` are not successful-byte evidence. Report failures with the ZIP
   and exact APK commit; never send ROMs, keys, tickets or save contents.
8. DFX events require the game to output the exact known marker through a real
   guest log interface. Missing Lua messages/locations cannot be reconstructed by
   scanning memory or inventing a stack. Compare the same game/save/scene for
   FPS/audio with diagnostics off, ordinary and detailed modes; repeat each run.

Normal emulation return drains/stops the sink on an ordinary thread before the
existing immediate process exit. Raw handlers only store a signal number for later
serialization. Forced kills, power loss or fatal paths that cannot return safely
can still lose queued events. A session summary confirms a completed drain; its
absence must not be presented as a clean shutdown.

## Optional host storage benchmark

Run `python3 tools/tests/benchmark-storage-diagnostics.py` with a C++20 host
compiler, or add `--output /path/to/new-directory` to retain evidence at a chosen
fresh path. The runner compiles production `IStorage.cpp`, `Backing` and the real
diagnostic sink with existing IPC/platform host stand-ins. Each mode makes the
same one million controlled 64-byte reads per trial in five fresh-process trials,
rotating mode order. It checks identical backing calls/checksum, sampling ordinals, valid
JSON, accepted plus dropped attempts, complete draining and zero disk errors;
timings have no pass/fail threshold. `result.json` records all trials, build
command, source hashes, host settings and accepted/written/dropped counts.

At `5e450f19`, the shared Linux host run (two CPUs, nice 15) measured median
off/normal/detailed call times of 11.47/94.69/296.21 ns. Across five million reads per
mode, accepted/written/dropped totals were 0/0/0, 39065/39065/0 and
148946/148946/163554. Successful reads sample first then every 128 in normal mode,
every 16 in detailed mode. This tight in-memory loop saturates the bounded async
writer, especially in detailed mode; scheduling and drop rates affect timing.
These observations do not measure Android game FPS, audio, device storage or
ARM64 performance, and do not replace the repeated owner scene comparison above.

The Java-failure cleanup regressions additionally run the production wrapper on
an actual JVM thread and let the JVM invoke the installed uncaught handler.
They verify pre-cleanup identity/flush ordering, same Throwable and controlled
stack frames, no launch resurrection on settings apply, A's native identity
observed during execution surviving replacement B, unverified early UI hints,
and sink-flush failure preserving propagation and cleanup. JNI side effects are
shadowed in these Activity tests; they are not Android APK execution.
