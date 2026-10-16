# Diagnostics v1 validation and owner checks

The authoritative binary identity is `build-identity.json` beside the Debug APK.
It contains the full source commit, APK/ARM64 ELF SHA-256 values, application ID,
ABI, JVM test totals and verification of the release emulation-process manifest.
Install **Pine Diagnostics** (`io.github.pine.emu.diagnostics`), version
`2.1.2-diagnostics-v1`, alongside your existing Pine installation.

## Automated evidence

| Check | Actual result at implementation review | Boundary |
| --- | --- | --- |
| Native core, concurrency, disk failure, bounds, UTF-8, rotation, retention, final summary | Passed host production-sink tests | Not game FPS validation |
| Two independent process sinks with concurrent rotation | Passed real fork/exec test | Android process lifecycle still needs runtime checks |
| Native ASan/UBSan and core ThreadSanitizer | Passed local review runs | Host tools, not an Android sanitizer APK |
| NSP selection/catch policy, production Backing/IStorage/PatchManager and BKTR/CTR/Region/OS compositions | Passed controlled host tests | Crypto keys, full NSP parsing and commercial files are not exercised |
| Android/JVM exporter, selection, process directory/session behavior and settings-search hierarchy | 21/21 passed actual Gradle task at `00811378` | Local JitPack proxy was unavailable; pinned upstream dependencies were rebuilt outside checkout for this supplemental run |
| Whole Android app/test Kotlin and Java compilation | Passed locally | Final exact-commit CI remains authoritative |
| Complete ARM64 native link and supplemental APK | Passed 983-step build, 229 changed-dependency objects, final static-STL link and APK packaging | The local supplemental native build uses debug compilation; the final exact-commit CI APK remains authoritative |
| Windows PS5.1/pwsh ADB | Full suite passed at `d859e3b5`, including native EOF, complete shared-file records and cancellation cleanup | Windows physical-keyboard Ctrl+C still needs manual acceptance; actual POSIX PTY Ctrl+C passed separately |
| Actual POSIX PTY Ctrl+C, fake ADB and native JSONL replay | Passed | These controlled inputs are not an Android/device run |
| Preview publication guards | Passed offline tests for rejected commits/jobs, tampered APK/evidence, wrong ABI and extra APK | Test ZIPs are deliberately non-executable fixtures |

Reproduce independent native checks without the Android SDK:

```sh
bash tools/tests/run-native-diagnostics.sh
bash tools/tests/run-content-diagnostics.sh
python3 tools/tests/test-preview-gates.py
```

Android build and JVM checks use Java17, Gradle8.6, NDK27.3.13750724, SDK35:

```sh
git submodule update --init --recursive --depth 1 --jobs 8
./gradlew testFullDebugUnitTest processFullReleaseMainManifest assembleFullDebug
```

The dedicated Diagnostics Debug workflow performs these checks and runs the ADB
suite in Windows PowerShell5.1 and pwsh. The `ci` PR label enables APK upload.

## Actual Android execution

See [environment evidence](diagnostics-android-runtime.md). This cloud host is
x86_64 with no `/dev/kvm` or graphics device. Official Android tools and an Android30
Google APIs image were installed; the image advertises ARM64 native translation.
The first software boot entered system_server watchdog restart loops before Pine
was installed. This is environment evidence, not an App failure. Runtime checks
continue using the same AVD without wiping it or changing guest security settings.

The source `00811378` supplemental APK passed signature, label/package/version,
all packaged ARM64 library headers, embedded DEX commit and release-process
manifest verification. Its SHA-256 is
`aea2cb73b5dfbca9f1341e8b964c34bd9e1735f01657377356f4e4445c4dd808`.
This is a local build with separately recorded flags and pinned-dependency
provenance, not the CI binary. The runtime report identifies which actual APK was
attempted; build success alone is not installation or UI evidence.

No physical ARM64, commercial-game or FPS/audio acceptance is claimed here.
The preview's annotated verification evidence/release notes record the final
attempted APK, runtime outcome and remaining limitations for that exact commit.

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

At `00811378`, the initial shared Linux host run (two CPUs, nice 15) measured median
off/normal/detailed call times of 12.33/93.66/172.90 ns. Across five million reads per
mode, accepted/written/dropped totals were 0/0/0, 26299/26299/12766 and
43639/43639/268861. Successful reads sample first then every 128 in normal mode,
every 16 in detailed mode. This tight in-memory loop saturates the bounded async
writer, especially in detailed mode; scheduling and drop rates affect timing.
These observations do not measure Android game FPS, audio, device storage or
ARM64 performance, and do not replace the repeated owner scene comparison above.
