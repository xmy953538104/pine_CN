# Diagnostics settings and export Activity regression coverage

This work starts at `b25e9e411dde6921b4fb1fda3c3cb7842930c2f3` (PR #17).
It fills the gap between the existing exporter tests and the actual settings
screen: `DiagnosticsActivityTest` creates the production `DiagnosticsActivity`
with its real manifest, theme, AndroidX lifecycle and view hierarchy. Tests
click the actual controls, dispatch results through the real Activity Result
registry, and inspect ZIPs written by the production exporter.

The Application is replaced with a plain test Application so no native game
engine starts. Only the Diagnostics JNI side effects are shadowed. A synthetic
ContentProvider substitutes for the system-selected destination; it can open a
real temporary output file or fail with a public canary exception. The file
picker itself is not executed. All source logs and output files belong to the
Robolectric app sandbox and contain public synthetic input only.

## Implemented scenarios

| User flow | Assertions at the real Activity/export boundary |
| --- | --- |
| Initial controls, enable, detailed tracing, disable | Default off, persisted settings, JNI configuration arguments and nonempty changing Session IDs |
| Cancel prepared preview | Preview lists fixed filenames; prepared copies are removed; no picker launches |
| Cancel document picker | Prepared copies removed; no destination writes; another preview is available |
| Successful document result | Correct CreateDocument action/MIME/title, real ZIP write, metadata commit, UTF-8, allowlist and URI redaction |
| Destination fails, then retry | Visible failure without the raw exception canary; cleanup; successful fresh retry |
| Export cache cannot be a directory, then retry | Visible preview failure, control reenabled, no picker; fresh preparation works |
| Retention chooser | Selected bound persists and reaches the configuration boundary |
| Category selection cancel/apply | Cancel preserves settings; deselecting everything persists explicit `none`, not the empty value used for all categories |
| Picker cancellation after Activity recreation | Retained snapshot is released through the new Activity registry and another preview works |
| Picker success after Activity recreation | New Activity exports exactly the old prepared metadata/log bytes after source logs change |
| Repeated clicks during preparation | Hold the first preparation at the JNI status boundary, count live lifecycle jobs after both clicks, drain all preparation workers, then assert one snapshot/preview and cleanup |
| RESULT_OK without a URI | Treated as cancellation; no output is opened |
| Cache evicts a prepared snapshot | Visible failure rather than false success, cleanup and successful fresh retry |
| Settings recreation while enabled | Preferences and live Session survive without another native configuration/start |

The AndroidX `CreateDocument` contract is exercised as supplied by the pinned
dependency; the suite does not invent an extra intent category requirement.
Main-looper handlers and real IO workers are allowed to complete before asserting
results. Synthetic failure tests assert observable recovery, not only internal
state flags. A temporary local negative control removed only the Activity
preparation guard: the repeated-click case failed with two live preparations
instead of one. That mutation was restored before commit qualification; no
production changes from the control are part of this branch.

## Reproduction

Use the repository's Java 17, Gradle 8.6 and Android SDK setup. Existing CI's
JVM-only graph check prevents building native emulation or an APK for this task.

```sh
IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon clean
IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon \
  -PpineNativeDebugOptimization=2 -PpineSelectedContent=false \
  testFullDebugUnitTest --tests emu.skyline.diagnostics.DiagnosticsActivityTest

IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon clean
IS_BUILD_SIGNED=false SIGNING_STORE_PATH=unused.jks ./gradlew --no-daemon \
  -PpineNativeDebugOptimization=2 -PpineSelectedContent=true testFullDebugUnitTest
```

Run the full unit task with each Selected build-gate value when qualifying a
commit. Exact source SHA, flags, JUnit totals and input/report hashes belong in
the accompanying qualification evidence; development runs do not qualify a
later commit. Clean the current worktree build outputs for each qualification: a
local incremental Kotlin run retained an older inlined `DIAGNOSTICS_COMMIT` in
the Activity even after BuildConfig changed. The exact metadata-commit assertion
caught that mismatch and remains required. Local verification may use the already documented pinned local
dependency helper; GitHub CI uses the repository's ordinary dependency setup.

## Boundaries and remaining device acceptance

These are Robolectric/JVM tests, not APK installation, Android framework/device
execution, native sink output, physical USB/wireless ADB, content loading or
commercial game acceptance. Native log production and real SAF provider
behavior are not established by a synthetic log or provider. A real device must
still verify settings, Logcat/file output, actual document-picker permissions,
destination durability and rotation/recreation through the Android framework.

The current cloud check found x86_64, no `/dev/kvm`, no `/dev/dri`, and no device
listed by the official ADB tool. This establishes no ready Android execution
target; it does not turn JVM tests into an ARM64 APK run or claim that software
emulation is theoretically impossible. No security settings or paid resources
were changed to obtain a target.

This work does not resume the separately stopped CNMT research, change normal
Loader/content validation, authenticate auxiliary structures, change BKTR or
merge main. The previous CNMT platform-screening result is historical; it is
not presented as a new denial from this test task.
