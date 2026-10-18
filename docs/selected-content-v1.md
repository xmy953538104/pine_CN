# Explicit selected native content, version 1

This optional C++ entry creates real Pine `Loader`, `NCA`, `CNMT`, `NPDM` and
`NACP` objects from explicit Catalog selections. It supports an Application
with its own complete Program and selected AOCs, an Application plus a separate
independently complete CTR Patch and AOCs, and an explicitly requested
`StandaloneRebuiltPatch` with no Base loader. The standalone loader retains its
real Patch CNMT and Application parent. Each AOC retains its own AddOnContent
CNMT and PublicData NCA; Data NCAs are rejected for this storage path.

All BKTR candidates are rejected, including those accompanied by a matching
Base. Consequently official Base plus BKTR Update acceptance is **not met**.
The existing BKTR reader has arbitrary-boundary paths that can enter a
zero-length AES decrypt; an independent synthetic actual-AES/UBSan exercise
reported `aes_cipher.cpp:60` through `bktr.cpp:179,109`. Merely validating the
tables does not make every guest read safe. A future selected-only solution
must also cover the effective storage returned after PatchManager reconstructs
an NCA. This implementation does not modify that backend or enable BKTR.

## Entry and selection

Both CMake options default to OFF. Enable `PINE_ENABLE_NSP_CATALOG` and
`PINE_ENABLE_SELECTED_CONTENT` to compile the new entry. Enabling only selected
content is a configuration error. No UI/JNI entry is added. Ordinary
`OS::Execute`, `NspLoader`, FD selection, key loading and Vulkan behavior remain
on their existing path; the common Execute tail is mechanically extracted,
with its original `base_control` diagnostic context as the default.

An internal caller opens one owning `catalog::Source`, scans it with
`catalog::ScanNative(source, providedKeys, options)`, and supplies the exact
values selected from that Catalog:

```cpp
loader::SelectionRequest choice;
choice.profile = catalog::ContentInfoProfile::Modern40; // explicit wire profile
choice.applicationId = applicationId;
choice.primary = {primaryGroupId, programContentId, programIndex, programId};
choice.update = loader::ProgramChoice{patchGroupId, patchContentId,
                                     patchProgramIndex, patchProgramId};
choice.addOnGroups = {aocGroupId1, aocGroupId2};
os.ExecuteSelectedContent(catalog, choice, providedKeys);
```

`groupId` is the `cnmt-sha256:` identity of one exact CNMT member. Program index
is the CNMT Program reference's `idOffset`; Program ID is independently checked
against the selected NCA and NPDM. There is no default index, first member or
highest-version choice. For a rebuilt standalone Patch, set
`mode = SelectedMode::StandaloneRebuiltPatch`, put its true Patch group in
`primary`, supply its explicit Application parent and omit `update`.

`PrepareSelectedContent(catalog, choice, providedKeys)` permits inspection of a
prepared bundle without installing it. `Install(state)` requires an empty,
nonrunning loader/process state and performs all validation and allocation
before its first state mutation. It fills `state.loader`, separate
`state.updateLoader`, and one `state.dlcLoaders` element per selected AOC. The
existing `PatchManager`, `LoadProcessData` and AOC/DataID storage paths consume
these actual objects. AOC PublicData header ID must equal its own CNMT DataID,
the exact ID matched by `OpenDataStorageByDataId`.

The selected profile also requires
`fullDataId == checkedAdd(primaryNacpAocBaseId, fullDataId & AOCTitleIdMask)`
with distinct projected indices. It uses the existing service's shared
`0x7ff` mask so ListAddOnContent, GetAddOnContentBaseId and the full-ID storage
lookup agree. This is only a service compatibility check; the explicit CNMT
parent remains the association proof. The constant moved into a small shared
header without changing the service implementation or value.

Prepared inspection getters expose const Loader views. This is a trusted
internal C++ interface, not an object-capability boundary: the existing Loader
types themselves contain shared mutable Backing pointers. Internal callers
must not mutate them around preparation or installation.

## Validation and source ownership

Preparation performs a fresh native scan of the same owning Source with full
raw-NCA SHA-256 verification. Every selected CNMT and contained content
reference must resolve uniquely with the expected hash, size and actual NCA
type. All metadata must be enumerated; unknown small outer members, unreadable
Meta, ambiguous groups, non-Full install types and unknown profiles are
rejected. This conservative first profile also rejects ticket-bearing
containers: callers must already have the lawful KeyStore needed by encrypted
members. It never searches for keys or treats ticket presence as authorization.

The application, Patch parent, Program index/identity, AOC parents and each
PublicData identity are checked independently. Patch metadata is not rewritten
to Application. Every chosen group's references must be contained and verified;
Delta, sparse, compression-layer, indirection and external-Base forms are not
supported. Both selected base and CTR update Programs must be self-contained;
an absent update file never silently falls back to the Base.

The selected runtime profile also checks:

- NPDM META/ACI0/ACID ranges and identities, the ACID Program-ID interval,
  supported 64-bit address-space flags, stack/resource limits, access-region
  extents and a single safe ThreadInfo capability before invoking real NPDM.
- Exact ExeFS membership, required `rtld`, `main` and `main.npdm`, supported
  optional `sdk`/`subsdk0` through `subsdk7`, bounded NSO segment file and memory
  extents, contiguous aligned mapping, exact LZ4 output, enabled segment hashes,
  bounded RO module strings and dynamic-table ranges. Nonempty ELF64 symbol
  tables have 8-byte alignment and a size divisible by 24; each nonzero name
  index points to a NUL-terminated string inside its declared `.dynstr` extent.
- All four RomFS table ranges and their combined budget, sequential padded
  record boundaries, parent/sibling graph and complete reachability, actual
  hash buckets and collision chains, and nonoverlapping header/metadata/file
  extents. Offset zero is a valid link; `0xffffffff` is the empty sentinel.
  Data may precede or follow metadata if its actual extents do not overlap it.
- A unique Control reference, actual bounded `control.nacp`, exact NACP size,
  supported-language information and terminated title/publisher/version fields.

`selected::Limits` defaults to 128 MiB metadata per structure, 512 MiB mapped
ExeFS per Program, 256 MiB per segment, 65,536 entries/hash buckets, depth 64,
1,024-byte names and 8 MiB total names/paths. Catalog's independent scan/hash
budgets also apply. The runtime-open bridge reuses the unchanged native strict
NCA profile; it does not weaken P0's rejection policy.

Raw selected member bindings retain the same Source. Full hashes are rechecked
after preparation and again immediately before installation. A cached stat or
snapshot token never authorizes a different Backing. The exact read-only
Source-to-Backing bridge throws on short reads and retains shared ownership;
it does not duplicate FD ownership. Source change checks remain active in
runtime reads. This is not an atomic filesystem snapshot: callers must keep
the opened input stable while it is in use.

Diagnostics describe the actual installed primary/update/AOC objects and their
real CNMT roles. `DLC_REGISTERED` occurs after successful installation;
standalone Patch identity uses its typed Patch parent. Diagnostic enablement
does not affect selection or exceptions.
The shared `PATCH_SELECTED` event is emitted after installing an actual selected
Patch, with source `update` or `standalone_patch`, its true CNMT/parent/Program
identities and `status: selected_not_executed`. It does not claim guest execution
or that RomFS has been opened.

## Evidence and limits

`tools/tests/test-selected-content.py` compiles the production selected factory,
NCA/CTR, CNMT, NPDM, NACP, RomFS, PatchManager and diagnostics sources. The default
lane uses a clearly labelled controlled cipher and a rejecting LZ4 stand-in.
The real lane requires clean sources at the repository's exact submodule pins:

```sh
git submodule update --init --depth 1 app/libraries/mbedtls app/libraries/lz4
python3 tools/tests/test-selected-content.py --real-aes --sanitize \
  --schema-check --evidence-dir /tmp/pine-selected-evidence
```

Pinned mbedTLS 2.16.11 (`d895668359eb8ba7536a04f5e2be1e5268c86b33`) and the
production Pine AES wrapper decrypt bytes independently encrypted with OpenSSL
AES-XTS/CTR/ECB and public generated test keys. Pinned LZ4
`ebb370ca83af193212df4dcbadcc5d87bc0de2f0` handles the compressed NSO positive
case. Missing or mismatched dependencies fail the real lane; they are not a
skip. The dedicated workflow requires this lane, sanitizer and Catalog schema
validation and retains exact-commit evidence without producing an APK.
Each lane writes `manifest.json` containing the source commit and dirty status,
dependency pins, exact C/C++ compiler arguments, input/artifact SHA-256 values,
cipher/stub/verification scope and scenario count. Source HEAD, worktree status
and input hashes must remain unchanged during the run. The ordinary Execute
prefix/tail, NspLoader and AOC/fssrv implementation hashes are checked against
the fixed P0 baseline to guard the default path.

The 680-byte `romfs-collision.romfs` is an independent literal oracle, SHA-256
`c7d6619a3d10e89e49b292f1d4c9d10cc4479dc95d094bcad4058ccba8176501`, audited
against switch-tools `22756068dd0ed6ff9734c59cb4f99ebd3f62555b` and frozen locally.
It exercises nested directories, data before metadata and collision links to
offset zero inside an actually selected encrypted Program NCA. Its independent
writer imports no production parser or fixture builder. Other fixtures are
generated content-free NSPs, with no commercial payload, keys or saves.

Only platform/kernel types, `OS::ExecuteLoadedContent` (the launch tail), and
`NcaLoader::LoadExeFs` (guest NSO mapping/execution) are host stand-ins. The
production OS tail has a reviewed mechanical diff. An additional local official
NDK r27 Clang `aarch64-none-linux-android29 -fsyntax-only` run uses all actual
Android/kernel/GPU/Loader headers for OS.cpp with the option disabled and
enabled, the three new selected sources, native adapter, CNMT and NPDM; all
eight translation-unit checks passed. It reuses existing Ninja compiler flags,
does not build/link an APK and does not replace the host stubbed tail with
device execution. These host tests do not execute GPU initialization, guest
code or Android services. They
prove bounded contained-source structure and actual native object selection,
not NSO semantic behavior, RSA/ACID signatures, IVFC tree authentication or
hardware/game execution. No ordinary game, Android build, device launch or
official BKTR update compatibility is claimed.

The actual AOC Count/List and fssrv OpenDataStorageByDataId IPC methods are not
linked or executed by the host harness. Their compatibility evidence consists
of unchanged/source-checked service projection and full-ID rules, actual
installed Loader fields, and real NCA/PatchManager backing reads. It is not a
guest service round-trip test. The existing `Loader::LoadExecutable` and
`NcaLoader::LoadExeFs` mapping/patching behavior, GPU tail and real process
startup remain unexecuted in these host tests.
