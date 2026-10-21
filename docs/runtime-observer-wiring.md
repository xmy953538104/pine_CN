# Load-owned NSO identity and a gated NCE callsite observer

This PR extends accepted PR24. It does not replace the PR22 APK or modify an NSP.

The production `LoadNso` → `LoadExecutable` path retains owned full 32-byte Build ID, original unpadded segment hashes/layout, and bounded windows. After NCE patching and mapping, it separately verifies actual mapped text/rodata windows while loading synchronously, before Guest admission. Export explicitly distinguishes preimage, post-NCE/pre-observer windows and any installed observer bytes. Existing symbol spans are never used for identity.

Route B is selected: one authenticated callsite BL establishes the original caller LR, a dedicated stackless producer uses only X16/X17, and a B tail transfer traverses the untouched original PLT. Its admission verifies that the PLT unconditionally overwrites both scratch registers. It neither reads the GOT nor asserts the eventual AbortImpl target or a true outer-function entry hit. Route A requires additional authenticated entry displacement and dynamic-binding evidence. Existing later SVC context is not entry evidence.

Actual NCE patch-size planning reserves the stub before layout only in an explicitly typed controlled test build. Actual `ThreadContext` appends the slot pointer while asserting all assembly offsets. Actual `KNceThread::Init` attaches a lifecycle-owned slot; teardown joins before the derived context dies. The producer writes a fixed single-owner POD slot without Guest stack access, allocation, flags changes, helper calls, scheduling, I/O, JNI, or locks. Failed epochs/inactive/full slots tail-transfer. An interrupted Writing slot is not a record and is never reused. Existing Diagnostics worker drains completed records, pins sink/session/process/launch/option generations, and admits at most one event for the module/Launch. Event timestamps describe worker recording; capture clock is explicitly unavailable.

## Deployment gate remains closed

Production `Domain::Guest` cannot reserve/install the observer: the only constructor granting controlled installation is excluded without `PINE_DIAG_RUNTIME_TEST`. Readonly identity requires verbose diagnostics plus explicit `module_runtime`, default unselected. Disabled execution preserves the original patch layout and creates no slots. The physical RWX mapping in Pine does not prove continuous runtime code/rodata integrity. This PR does not alter that memory policy. Consequently no actual game interception or experimental APK is authorized by these tests.

Opening the gate requires a qualified immutable-code/assertion lifetime mechanism compatible with existing mappings; executed Android/Pine process/thread teardown, interrupted producer and original error propagation tests; and review of actual binding/target provenance for any outer-entry claim. No hot unpatch is attempted: close epochs and retain storage until producers quiesce. Rollback helper applies only during quiescent loading and rejects conflicting text/stub bytes.

## Verification and scope

Run `python3 tools/tests/test-runtime-observer.py`, `python3 tools/tests/test-nso-image-observation.py`, and `python3 tools/tests/test-runtime-pipeline.py` for host lifecycle/emitter/actual Loader/NCE mapping checks. The new CI installs signed Ubuntu cross tools and QEMU, reassembles every producer byte, and runs `test-runtime-pipeline.py --aarch64 --output runtime-evidence`.

The pipeline extracts current production LoadNso, LoadExecutable, NCE planner/patcher and KNceThread Init/Run bodies verbatim. VFS/decompression, mapping, HLE and process/kernel dependencies remain controlled. Its own NSO-like content and PLT/resolver have no commercial bytes. Guarded Guest SP has no writable redzone. Resolved/unresolved own PLT, return/nonreturn, register/flags/SIMD, stale/full/Writing/inactive slot paths are assertions in the QEMU Linux-user AArch64 target on x86_64. This target is neither Android nor native ARM64 hardware. The own resolver is not Horizon rtld. Register comparisons explicitly allow relocated module-dependent values and assert their formulas. The export test requires actual DiagnosticExport to receive real pipeline-writer files and retain raw 64-bit strings and `runtime_connected=false` exactly once in its ZIP/resource window.

Source-body hashes, generated-source hash, source SHA/dirty state, commands and execution domain are written to evidence. Each of the 14 cases retains its own actual writer files. Missing resource windows or missing/extra capture rows fail the pipeline. No performance, complete Pine scheduler/longjmp teardown, real game branch identity, actual Host SIGSEGV stack or Android APK execution is implied.

Owner-published local environment verification (2026-10-21): activation scripts were inspected before sourcing. The actual wrappers in `/workspace/pine-env/bin` use GCC/G++ 14.2.0 (Debian 14.2.0-19), Binutils 2.44 and QEMU 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1). The compiler sysroot is `/workspace/pine-env/aarch64`; QEMU's Linux loader/library prefix is `/workspace/pine-env/aarch64/usr/aarch64-linux-gnu`. No toolchain reinstall was needed. Run with:

```sh
source /workspace/pine-env/activate.sh
python3 tools/tests/test-runtime-pipeline.py --aarch64 \
  --qemu-ld-prefix /workspace/pine-env/aarch64/usr/aarch64-linux-gnu \
  --output /tmp/pine-runtime-evidence
```

The original candidate `5f9a83d1aab18d4c5ffbea1f8c81e91831a2d44e` failed GCC compilation because its extracted Run body used the Clang-only `lr` clobber spelling. The equivalent `x30` spelling compiles with both compilers; official NDK compilation produced identical 308-byte `.text` for both spellings. Execution then exposed a fixture bug: `MoveRegister` explicitly returns zero for omitted instructions, and the fixture wrote those zero values as UDF. The fixture now omits them. QEMU's `-L` remaps absolute `/` opens into the library root, so the test executes with a relative log directory inside its output cwd; production resource-window path policy is unchanged.

After these corrections, local controlled AArch64 execution passed all 14 cases, including actual asynchronous writer/resource-window retention. Production identity/invalidation/admission and consumer-slot tests, callsite emitter/rollback negatives, 85 owned metadata assertions, actual LoadNso body, host LoadExecutable/NCE mapping, full existing native Diagnostics script and exact 208-byte producer assembly equality also passed. These establish the stated controlled execution scope only; production installation remains hard CLOSED and real game captures remain zero.

The normal Gradle JVM-only task graph passed `tools/ci/verify-jvm-only.py`. Its actual test task could not resolve offline JitPack colorpicker and renderscript artifacts; no denied resource was retried online. For an independent JVM exporter run, supply existing Kotlin compiler/JUnit/serialization classpaths:

```sh
python3 tools/tests/run-runtime-export.py \
  --compiler-classpath "$PINE_KOTLIN_COMPILER_CLASSPATH" \
  --classpath "$PINE_EXPORT_TEST_CLASSPATH" \
  --capture /tmp/pine-runtime-evidence/logs/unknown \
  --output /tmp/pine-runtime-export
```

This runs the unchanged production DiagnosticExport and original RuntimePipelineExportTest, with the actual portable SelectedScanRecord parser extracted verbatim and its real dependencies. No dependency stubs are used. It omits the unrelated Android SelectedScanReports worker, makes no downloads, and produces source/input/ZIP hashes. Local execution passed with real QEMU writer input, one retained callsite row in the ZIP resource window, raw `0xfedcba9876543210` intact and `runtime_connected=false`. This is JVM export evidence, not an Android Activity/SAF or app execution result.

The explicit exporter runner is named `run-runtime-export.py` because it requires classpaths and a writer capture. It is outside the argument-free `test-*.py` fast-check discovery glob. The runtime-observer regression additionally compiles without `PINE_DIAG_RUNTIME_TEST`: the controlled constructor must be unavailable, and the normal production constructor must reserve zero observer bytes and attach no Guest slots even with readonly identity explicitly selected. This is host execution of the closed gate, not Android lifecycle qualification.

An O2 Debug APK attempt on exact `806731f58e41644b907cde1df20a60ce010e6373` ran `./gradlew --offline --no-daemon --build-cache -PpineNativeDebugOptimization=2 assembleFullDebug --stacktrace` and failed before compilation at `:app:dataBindingMergeDependencyArtifactsFullDebug`. The uncached fixed dependencies were `com.github.KikiManjaro:colorpicker:v1.1.12` and `com.github.android:renderscript-intrinsics-replacement-toolkit:344be3f`. The environment also lacks `app/libraries/perfetto/sdk`; its pinned submodule is `e5293f1cf1800d5776f4bfe96d1ec0aeec30cd46`. No restricted download was retried, and no replacement dependency was introduced. No APK, packaged ELF, matching symbols or actual candidate signature was produced or claimed.

The existing `.github/workflows/diagnostics-performance.yml` is the O2 candidate path: it requires successful fast checks for the exact source and the repository's existing CI signer (public certificate SHA256 `70edcdc802bb16afe9665ecd3d342ea74b217a79b6efb4dbdfbb3ea3eddf16c1`). It must not fall back to a local Debug signer, add credentials, or activate the observer. To resume, the owner can publish the exact missing artifacts and pinned Perfetto source in the managed environment/cache, or run the existing CI workflow with its existing signing configuration once prerequisites are available. Its optional runtime attempt remains disabled; neither build success nor QEMU evidence satisfies the original Android safety acceptance gate.

Owner-authorized reads of new exact CI runs confirmed that `806731f5` passed Runtime observer controlled pipeline run39544660627, while fast run39544660718 failed Tools at the required-arguments exporter runner and JVM at DiagnosticsActivityTest's default-category assertion (263 tests, one failure). The later `bccb4698` fast run39546987357 passed Tools but retained the same JVM failure. The Activity correctly defaults `module_runtime` to unselected; the test previously assumed every category was selected. The test now explicitly asserts the five ordinary default selections and the unselected experimental category, deselects only checked rows, and still requires the actual dialog to persist/send `none`. This does not change production behavior or the installation gate.

Earlier Forbidden Actions/PR resources were not retried. Their responses had no captured HTTP headers/status or approval-review rejection text, so their origin remains unknown. New CI job logs were read through the authorized GitHub connection; transfer of the separate JVM report artifact to the local executor hit a proxy tunnel HTTP403 and was stopped. No alternate download route, historical PR22 metadata, or denied Perfetto/JitPack resource was used. Subsequent exact-source CI/build outcomes are recorded separately and must not be inferred from local test results.

To reproduce compile-only qualification with the official NDK: `python3 tools/tests/test-runtime-pipeline.py --aarch64 --compile-only --cxx <NDK>/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++ --output /tmp/pine-pipeline-ndk`. Its result explicitly says not executed.

Review corrections: the consumer-only host fixture configures `observerInstalled` before release publication, never while the worker can read it. The real registry try-lock/capacity admission now occurs before writing stub or redirected BL. Deterministic tests hold the real registry mutex on another host thread or fill all 16 weak entries with retained owners; both failures must leave text and stub byte-identical. These tests do not execute the producer. Preloaded Writing/full states are not interrupted Guest execution. The controlled pipeline has no-op permissions, empty HLE and simplified process/thread dependencies; it excludes real scheduler/Kill/error propagation/teardown join qualification even if QEMU later passes.

Historical capability diagnosis: the preceding executor lacked QEMU and AArch64 GCC/G++ and stopped before Guest execution. PR24's earlier QEMU proof came from GitHub run39530368168. That historical lack of local tools is superseded by the owner-published environment above; remote CI results remain unobserved.
