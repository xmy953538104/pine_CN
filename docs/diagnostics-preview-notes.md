This is a Diagnostics v1 developer preview, packaged as a Debug APK for owner testing.
It installs alongside Pine and the old diagnostics app as **Pine Diagnostics Preview** (`io.github.pine.emu.diagnostics.preview`),
version `2.1.2-diagnostics-v1`. Diagnostics is off by default.

The old and initial fixed CI APKs have different Debug signing certificates.
This fresh application ID avoids requiring an uninstall or clearing old data.
It does not migrate another app's private settings, files, URI grants or exit
history. Configure lawful local content in the new app; do not share keys.
Continuous in-place Debug updates still require a stable development signer.

This revision repairs the identified VMA mapped-allocation contract and safe
failure for missing/revoked SAF content, including ACTION_VIEW metadata entry.
Controlled regressions pass; restored device game startup remains unverified.

It adds bounded asynchronous JSONL/PineDiag events, actual content selection and
storage observations, guest message/DFX forwarding, developer settings, redacted
export and Windows ADB collection. Main and emulation process logs are isolated.
Normal storage reads sample 1/128; detailed mode samples 1/16; known short reads
and exceptions are unsampled. BKTR byte counts remain explicitly unknown when the
legacy implementation cannot prove a real byte count.

This preview has not passed physical ARM64, commercial-game or FPS/audio regression
acceptance. Closed-source Lua stacks require game-side logs or a diagnostic bridge.
Use your own lawful game content and keys; none are included. The exact CI, APK
checksum, attempted runtime checks and remaining limitations are attached below.

Owner instructions: `docs/diagnostics-validation.md` and `tools/README-diagnostics.md`.
The feature branch remains in draft PR #2; main has not been merged.
