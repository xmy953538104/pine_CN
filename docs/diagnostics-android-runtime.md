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
