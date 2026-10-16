# Diagnostics v1 implementation baseline

Baseline checked against origin/main: f665512f52fa6c3b7e726299d0c23ff3abe09d8c.
Initial worktree clean; no repository AGENTS.md or .agents/skills found.

Implement an independent bounded async native sink, JNI-configured opt-in settings,
allowlisted export, actual content decision events, guest-message forwarding and
Windows ADB streaming. Preserve first-enabled UI update selection, last-match URI
fallback, base romType for update/DLC, updateFd > 0, NSP overwrite semantics and
exception propagation. Observe actual ReadUnchecked result; Backing::Read's return
is not a successful-byte count. Record CTR/BKTR where ReadRomFs branches.

No JSON, allocations, locks, JNI or joins in raw signal handlers. DFX events require
actual guest messages. Closed-source Lua stacks cannot be invented.

Build baseline: Gradle 8.6, Java 17, AGP 8.4.0, NDK 27.3.13750724, compileSdk 35,
arm64-v8a only, 21 initially uninitialized submodules. Existing CI builds release;
the additional workflow builds Debug and runs native, JVM and Windows shell tests.
Main is not merged by this work. A clearly marked pre-release may be published only after the authorized quality checks and exact-commit artifact verification.

Runtime evidence and final acceptance results are recorded separately; synthetic
input and JVM tests are not evidence of running the ARM64 APK.

Build review updates: the root Gradle8.6 wrapper JAR was replaced with the exact
published Gradle8.6 JAR and its distribution SHA-256 pinned. CI validates that root
wrapper before obtaining native CMake submodules. Oboe's unused sample Gradle
wrapper (an old unverifiable binary) is never invoked; it is not blindly allowlisted
and root validation is not disabled. Perfetto uses Google's GitHub mirror at the
same original pinned commit. Shallow submodule fetching preserves all pins.

The Debug application has a separate `.diagnostics` ID and preview version.
Preview publication is an explicit annotated-tag action, bound to a successful
exact-head Diagnostics Debug run and the tested APK digest. The workflow uploads
only that APK, compares downloaded draft assets/evidence, and publishes a
pre-release after verification. No tag or public release is created by ordinary
branch pushes.
