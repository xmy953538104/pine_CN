# Android runtime test: cloud execution evidence

Checkpoint: 2026-10-16 UTC. **No Pine APK has been installed or launched at this checkpoint.** APK/CI build is still in progress. Emulator startup, native core tests, JVM tests and fake ADB tests must not be reported as an APK smoke-test pass.

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

Manifest URLs:

- <https://dl.google.com/android/repository/repository2-1.xml>
- <https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-1.xml>

The emulator supports software rendering and software CPU execution, so the absent KVM/GPU alone was **not** treated as proof that Android cannot run. Official documentation: [acceleration requirements and software rendering](https://developer.android.com/studio/run/emulator-acceleration), [command-line startup](https://developer.android.com/studio/run/emulator-commandline), [emulator release notes including ARM translation](https://developer.android.com/studio/releases/emulator).

## Actual launch and current checkpoint

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

At handover the QEMU emulator process is **PID 2723**, running in the background; do not start a second copy against the same AVD. The original 120-second wrapper was removed while retaining QEMU to permit continued slow software boot. Check the current PID rather than assuming it remains valid after environment restart.

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

## Next actual checks (pending)

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
APK/UI test. An actual APK smoke-test result remains pending completed Android
boot and the final APK.
