# Run a new observer against an existing APK

The application candidate remains PR#4/source79a4d1a2. This independent tool
branch never rebuilds or re-signs it. `.github/workflows/diagnostics-runtime-input.yml`
provides a manual entry with four required inputs: full `apk_source_commit`,
APK byte `apk_sha256` (not artifact ZIP digest), producer `run_id`, and APK
`artifact_id`. The checkout's observer SHA is recorded separately.

The entry requires a completed successful same-repository Diagnostics producer,
exact artifact ID/name/run/source and expiry, then downloads only that existing
artifact. It rehashes the APK and packaged ELF, checks ARM64/DEX source binding
and actual package/minSDK/Debug metadata, and runs official apksigner against
the existing70ed public certificate pin. It does not load a keystore or add
Secrets/permissions; original APK/build manifests remain untouched. The artifact
ZIP digest is labeled GitHub-reported, rather than falsely claiming an independent
ZIP rehash. A checksum/signature/input check is not an app execution result.

For a draft same-repository PR, a public `tools/ci/runtime-input.json` with these
four values and the `ci` + `verify-existing-apk` label provides an input-qualification
run only. This lets tools/docs changes consume an exact original APK even before
a workflow_dispatch UI is available on the default branch. Future manual runs
can supply other immutable targets without editing the file. Unrelated labels
cannot cancel existing evidence.

`run_runtime=false` is the default and never creates an AVD/starts Pine. Only an
explicit manual `run_runtime=true` invokes the current official Android observer
with the bound original APK source. It requires hardware acceleration and never
changes KVM permissions/groups, bypasses Android security, or falls back to an
unqualified software emulator. The known current KVM restriction remains a real
external block; do not use this entry to repeat that same failed configuration.
Use runtime only after an authorized capable host is available. Its report is
saved separately from input qualification, including blocked/failed outcomes.

There is no Gradle/NDK/CMake/submodule/signing-build step in this workflow.
Native/JVM/fake-ADB tests remain in the normal fast suite. No physical-device,
game-speed, Lua bridge or menu/SAF success follows from the qualification artifact.
