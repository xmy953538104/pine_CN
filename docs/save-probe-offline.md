# Offline dedicated SaveData probe verifier

This Python standard-library CLI compares explicitly named local evidence. It does
not call Switch APIs, write a save, search directories, upload files, or change
Diagnostics/Commit/Flush/game-save behavior. The delivered examples and CI evidence
are **synthetic only**. No Owner save or device persistence experiment was available.

```sh
python3 tools/pine-save-probe-verify.py \
  --manifest /local/probe/manifest.json \
  --initial-file /local/probe/initial/dfx_diag_probe_v1.json \
  --restart-file /local/probe/restart/dfx_diag_probe_v1.json \
  --export-zip /local/probe/stopped-export.zip > /local/probe/report.json
```

All paths must be supplied explicitly; the manifest never opens a path. Each
payload file must have exactly the dedicated basename `dfx_diag_probe_v1.json`.
Use a POSIX host with `O_NOFOLLOW` support (Python 3.10+); Linux is tested. Windows
is unsupported. Final-component
symlinks and nonregular inputs are rejected. Caller-selected parent directories
are trusted; this is not a sandbox for arbitrary host paths.

Exit codes: `0` means all described evidence checks matched, `1` means a
contradiction/rejected input, and `2` means incomplete, unknown, or unsupported
evidence. There is no overall persistence-success field. Power-loss durability is
always unknown and is excluded from exit-code calculation. A partial invocation
still returns useful per-stage results with code `2`.

The report contains status/reason codes, byte counts, SHA-256 values and a hash of
the nonce. It omits raw paths, identity values, nonce and payload text. It does not
echo invalid input or OS exception strings. Keep the manifest and complete save
ZIP local: they can contain private paths or unrelated save data. Only generated
noncommercial fixture data belongs in this repository or the host CI artifacts.

## Evidence contract

The versioned [manifest schema](../tools/save-probe/manifest-v1.schema.json),
[synthetic example](../tools/save-probe/example/manifest-v1.example.json) and
[payload schema](../tools/save-probe/payload-v1.schema.json) define the format.
Unknown keys, duplicate JSON keys, invalid IDs and unsafe paths are rejected;
optional missing observations stay missing. Runtime byte/security limits also
apply. `authorization` explicitly restricts the logical key and basename to this
probe. A declaration does not authenticate a file's provenance.

| Report layer | What is checked | What it establishes |
| --- | --- | --- |
| Identity, per stage | Expected vs recorded space/type/user/save owner/program/path, individually | `asserted_match` only; host copies do not prove the original SaveData location |
| Codec | Explicit `pine-probe-json-utf8-v1` | One implemented dedicated format; other/absent codecs do not read payload bodies |
| Writer | Recorded callback, writer completion, close and write ID | Manifest assertions only, with no durability inference |
| Initial | Independently read file, decoded nonce/write ID, exact byte length/hash | `observed_match` against the manifest's expectations |
| Restart | Separately read Get-API output file, same checks, distinct capture/process/write identity and API/previous-stop records | Bytes match; actual process lifecycle and API validity remain unauthenticated assertions |
| ZIP | Exact member, independent bytes/CRC/hash/nonce, declared export source and consistency | Selected member integrity; no archive-wide or snapshot guarantee |

`missing_evidence`, `contradiction`, `unknown`, `unsupported`, `rejected`,
`asserted_match` and `observed_match` are distinct. Failure in one stage does not
replace results from other stages. Matching bytes cannot turn a failed callback or
wrong owner/user/path into a match. Manifest assertions can be fabricated; this
tool does not independently authenticate any device, identity, callback, restart,
export source or stop/coordination claim.

The only supported codec is a strict UTF-8 JSON object with `schema: 1`,
`logical_key: "dfx_diag_probe_v1"`, `write_id`, `nonce`, and fixed
`text: "pine dedicated save probe v1"`. Write IDs/nonces are 32 lowercase hex digits;
the entire file, including whitespace, is at most 1 KiB. Hash/length compare the
original file bytes. Extra/private fields and shared-save objects are rejected.
This is a newly defined probe profile, **not** an inferred game save encoding.
Compressed, binary and shared-container formats remain unsupported until their
actual codec/field extraction is implemented and reviewed. An absent plaintext
nonce in such a format does not establish a write failure.

Restart needs an explicit second file, not the initial file or a hardlink to it.
Capture it from the actual validated Get API in a fresh process and retain its
own manifest record (`source: validated_get_api`, verified signature, prior
process stopped, different process/capture IDs, same write ID). Matching copies
alone cannot prove restart: the lifecycle/source assertions are visibly separate.
For an actual experiment, use a fresh random write ID/nonce per write; a second
round on the same dedicated key helps exclude stale data. The example's fixed
nonce is only for repeatable tests. No Set/GetPersistentString signatures, callback
arguments, or readiness timing are guessed or implemented here.

## ZIP and resource bounds

No extraction or archive-wide CRC check occurs. The verifier parses ZIP metadata
and reads/decompresses exactly the manifest's `zip_member`; unrelated member bodies
are not processed. This permits a large private entry to remain unread, and an
unread corrupt entry does not become an archive-wide integrity claim. Metadata may
include unrelated names; they are not printed.

Limits are 64 KiB manifest, 1 KiB payload/output, 32 MiB physical ZIP, 1 MiB central
directory, 2,048 entries, 4 KiB selected compressed bytes, and 100:1 selected ratio.
Declared size, actual decompressor output, stream termination, local/central
headers, descriptors, length and CRC are checked independently. ZIP64, multidisk,
unsupported target compression, encrypted/symlink/special entries, duplicate or
Unicode-normalized/case aliases, absolute/traversal/backslash/control names,
truncation and overlapping selected ranges are rejected. Only ZIP Store/Deflate
are supported, including Android-style streaming data descriptors. Large ordinary
save archives may exceed these deliberate limits; the CLI never broadens its scope
automatically. It does not read the whole ZIP to hash private entries.

For `export.source: save_management_utils_v79a`, the declared original path is
cross-checked against the fixed default-user subtree and the exact ZIP member.
`program_id` must equal `save_owner_id`, and the ZIP entry retains the uppercase
save-owner prefix produced by 79a. Hex identity fields accept either case and
compare numerically; paths and ZIP members remain exact and case-sensitive.
Other users, space/types or paths contradict a claim that this UI export
covers them. An `explicit_dedicated_probe_archive` can still have matching bytes;
coverage by the existing UI remains unknown. `uncoordinated`/`unknown` export
consistency remains unknown even when every selected byte matches. File metadata
is checked before/after reads, but this does not create a snapshot or prevent all
concurrent-change races.

Record the resolved SaveData program ID used in the path; do not substitute a
CNMT Application ID or a zero-valued request that native resolves via NACP.

## Synthetic reproduction and CI

```sh
python3 tools/tests/test-save-probe-verifier.py
python3 tools/tests/make-save-probe-fixture.py --output /tmp/pine-probe-synthetic
python3 tools/pine-save-probe-verify.py \
  --manifest /tmp/pine-probe-synthetic/manifest.json \
  --initial-file /tmp/pine-probe-synthetic/initial/dfx_diag_probe_v1.json \
  --restart-file /tmp/pine-probe-synthetic/restart/dfx_diag_probe_v1.json \
  --export-zip /tmp/pine-probe-synthetic/stopped.zip
```

The generator requires a new/empty destination. It fabricates all lifecycle
records and includes one unrelated synthetic ZIP body to exercise exact-member
scope. The [dedicated host workflow](../.github/workflows/save-probe-offline.yml)
checks the exact source SHA and uploads only its synthetic manifest/files/ZIP,
report, schemas, Python version and test log. It has read-only repository
permissions, no signing credentials, APK build, Release publication or main merge.

## Fixed source boundaries (Owner-tested 79a)

- UI export selects `P/switch/nand/user/save/0000000000000000/00000000000000000000000000000001`,
  optionally plus the UI save-owner field, and walks that subtree. It is not a
  snapshot and has no writer coordination. Its nullable output stream can still
  yield a success Toast; this host-only change leaves that Android issue present.
  The destination ZIP must be verified locally. [SaveManagementUtils.kt](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/java/emu/skyline/utils/SaveManagementUtils.kt#L38-L172)
- UI `titleId` comes from NACP save owner, not Application CNMT ID. Native requests
  can use other space/type/user/explicit program paths. This verifier records those
  identities separately and never derives them from a filename or displayed title.
  [RomFile.kt](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/java/emu/skyline/loader/RomFile.kt#L219-L239),
  [IFileSystemProxy.cpp](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/services/fssrv/IFileSystemProxy.cpp#L24-L78)
- Commit/Flush report success without storage synchronization; OsBacking uses
  `pwrite64` and `close`, without `fsync`/`fdatasync` in this path. Callback, close,
  exported bytes or restart readback therefore do not prove power-loss durability.
  [IFileSystem.cpp](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/services/fssrv/IFileSystem.cpp#L93-L105),
  [IFile.cpp](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/services/fssrv/IFile.cpp#L61-L63),
  [OsBacking](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/vfs/os_backing.cpp#L18-L55)
- DiagnosticExport has a separate fixed log allowlist; SaveData is not added to it.
  [DiagnosticExport.kt](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/java/emu/skyline/diagnostics/DiagnosticExport.kt#L18-L36)
