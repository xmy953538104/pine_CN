This is a Diagnostics v1 developer preview, packaged as a Debug APK for owner testing.
It installs alongside Pine as **Pine Diagnostics** (`io.github.pine.emu.diagnostics`),
version `2.1.2-diagnostics-v1`. Diagnostics is off by default.

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
