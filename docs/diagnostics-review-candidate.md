# Diagnostics review candidate

This branch is a review candidate after the v1.2 developer preview. Use the exact
PR head SHA and its CI artifacts when comparing evidence. This change does not
identify the writer behind an ART heap-corruption abort.

## Implementation to review

- Application, selected base Program/CNMT/NCA entry identities are distinct. A
  launch survives diagnostic level/session changes and ends at a game boundary.
  Entry IDs are not full package hashes. Loader slots and actual CNMT roles are
  observations; Patch-in-base does not reject a working merged package.
- Critical events have finite reserved queue admission. Drop reasons, critical
  losses and bounded last-state snapshots remain visible. Normal shutdown drains
  accepted records; abrupt death, contention and disk failure can still lose data.
- API30 process summaries contain anonymous IDs only, coalesce off the UI thread,
  and associate historical records with the platform's last published state.
  Shared private history deduplication and export deduplication avoid repeatedly
  counting the same system exit as new crashes.
- API31 native traces are optional. A bounded task parses a fixed AOSP protobuf
  subset and verifies own UID/PID internally. Only known module basenames,
  relative PCs, Build IDs and known abort-class enums survive. Raw traces,
  registers, memory, mappings, FD/path lists and abort text are never exported.
- UI import/association facts are structured. Duplicate reported-success remains
  distinct from metadata success and actual content-list stream closure. Owned
  AudioCore/Shader errors have a bounded opt-in bridge and unknown fatality.
- The Debug job archives matching unstripped Pine symbols, requiring the same GNU
  Build ID as the packaged ELF and recording exact source/APK/ELF/symbol hashes.

`last_load_stage` retains legacy `last_stage`. `last_guest_stage` is unknown until
a real logging call or an allowlisted DFX progress marker is observed. It does
not imply first guest instruction, Lua initialization, a menu/world transition or
a displayed frame. Controlled marker fixtures do not validate TheSim forwarding.
See [the event contract](diagnostics-event-contract.md) for bounds and field meanings.

## Reproduce without commercial content

Run native/core/content and controlled API/JSON/ADB tests:

```sh
bash tools/tests/run-native-diagnostics.sh
bash tools/tests/run-content-diagnostics.sh
python3 tools/tests/test-native-symbols.py
./gradlew testFullDebugUnitTest processFullReleaseMainManifest assembleFullDebug
```

On Windows, run both `powershell -NoProfile -File tools/tests/pine-diag.Tests.ps1`
and `pwsh -NoProfile -File tools/tests/pine-diag.Tests.ps1`. They use fake ADB and
check UTF-8, selection, unauthorized/disconnected devices, Ctrl+C and filtering.
They are not proof of USB/wireless hardware transport.

For an independently reviewed candidate APK, first reproduce Settings enable,
level/retention changes, preview, DocumentsUI ZIP export and Stop. The exact CI
runtime report must distinguish actual translated-AVD execution from JVM/host
fixtures and retain any blocked observations. A cloud AVD cannot establish game
playability, native GPU behavior or a physical device's exit-trace availability.

Use the collector before launch without filters to retain unknown associations:

```powershell
.\tools\pine-diag.ps1 -ListDevices
.\tools\pine-diag.ps1 -Serial YOUR_DEVICE -OutputDirectory .\pine-review
# Once an observed launch is known:
.\tools\pine-diag.ps1 -Serial YOUR_DEVICE -TitleId APPLICATION_TITLE_ID -LaunchId OBSERVED_UUID
```

After a process death, restarting the app with diagnostics enabled can import
own historical exits/traces. `verified` links the record to its platform-retained
last state, which can be stale or absent; it is not a crash-cause assertion.
Unknown historical records remain visible in unfiltered collection. Raw optional
ADB crash context remains separate and has no automatic live PID/window linkage.
Review it privately before sharing.

## Explicitly deferred or awaiting device evidence

- Physical Android API exit-state/native-trace roundtrip, real game FPS/audio,
  current candidate Base/Update/DLC/BKTR and TheSim:LuaPrint/LuaError forwarding.
- Optional exact full-document background hashes, cancellation/change/failure
  states and safe cache invalidation. Import-time verification cannot stand in
  for a later launch's byte identity; SAF metadata cannot prove immutability.
- Real guest-thread/first-message and submit-versus-present timestamps, broader
  game-side stage protocols and verified Lua/menu/world markers.
- Automatic live ADB crash-window association, memory-writer attribution, and
  separately reviewed CheckJNI/HWASan investigation builds.

No test fixture includes ROMs, tickets, keys, saves or commercial game resources.
