# Controlled native optimization candidate

This experiment does not establish restored game speed. The existing Debug default
remains `-O0`; opt in with `-PpineNativeDebugOptimization=2`. Both builds retain
the same diagnostics, assertions, `-g3`, DWARF5, frame pointers, Android stack
protector and ordinary Debug APK configuration. No Release `NDEBUG`, fast-math,
LTO or disabled stack protection is introduced. No game clocks, rendering logic,
guest storage behavior, error propagation or logging policy changes here.

The generated `compile_commands.json` is checked for four core translation units
and published as `native-build-flags.json`, bound to the APK SHA256 and source.
Each session records `native_debug_optimization` separately from commit/build type.
Optimized symbols must match the optimized APK's own Build ID;991 symbols cannot
symbolize a different library.

## Host evidence and limits

The same production991 IStorage/Backing/sink and deterministic64-byte read fixture
were compiled with identical host options except `-O0` versus `-O2`. Three trials
per mode, one million reads each, preserved the same backing calls and checksum.

| Native host option | Diagnostics off | Normal | Detailed |
|---|---:|---:|---:|
| `-O0` | 69.3ns/read | 486.4ns/read | 1552.8ns/read |
| `-O2` | 8.9ns/read | 49.9ns/read | 111.5ns/read |

These are shared x86_64 host observations, not Android/ARM64/game FPS. Saturated
event loops drop records and therefore cannot be interpreted as accepted-event
throughput or a device speedup prediction. Paced critical records and concurrent
bursts are reported separately, with CPU time, producer latency, drop reasons,
write syscalls and retained bytes. Wall latency includes host scheduling.

An independent controlled fixture compiles the production legacy logger and
CircularQueue with the exact AudioCore adapters, replacing only host platform
dependencies. With text level Disabled,4096 adapter calls still caused4096 legacy
writes; a controlled200ms stalled logcat writer caused an approximately200ms
producer stall. The normal guarded LOGE macro produced zero records. Diagnostics
off/on did not remove the adapter's legacy writes. This demonstrates a blocking
path, not evidence of an actual game log flood. No logger policy is changed.

Reproduce without commercial content:

```sh
python3 tools/tests/test-native-debug-flags.py
python3 tools/tests/benchmark-diagnostics-performance.py --output /tmp/pine-perf-new
./gradlew -PpineNativeDebugOptimization=2 testFullDebugUnitTest processFullReleaseMainManifest assembleFullDebug
```

The performance workflow publishes the controlled O2 candidate, its101 JVM tests,
actual flags and matching symbols. The ordinary Diagnostics workflow still
builds O0 and audits its actual commands. No candidate release is published.

## Official emulator recovery

Retain both original O2 attempts:39045248440 and39047747847. Each installed
the exact26a198…07bf ARM64 APK but stopped at5/18 observed checks with13 blocked;
neither enabled diagnostics. The latter captured an actual successful
`lib/arm64/libskyline.so` load, but native session metadata remained unobserved.
Their covering ANRs belonged to Pixel Launcher and Digital Wellbeing respectively;
this is not evidence of a Pine crash or successful Settings observation.

The recovery host has4 logical CPUs, approximately14GiB RAM, working
Hypervisor.Framework; its AVD uses4 cores and6GiB with software graphics. The
selected cloud executor has neither `/dev/kvm` nor `/dev/dri`. Keep the official
API35 Google APIs x86_64 image with observed `libndk_translation.so` ARM64 support.
Do not mechanically add resources or substitute an x86 APK. Official
[ATD documentation](https://developer.android.com/studio/test/managed-devices#use-atd)
describes removed system apps/services; an alternate image's ARM64 translation
and complete DocumentsUI/SAF support have not been verified here. Its reduced
surface cannot stand in for this export acceptance.

For fresh job-owned AVDs only, `--close-known-system-anr` permits one ordinary
Close app per identified `com.google.android.apps.nexuslauncher` or
`com.google.android.apps.wellbeing` ANR during initial Settings observation.
The exact Android title and Close/Wait controls, matching current/focused ANR
window and preinstalled system-package identity must agree. Save the trace,
revalidate the hierarchy and focus, record all evidence and taps, then require a
fresh successful Settings observation. Pine ANRs fail, unknown/persistent system
ANRs remain blocked. No packages are disabled, data cleared or security settings
changed. All18 functional checks and original deadlines are retained. Report the
system failures even if later functional checks pass. This is translated APK
execution, not physical ARM64/NCE/game-speed validation.

## Reuse the existing CI signer

New O0 and O2 candidate builds use the repository's existing `KEYSTORE` and
`SIGNING_*` CI configuration. Missing configuration stops the build; there is
no generated key or random Debug-signer fallback. CI decodes the existing signer
only into an owner-readable temporary file outside the checkout and caches,
removes it after the build even on failure, and never uploads it. Fork PRs cannot
run the signed-APK job. These changes do not read key material into an agent,
create Secrets, resign old artifacts, publish a release or alter main.

The public certificate pin is
`70edcdc802bb16afe9665ecd3d342ea74b217a79b6efb4dbdfbb3ea3eddf16c1`.
It was independently read from the existing signed O0 APK in run39043871038,
artifact12105432966, APK SHA256
`e087533f923e38f49fd9e535d2adf9537c220d7190b2eb93a9a8dd9ec73806d6`,
by [public-only audit39050605173](https://github.com/xmy953538104/pine_CN/actions/runs/39050605173).
`verify-apk-signer.py` requires official `apksigner verify` success and exactly
that one signer; it writes `signer-identity.json` bound to the new source commit
and APK SHA256. Each new build has its own tests, actual optimization manifest,
APK hash, native Build ID and matching symbols. The original26a198…07bf O2
and its blocked runtime evidence remain unchanged.

This stabilizes signatures between future candidates. The published991 v1.3
Preview uses certificate `0ecefc03…7e47`, which differs from the existing CI
signer. It cannot be updated in place by these candidates while keeping the same
package. Do not uninstall or clear an owner's app automatically. An initial
migration needs the owner's backup and installation choice; subsequent candidates
with this same package/certificate can use Android's normal update mechanism.
Formal Pine's package remains separate. No claim of save/cache preservation on
an owner device is made by a public-certificate check alone.

## Device comparison still required

Keep compilation and diagnostic settings as two independent axes: O0/off,
O0/on, O2/off and O2/on. Use the same legal content, save, scene, graphics/audio
settings, device performance mode and warm-up; alternate order and repeat trials.
Measure wall time for a repeatable simulation action, frame-time behavior,
temperature/throttling and audio. Overlay FPS measures a smoothed presentation
interval and has no freshness or game-tick measurement, so60FPS alone does not
establish real-time simulation. Current reports have no measured logic-tick ratio.

Trace eligibility for fatal signaled exits, previous-session snapshot retention
and critical admission contention are separate known evidence gaps. They are not
fixed or hidden by native optimization. A critical reserve does not guarantee
admission when either try-lock fails; accepted snapshots cannot recover dropped
events, and a new session replaces the previous fixed-name critical state.
