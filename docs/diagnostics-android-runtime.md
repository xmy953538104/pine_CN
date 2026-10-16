# Android runtime test: cloud execution evidence

Local checkpoint: **2026-10-16 18:01 UTC — the exact supplemental `00811378` APK remains runtime blocked.** Its packaged ARM64/commit identity passed. Both official Android 30 installation methods and the one actual Android 34 installation attempt failed at the package service. A successful final Android 34 package listing confirmed the target absent. This local APK therefore has no observed installed-APK hash, Settings launch, Diagnostics UI, native JSONL, export, or Stop-summary pass. Unobserved checks remain blocked.

These claims apply only to the identified local APK and recorded attempts. For every later CI APK, its matching `runtime-report.json` and preview runtime annotation are authoritative; this historical checkpoint does not qualify or override a later artifact's results.

A separate later **CI Android 34** run for `bdbe5036b480e568f95a95c5236f33abf1a5def2` successfully installed its own exact CI APK and verified the installed SHA256. It then blocked at the Settings UI observation and **failed** its app-crash gate with a real SIGILL. The matching report/crash evidence are in CI run `38939148938`, job `117104977029`, artifact `12063203454`. That CI APK is distinct from the supplemental local APK below, and its original failed report remains unchanged.

A further readback (CI run `38943813038`, job `117117783108`) recovered the precise same-process sequence: at 17:45:54.595, `ndk_translation` reported `Undefined instruction 0xd53be008 at 0x00007ae118375b08` for Pine PID 4111; 21 ms later that PID received SIGILL. The opcode decodes to `MRS x8, CNTFRQ_EL0`; inspection of the verified official bridge confirms its decoder rejects that selector. This establishes a bridge failure on the observed instruction, not a successful APK run. The baseline `common/utils.h` is byte-identical to `origin/main` commit `f665512f52fa6c3b7e726299d0c23ff3abe09d8c` (blob `69405941cbe477ec13d98cc2b5de8442e92e6e49`, recorded in `baseline-clock-file-identity.json`). It initializes global `util::ClockFrequency` by reading `CNTFRQ_EL0`, and `SkylineApplication.onCreate` loads `skyline` before calling the new `Diagnostics.apply`. That is a strong inference for the caller; complete guest-PC-to-library symbol mapping and an actual unmodified baseline APK comparison were not obtained. No clock or guest execution logic was changed to accommodate x86 translation. The failed app-crash gate, unobserved Diagnostics UI, and unrun physical ARM64/NCE/game checks remain explicit.

The newer official **Android36 Google APIs x86_64 r7/ext17** image provides a
different bridge. Its downloaded ZIP matched Google's manifest SHA1
`c6bf44bdcd885bb902b4ba752d111a073ad7a817`; the extracted bridge SHA256 is
`fbadc774c989534a567e6af8fd16d2c00727b1f1d9cc778bf538d6b59ed9776d`.
All three inspected decoder engines explicitly support `CNTFRQ_EL0`, supplying
10,485,760 Hz. This removes the specific static rejection seen on Android30/34,
and justified attempting the unchanged ARM64 APK on Android36. Static instruction
support is not an installation or UI pass. Each exact runtime report is
authoritative. Image/CRC/ELF provenance is recorded under
`/workspace/android-build/newer-bridge-evaluation`; independent instruction and
decoder evidence is under `/workspace/android-build/evidence/cntfrq-translation`.

**CI Android36 checkpoint, 2026-10-16 18:41 UTC:** source
`bf34b9afb5c0e86c3fbdc1c8a7f54a2de1c97f30`, APK SHA256
`4e8e3dd0a427d7791f9226aca670c590a9b443010d9b7cac7f1ae5e775f03c9f`,
[run 38945644929 / runtime job 117126639270](https://github.com/xmy953538104/pine_CN/actions/runs/38945644929/job/117126639270).
The official image booted in about 15 minutes. ABI/boot/package-manager checks
passed with system-server PID 1846 stable during the short readiness observation.
Actual `adb install` then exhausted its 300-second command budget (302.912 seconds
including the preceding absence query). Its report is `environment_blocked`:
2 checks passed, 16 blocked; no App launch, Settings, native-log or export pass.
The [runtime artifact](https://github.com/xmy953538104/pine_CN/actions/runs/38945644929/artifacts/12066918720)
preserves that outcome. A green Actions conclusion does not change it. The
runner had `/dev/kvm`, but the official probe reported permission denied; software
acceleration was used without modifying group, device or security permissions.
Framework logs during installation show repeated other-system-app ANRs/restarts
and slow service handling; these do not establish a Pine failure or a single
installation root cause.

The next workflow retains a finite 900-second CI installation budget and full
installation-interval logcat. It also tries the same exact APK on standard
`macos-15-intel`, using the official emulator's own acceleration probe. GitHub
[documents this standard Intel runner as free for public repositories](https://docs.github.com/en/actions/reference/runners/github-hosted-runners);
this repository is public. Hosted ARM macOS does not support nested virtualization,
so it is not used. This alternative does not change Linux KVM permissions and is
not assumed to succeed before its actual runtime report exists.

**Intel macOS checkpoint, 2026-10-16 19:11 UTC:** source
`11a190d1597276b40cddc6d30e7bc43b858ba144`, exact APK SHA256
`07bfacb6e1b7e6914769cc0e7d5bc992148502b640385cb6a03d5c097f64d722`,
[run 38950428391 / job 117142353647](https://github.com/xmy953538104/pine_CN/actions/runs/38950428391/job/117142353647).
The official acceleration probe returned 0 and `Hypervisor.Framework OS X Version
15.7`; read-only `kern.hv_support` returned 1. Android completed boot, but the
first readiness `pidof system_server` returned `672 1618`, and the runner stopped
after 1.273 seconds without installing Pine. The [report and commands](https://github.com/xmy953538104/pine_CN/actions/runs/38950428391/artifacts/12068704693)
record `environment_blocked`, 1 check passed and 17 blocked. Framework logs show
PID 672 handling boot and system-app ANRs, but no `/proc` identity was captured
for PID 1618; its cause must not be invented. This is not evidence that Intel
HVF is unavailable. The next readiness check retries transient missing/multiple
PIDs or package-service failures within its existing finite budget, while still
requiring one unchanged PID across the package query and continuity interval.

The same commit's [Ubuntu job 117142353476](https://github.com/xmy953538104/pine_CN/actions/runs/38950428391/job/117142353476)
successfully installed that APK in 482.404 seconds including its absence query,
then verified the exact installed SHA256. At 19:20:50.143, Android watchdog killed
the observed system-server PID 1913 for `Blocked in handler on i/o thread
(android.io) for 153s`. The Settings start command overlapped this restart and
returned `Failure calling service activity: Broken pipe (32)`. Its [runtime
artifact](https://github.com/xmy953538104/pine_CN/actions/runs/38950428391/artifacts/12070356257)
records 4 checks passed, 14 blocked, and no observed Pine crash; this does not
establish successful native loading or UI acceptance. The next fresh AVD uses
4 GiB inside the same standard runner, after the 3 GiB boot logs showed memory/I/O
pressure and system-app ANRs. This is an empirical capacity retry, not proof of a
single cause or a controlled game-performance comparison.

**Intel macOS Android36 checkpoint, 2026-10-16 19:50 UTC:** source
`f8aefde1c30895859ad70b28f60edf4980145bdf`, exact APK SHA256
`08e0c44e3cec9becd92ac0aa1dee94f5d0bd35ab0e67a62bb44c229161f856af`,
[job 117156978705](https://github.com/xmy953538104/pine_CN/actions/runs/38954682338/job/117156978705).
The revised readiness gate passed on its first observation with PID 665 unchanged
across a real package query and 2.553-second continuity window. Installation
succeeded in 235.583 seconds including its absence query; the installed SHA256
matched. The actual Settings-start command was followed by a 120-second UI
observation that never reached Pine's Settings. Four captured XML hierarchies
show `Process system isn't responding`, with normal `Close app` and `Wait`
buttons. A later window dump separately shows `Application Not Responding:
com.google.android.gms`; these are not assumed to be the same dialog.
UiAutomation itself also repeatedly timed out. The [unmodified report](https://github.com/xmy953538104/pine_CN/actions/runs/38954682338/artifacts/12071905739)
is `environment_blocked`: 5 passed, 13 blocked, 0 failed, with no Pine Java/native
crash observed during capture. This does not prove Pine native loading, Settings,
diagnostic logging or export. No dialog was dismissed or platform setting changed.

The next attempt uses the official Android35 Google APIs x86_64 r9 image.
Its actual bridge also supports `CNTFRQ_EL0` in all three decoder engines, rather
than inferring support from the shared `0.2.3` version string. The archive matched
Google's manifest SHA1 `0103e6dab21290c4b9d16550a3ce99476f884eef`; the actual bridge
SHA256 is `9b5808354a85d722a290ae879400b909aa961955e80ea1a7e807aea9416d92f9`.
Read-only extraction and official `aapt` manifest parsing also verified actual
DocumentsUIGoogle, ExternalStorageProvider, DownloadProvider and DownloadProviderUi
APKs, with packages `com.google.android.documentsui`, `com.android.externalstorage`,
`com.android.providers.downloads` and `com.android.providers.downloads.ui`.
This confirms the SAF packages exist, not that a save intent has executed.
This is a platform
configuration retry after observed Android36 ANRs, not an App-code workaround or
a claim that Android35 has run Pine successfully. The same bounded install/UI,
identity, native-log, export and crash checks remain required.

- Local APK source commit: `00811378bad3ebeb7a7188508fc1cdc643fb4304`.
- APK: `/workspace/android-build/evidence/00811378/pine-diagnostics-00811378-supplemental-arm64-debug.apk` (51,470,773 bytes).
- APK SHA256: `aea2cb73b5dfbca9f1341e8b964c34bd9e1735f01657377356f4e4445c4dd808`.
- Local provenance is supplemental: pinned upstream JitPack dependencies were rebuilt unchanged outside the checkout after local download restrictions; the production native source tree matched the identified source. It is not the exact CI artifact.

| Local attempt | Actual observation | Evidence under `/workspace/android-runtime` |
| --- | --- | --- |
| Android 30 streamed install | APK/device checks passed; real `adb install` failed after 79.853 s with package-service broken pipe. No UI attempted. | `smoke-00811378-local-2/runtime-report.json` |
| Android 30 non-streamed install | Successful target-absence query, complete 51,470,773-byte push, then exit 224 after 205.823 s with broken pipe. Final PMS listing returned no target. | `smoke-00811378-local-4-no-streaming/non-streaming-attempt-report.json` |
| Android 34 strict runner | Thirty minutes of boot observation plus the final 60 s gate did not produce a boot-complete flag; 1 required check passed, 17 blocked, exit 2. No installation from this strict run. | `smoke-00811378-api34-1/runtime-report.json` |
| Android 34 independent manual attempt | Real PMS and AMS responses and short same-PID continuity passed while boot flag was absent. The sole real install failed after 44.947 s with broken pipe; full boot remained explicitly blocked. A later successful package listing confirmed the target absent. | `smoke-00811378-api34-manual-preboot-1/manual-preboot-attempt-report.json`, `api34-final-target-package-query.json` |

The Android 30 streamed-install window contains a confirmed system-process fatal error: system server PID 4153 threw `IllegalStateException: Lost network stack`, then Zygote recorded its death as the installer lost its service connection. During the non-streamed install, Zygote recorded PID 5723 killed by signal 9; the retained evidence does not identify that kill's sender. The network-stack root cause and any separate CI compatibility issue remain unisolated. Retained Android 30 logs show network-stack processes killed for background ANRs; no explicit guest OOM/LMKD kill was found there. Absence in logs does not rule out pressure. Host cgroup OOM counters were zero. Android 34 later recorded a `system_server_lowmem` marker; that marker alone does not establish why a process was killed.

The original AVD was preserved and stopped gracefully. A separate official **Android 34 Google APIs x86_64 r14** AVD started at 17:24:18 UTC with 3072 MiB RAM, two cores, software CPU acceleration and a 480×800 display at 240 dpi. The emulator enforced a sparse 6 GiB virtual userdata partition despite a 2 GiB request; physical workspace free space remained above 6 GiB. No guest security, network or watchdog setting was changed. Download and real native-bridge library checksums were verified. The live guest advertised `x86_64,arm64-v8a`, `native.bridge=libndk_translation.so` and `isa.arm64=x86_64`; none of these facts proves Pine native loading.

Initial Android 34 package preparation completed, but framework watchdogs killed PIDs 532, 1231 and 1832 while waiting on default runtime permissions. PID 2338 completed that phase, then its watchdog killed it at 17:56:32 while waiting on `RoleService.maybeGrantDefaultRolesSync`. The manual install's broken pipe at 17:56:35 overlaps that confirmed platform death. Android performed its own framework restarts; QEMU was not restarted. No local Pine process ran, so these logs do not reproduce or explain the separate CI SIGILL.

Official ATD candidates were excluded after their real build properties showed no advertised ARM64 translation. The image evaluation is `/workspace/android-build/image-evaluation/evaluation-summary.json`. Launch/ABI/watchdog/install evidence is in `api34-start.json`, `api34-boot-observations.jsonl`, `api34-watchdog-excerpts.txt`, `api34-manual-install-platform-window.txt`, and the preserved unfiltered `api34-through-local-attempt-logcat.txt`. The complete local checkpoint index is `local-00811378-runtime-checkpoint.json`.

Historical checkpoints and their unmodified failure evidence follow. Build/JVM/native-core/fake-ADB checks are not APK runtime passes; translated execution also does not establish physical ARM64 or NCE/game compatibility.

## Host and official tools

- `uname -m`: `x86_64`; CPU: AMD EPYC 9V74.
- `/dev/kvm` and `/dev/dri`: both absent. CPU flags expose neither `vmx` nor `svm`.
- Official emulator `-accel-check`: code 3, `KVM requires a CPU that supports vmx or svm`.
- Repository `app/build.gradle`: `minSdk 29`, `abiFilters "arm64-v8a"`. An ordinary x86-only Android image cannot establish compatibility with this APK.
- Installed **only official Google SDK downloads**, outside the repository under `/workspace/android-runtime`. Archives were verified against SHA1 checksums in Google's HTTPS repository manifest before extraction:

| Package | Archive | Manifest SHA1 |
| --- | --- | --- |
| Emulator 37.2.12.0 | `emulator-linux_x64-16428233.zip` | `cd7362ea55dfb86a418958138dc396e74165dd01` |
| Platform tools 37.0.1 | `platform-tools_r37.0.1-linux.zip` | `477254aa5f903c15cf51001717bdf347fb6b53e0` |
| Google APIs Android 30 x86_64 r16 | `x86_64-30_r16.zip` | `6ae21030eaadc041078444d3798e4b399f3e787d` |
| Google APIs Android 34 x86_64 r14 | `x86_64-34_r14.zip` | `e0f6c9a0691aa27bd597d0deb1bcfdc943ac8ca7` |
| Google APIs Android 36 x86_64 r7 | `x86_64-36_r07.zip` | `c6bf44bdcd885bb902b4ba752d111a073ad7a817` |

Manifest URLs:

- <https://dl.google.com/android/repository/repository2-1.xml>
- <https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-1.xml>

The emulator supports software rendering and software CPU execution, so the absent KVM/GPU alone was **not** treated as proof that Android cannot run. Official documentation: [acceleration requirements and software rendering](https://developer.android.com/studio/run/emulator-acceleration), [command-line startup](https://developer.android.com/studio/run/emulator-commandline), [emulator release notes including ARM translation](https://developer.android.com/studio/releases/emulator).

## Original Android 30 launch (historical checkpoint)

AVD configuration: `/workspace/android-runtime/user/avd/PineDiag.avd/config.ini`.
System image: `/workspace/android-runtime/system-images/android-30/google_apis/x86_64/`.
Full startup log: `/workspace/android-runtime/boot.log` (local evidence, not an export artifact).

Executed:

```sh
ANDROID_AVD_HOME=/workspace/android-runtime/user/avd \
ANDROID_EMULATOR_HOME=/workspace/android-runtime/user \
ANDROID_SDK_ROOT=/workspace/android-runtime \
/workspace/android-runtime/emulator/emulator \
  -avd PineDiag -no-window -no-audio -no-snapshot \
  -gpu swiftshader -accel off -cores 2 -verbose
```

At the original handover the QEMU emulator process was **PID 2723**. It was later stopped gracefully and the original AVD was preserved. The original 120-second wrapper had been removed while retaining QEMU to permit continued slow software boot. Check the latest checkpoint and process evidence rather than assuming a historical PID remains valid.

The log confirms software rendering initialization and image fingerprint:

```text
google/sdk_gphone_x86_64/generic_x86_64_arm64:11/RSR1.240422.006/12134477:userdebug/dev-keys
```

This suggests an ARM translation image, but **does not prove Pine ARM64 JNI or NCE compatibility**. The actual guest ABI list and successful APK startup must still be tested.

`adb` initially aborted even for `version` because `/home/agent/.android` was missing on a read-only home. `ANDROID_USER_HOME`, `ANDROID_SDK_HOME` and `ANDROID_EMULATOR_HOME` did not fix this ADB behavior. A narrowly scoped, automatically approved escalation created that standard ADB configuration directory; official ADB then started its server normally. No authentication disabling, security configuration change, paid service, or unofficial download was used. Never commit the ADB directory or its authorization keys.

Last observed command/result:

```text
/workspace/android-runtime/platform-tools/adb devices -l
List of devices attached
emulator-5554          offline transport_id:1
```

## Checks pending at the original checkpoint (historical)

```sh
/workspace/android-runtime/platform-tools/adb devices -l
/workspace/android-runtime/platform-tools/adb -s emulator-5554 shell getprop sys.boot_completed
/workspace/android-runtime/platform-tools/adb -s emulator-5554 shell getprop ro.product.cpu.abilist
/workspace/android-runtime/platform-tools/adb -s emulator-5554 install -r /path/to/CI-debug.apk
```

After boot and ABI verification, inspect the APK manifest for the real application ID/activity and launch it. Exercise Settings → Diagnostics off/on, session and level changes, status, export preview and Android document picker; capture screenshots/UI hierarchy and `PineDiag` JSONL/logcat. Do not infer success from install alone. Keep an unmodified log of errors. Even successful UI execution through ARM translation is **not** physical ARM64 execution or commercial-game/NCE validation. No commercial games, tickets, keys or private saves were used.

Still unrun at this checkpoint: completed Android boot, guest ABI query, APK installation/launch, settings toggling, on-device logging, export flow, physical ARM64 test, commercial game validation, performance comparison.

## Runtime follow-up: software boot and framework watchdogs

At 2026-10-16 16:31 UTC the original emulator had run for about 40 minutes.
Official ADB could execute guest commands, but `sys.boot_completed` was empty and
`init.svc.bootanim` was `running`. The actual guest ABI list was:

```text
x86_64,x86,arm64-v8a,armeabi-v7a,armeabi
```

This confirms the image advertises ARM64 translation; it does not establish
successful Pine JNI loading, physical ARM64 execution, or NCE compatibility.
The preserved log shows two explicit framework watchdog kills: system server
PID 525 while enumerating installed package information for SettingsProvider,
and PID 818 while loading DevicePolicyManagerService. A third system server
PID 1125 subsequently started. These are framework restarts, not Pine crashes;
no Pine APK had been installed at that point. Do not describe three starts as
three verified watchdog kills.

The execution environment has a four-CPU cgroup quota and a 16 GiB memory limit.
While the native build was running, memory use was about 14 GiB and the emulator
used approximately one host CPU. No cgroup OOM kill was recorded. This is evidence
of constrained software execution; the exact cause of each watchdog timeout has
not been isolated.

After coordination with the build owner, the original emulator was closed and
the same AVD/data was restarted. `adb -s emulator-5554 emu kill` returned exit 0
without stopping the process, so SIGTERM closed the original QEMU process
gracefully; its log records renderer shutdown and user-configuration saving.
No data wipe, ABI replacement, or Android security-setting change was performed.
Official emulator help documents `-no-boot-anim` as reducing boot time on slow
machines, and `-skin WIDTHxHEIGHT` as setting an exact framebuffer size.

```sh
ANDROID_AVD_HOME=/workspace/android-runtime/user/avd \
ANDROID_EMULATOR_HOME=/workspace/android-runtime/user \
ANDROID_SDK_ROOT=/workspace/android-runtime \
/workspace/android-runtime/emulator/emulator \
  -avd PineDiag -no-window -no-audio -no-snapshot -no-boot-anim \
  -gpu swiftshader -accel off -cores 2 -skin 480x800 -verbose
```

Restarted QEMU PID: **31534** (verify it is still current before use).
The new startup log confirms `Setting display: 0 configuration to: 480x800`.
At this follow-up checkpoint the restarted guest was still booting; this is not
an APK smoke-test result.

The restarted guest reached ADB-online after approximately 2 minutes 45 seconds.
`adb shell getprop` then confirmed `ro.dalvik.vm.native.bridge=libndk_translation.so`,
the same advertised ABI list, and `debug.sf.nobootanimation=1` from the official
startup flag. The boot-animation service still reported `running` at that early
checkpoint, so that service property alone must not be interpreted as evidence
that animation suppression failed. `sys.boot_completed` was not yet present.

Local evidence, outside the repository:

- `/workspace/android-runtime/first-boot-host-state.txt`
- `/workspace/android-runtime/first-boot-system-errors.log`
- `/workspace/android-runtime/first-boot-crash-buffer.log`
- `/workspace/android-runtime/first-boot-logcat-excerpt.txt`
- `/workspace/android-runtime/runtime-current-logcat.txt`
- `/workspace/android-runtime/first-boot-shutdown-note.txt`
- `/workspace/android-runtime/boot-optimized-command.txt`
- `/workspace/android-runtime/boot-optimized.log`
- `/workspace/android-runtime/optimized-boot-properties.txt`
- `/workspace/android-runtime/optimized-boot-logcat.txt`

### Subsequent framework restarts in the optimized boot

The lighter startup reached package-manager readiness at guest 332.603 seconds.
Official `adb shell cmd package path android` returned
`package:/system/framework/framework-res.apk`; this proves a responding package
manager at that checkpoint, not completed boot or stable UI. The log records
`InstallSystemProviders` completing in 4.227 seconds, but this improvement does
not isolate the effects of animation suppression, framebuffer size, cached data,
or concurrent build load.

Two further watchdog kills were observed in the optimized boot:

- 16:41:20 UTC, system server PID 518: main-thread stack in
  `PermissionManagerService.checkUidPermissionInternal`.
- 16:46:53 UTC, system server PID 1159: main-thread stack in
  `Thread.nativeCreate`, called from `DropBoxManagerService`'s observer. This
  instance had reached `boot_progress_enable_screen` at guest 798.174 seconds.

Exact extracted stacks: `/workspace/android-runtime/optimized-watchdog-stacks.txt`.
The complete unmodified streamed log remains in `optimized-boot-logcat.txt`.
At the 16:49 UTC query, the third system server in this boot (PID 2007) had reached
activity-manager readiness. Official ADB still responded, boot animation was
`stopped`, and `sys.boot_completed` was still absent. No Pine APK had been
installed. The AVD remains running; no watchdog or Android security setting was
changed and no further emulator restart was performed.

### Repeatable actual-APK UI runner

`tools/android-diagnostics-smoke.py` uses official ADB/UIAutomator, normal input
events, and read-only `run-as` access to the debuggable app's fixed diagnostics
files. It requires a fresh target package and completed Android boot; it does
not uninstall, clear app/device data, clear logcat, root, or change Android
security settings. Example (supply the commit embedded in the APK):

```sh
python3 tools/android-diagnostics-smoke.py \
  --adb /workspace/android-runtime/platform-tools/adb \
  --serial emulator-5554 --apk /path/to/app-full-debug.apk \
  --expected-commit FULL_40_CHARACTER_SOURCE_COMMIT \
  --output /workspace/android-runtime/pine-smoke-run-1
```

The runner records the submitted and installed APK SHA256, actual device/ABI,
command results, screenshots, UI XML, native JSONL, filtered runtime logcat,
preview snapshot bytes, and the real SAF-saved ZIP when obtainable. It checks
Settings → Developer diagnostics, default off, enable/session/native metadata,
detailed tracing, retention/category configuration, preview, export bytes/ZIP
allowlist and JSON, and disabling with the native `SESSION_SUMMARY` Stop record.
A picker that cannot be safely automated becomes an explicit blocked check with
its UI evidence. Ordinary UI cleanup attempts to disable diagnostics even after
a later check is blocked.

`runtime-report.json` reports `verified` (exit 0), `environment_blocked` (exit 2),
or `failed` (exit 1). `summary.json` retains all individual pass/fail/blocked
checks. A recorded app Java/native crash fails the run even when UI observation
timed out. Possible platform causes are retained for manual attribution and
cannot mask an app crash; a framework watchdog without a recorded Pine crash is
an environment blocker. `tools/tests/attempt-android-runtime.sh` preserves exit-2 evidence and
states the limitation in the CI step summary without presenting it as an APK bug.

Runner-only validation performed here: Python syntax, CLI help, Bash wrapper
syntax, and an absent-APK failure-path check. That check produced exit 2,
`environment_blocked`, 18 blocked checks and **zero ADB commands**. It is not an
APK/UI test. The subsequent actual Android 30 installation attempts are recorded
in the latest checkpoint above; Android 34 testing is a separate attempt.

The offline `tools/tests/test-android-smoke-deadlines.py` regression also passed
six tests using real controlled subprocesses. The original five deadline scenarios
reject late UI/session/native observations, bound scrolling, and keep a slow
screenshot from replacing an already observed result; all five reject the old
runner. Additional readiness cases cover missing/multiple PIDs and failed package
queries before a stable observation, persistent ambiguity reaching its deadline,
and a PID change before a later stable window. These are fake-ADB tests; they
pass none of the 18 Android APK acceptance checks.

Cleanup checkpoint, **2026-10-16 18:08 UTC**: only the verified owned Android 34 QEMU PID 45401 and full-logcat PID 46096 were stopped with SIGTERM; both reached exited/zombie state and emulator output records normal configuration/render-thread shutdown. The AVD and userdata remain intact, with no wipe or additional installation. Final properties and log tail were preserved before stopping. Those late properties reported `sys.boot_completed=1` and system-server start count 6; this later flag does not replace the earlier blocked strict report or establish any Pine UI/native pass. The last successful target-package absence query remains the one recorded at 17:59:04. Shutdown evidence is `/workspace/android-runtime/api34-final-shutdown.json`, `api34-final-shutdown-getprop.txt`, `api34-pre-shutdown-logcat-tail.txt`, and the final unfiltered `api34-final-stopped-logcat.txt` (8,517,713 bytes, SHA256 `aa552828aaa848ccb5dd6f19dc77ad6d18b76336f8d2ffe2a563a3fd96fe52fc`).
