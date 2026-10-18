# P1 integrated test candidate

This branch combines the Android/JNI entry from PR12 at
`d2d617f54427d6acf4a09d47e2252843486dba66` with the corrected actual-service
fixture from PR13 at `b4c2039c406720eaf28f772f3908b9e05f7bb000`.
The three PR13 test commits are replayed here; production `app/` files remain
byte-for-byte identical to PR12. Parallel-branch results do not pass this
candidate: all required evidence must name its own exact source commit.

The Android integration workflow now requires the real-AES/LZ4 selected-core
sanitizer/schema lane and corrected actual IPC/service lane before its Debug
APK job, alongside actual host-JVM JNI transfer and the app JVM OFF/ON matrix.
The native/service artifact includes both manifests, source status and the
workflow hash. Each service manifest must match its complete runtime wire
profile. The separately retained narrow legacy LSan exception still applies;
this is not a leak-clean result.

The explicit `selected-apk` and `ci` labels request a Debug artifact only.
Its verifier checks the source commit, existing CI certificate, ARM64 ELF,
actual O2 Debug flags, generated Kotlin feature flag and packaged JNI symbols.
An official Android35 Google APIs x86_64 attempt follows on the exact APK;
`runtime-report.json`, including blocked and failed checks, determines the
observed result. A green workflow is not APK-runtime or physical ARM64
acceptance. No formal Release, merge or commercial content is involved.

The opt-in build command, after initializing pinned recursive dependencies
and installing the repository's Java17/SDK/NDK requirements, is:

```sh
SIGNING_STORE_PATH=unused.jks IS_BUILD_SIGNED=false ./gradlew --no-daemon \
  -PpineNativeDebugOptimization=2 -PpineSelectedContent=true assembleFullDebug
```

That local command uses a local Debug signer. CI uses the existing authorized
signer; compare the resulting identity report rather than clearing app data.
The entry is NSP details → **Choose NSP content / 选择 NSP 内容**. It requires
both Kotlin and native opt-in flags; ordinary builds keep it disabled. See
[selection UI](selected-content-ui-v1.md),
[native profile](selected-content-v1.md) and
[service evidence boundaries](selected-service-boundary-v1.md).

BKTR remains rejected with `selected_bktr_unsupported`, even with a matching
Base. A controlled real-AES/UBSan read previously reached zero-length decrypt
and a null destination passed to `memcpy` through the legacy BKTR reader.
Table validation alone does not establish arbitrary guest-read safety, and
PatchManager reconstructs the effective NCA storage later. The minimum future
path is a separately validated selected-only storage implementation (or an
independently reviewed backend fix), covering zero/unaligned/bucket-edge reads,
overflow and Base/Patch provenance, and then testing the reconstructed effective
storage through actual IStorage consumers. Until those gates pass, this
candidate preserves the rejection and the old default route unchanged.

This candidate does not establish NCA signatures, full IVFC verification,
arbitrary rebuilt-NSP compatibility, guest mapping/execution, actual GPU/NCE
behavior, physical-device performance or game-specific persistence/Lua/DFX
behavior. Owner runtime acceptance must be reported separately.
