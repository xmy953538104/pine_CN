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
