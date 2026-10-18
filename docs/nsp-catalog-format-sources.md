# NSP catalog P0 format evidence

Reviewed 2026-10-18 UTC against the fixed primary sources below. This document
defines a read-only inspection profile and independent synthetic oracles. It does
not certify execution, NCA signatures, commercial packages, keys, or save data.
No applicable `AGENTS.md` was found in the workspace/repository search.

## Fixed primary sources

The links point to actual format definitions or parser/service code, not a
secondary format summary. Atmosphere's NCM parser is useful evidence for byte
layout, but is not itself an untrusted-input bounds validator.

| Evidence | Fixed source |
| --- | --- |
| Packaged CNMT header, extended headers, table addressing | [Atmosphere 6b0c077, ncm_content_meta.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta.hpp#L34-L183) |
| Header storage/install/committed fields and install-aware metadata key | [Atmosphere 6b0c077, header](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta.hpp#L62-L78), [GetKey](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta.hpp#L224-L226), [ContentMetaKey/default Full](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_meta_key.hpp), [StorageId](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_storage_id.hpp) |
| Digest followed by ContentInfo | [Atmosphere 6b0c077, ncm_content_info_data.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_info_data.hpp#L24-L54) |
| Current ContentInfo: 40-bit size and attributes | [Atmosphere 6b0c077, ncm_content_info.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_info.hpp#L23-L68) |
| Historical ContentInfo: 48-bit size | [Atmosphere 77fe5cf, ncm_content_info.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/77fe5cf6f574a0df4198015af672acfad4b88acb/libraries/libstratosphere/include/stratosphere/ncm/ncm_content_info.hpp#L22-L61) |
| Exact 15.0.0 change: size/attributes, AOC, DataPatch | [libnx 7caf74a, Update ncm_types to 15.0.0](https://github.com/switchbrew/libnx/commit/7caf74af7611df5ad2e9deae8a5abbcfdc68d99a) |
| Current DataPatch and legacy/current AOC field definitions | [libnx 20cbcfd, ncm_types.h](https://github.com/switchbrew/libnx/blob/20cbcfd90567f7bf84df7725f27e345e2164fa6b/nx/include/switch/services/ncm_types.h#L127-L166) |
| NCA/FS structures, enums, section units | [Atmosphere 6b0c077, fssystem_nca_header.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/fssystem/fssystem_nca_header.hpp#L33-L302) |
| FS-header SHA256 validation | [Atmosphere 6b0c077, fssystem_nca_reader.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/source/fssystem/fssystem_nca_reader.cpp#L390-L438) |
| Two-layer SHA256 section handling | [Atmosphere 6b0c077, fssystem_nca_file_system_driver.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/source/fssystem/fssystem_nca_file_system_driver.cpp#L1040-L1092) |
| SHA256 table/master and final partial-block hashing | [Atmosphere 6b0c077, fssystem_hierarchical_sha256_storage.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/source/fssystem/fssystem_hierarchical_sha256_storage.cpp#L37-L119) |
| PFS0 sizes and offsets | [hactool 1d64a83, pfs0.h](https://github.com/SciresM/hactool/blob/1d64a83450e025622f3468c28fc4164dad2c5ef6/pfs0.h#L10-L65), [actual file seek](https://github.com/SciresM/hactool/blob/1d64a83450e025622f3468c28fc4164dad2c5ef6/pfs0.c#L45-L50) |
| Program index obtained from Program ContentInfo.idOffset | [Atmosphere 6b0c077, pgl_srv_shell_host_utils.cpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/source/pgl/srv/pgl_srv_shell_host_utils.cpp#L215-L231) |
| Program ID/index mapping is a separate explicit map | [Atmosphere 6b0c077, fs_program_index_map_info.hpp](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/fs/fs_program_index_map_info.hpp#L23-L31), [map lookup](https://github.com/Atmosphere-NX/Atmosphere/blob/6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8/libraries/libstratosphere/include/stratosphere/fssrv/impl/fssrv_impl_program_index_map_info_manager.hpp#L68-L94) |

The original `Thealexbarney/LibHac` repository returned GitHub 404 on this review
date. No unverified mirror or remembered LibHac layout is used as evidence.

## PFS0

All integers below are little-endian. Header size is `0x10`: magic `PFS0`,
u32 entry count at `+4`, u32 string-table size at `+8`, reserved u32 at `+0xC`.
Each entry is `0x18`: u64 offset at `+0`, u64 size at `+8`, u32 name offset
at `+0x10`, reserved u32 at `+0x14`.

String-table start is `0x10 + entryCount*0x18`. File-data start is that value
plus the declared string-table size. Entry offsets are relative to file-data
start, not container start. Padding is represented by declared table/data
offsets; do not add an assumed alignment after the string table.

Validate checked arithmetic, source containment, bounded NUL termination, flat
nonempty names, duplicates, overlapping nonempty extents, and count/string
budgets. File order and file-name stems are discovery/lookup information, not
content identity or association proof.

## Packaged CNMT

`PackagedContentMetaHeader` is `0x20` bytes, not the installed
`ContentMetaHeader` (`0x8`). Relevant offsets are:

| Offset | Width | Field |
| --- | --- | --- |
| 0x00 | 8 | title/meta ID |
| 0x08 | 4 | title/meta version; not a format-schema version |
| 0x0C | 1 | meta type |
| 0x0D | 1 | current platform; formerly reserved |
| 0x0E | 2 | extended-header size |
| 0x10 | 2 | content count |
| 0x12 | 2 | content-meta count |
| 0x14 | 1 | meta attributes |
| 0x15 | 1 | storage ID; not reserved |
| 0x16 | 1 | content install type; not reserved |
| 0x17 | 1 | committed flag; raw byte retained |
| 0x18 | 4 | required download system version |
| 0x1C | 4 | reserved bytes |

The catalog preserves the complete raw `0x20` header plus each field above.
Nonzero storage/install/committed values must not hide bounded content and
metadata-reference tables. Unknown enum values remain raw observations;
`committed` is decoded as a boolean only for raw values 0 and 1.
Installation policy is separate: Full is 0, FragmentOnly is 1, and the known
Unknown enum is 7. Non-Full groups remain parsed but unsupported as complete
Programs. Metadata identity includes install type, as the primary `GetKey`
does. A `ContentMetaInfo` has no install-type byte; its `ToKey()` uses `Make`'s
default Full selector. `resolution_install_type=0` documents that accessor
policy and is not presented as a wire field. A FragmentOnly CNMT must not
satisfy a Full metadata reference.

`PackagedContentInfo` is `0x38`: SHA256 bytes `[0,0x20)`, content-ID
bytes `[0x20,0x30)`, size/profile region `[0x30,0x36)`, content type
at `0x36`, idOffset at `0x37`. `ContentMetaInfo` is `0x10`: u64 ID,
u32 version, u8 meta type, u8 attributes, two reserved bytes.

The primary accessor's ordering is:

```text
contents = 0x20 + extendedHeaderSize
metaInfos = contents + contentCount * 0x38
extendedData = metaInfos + contentMetaCount * 0x10
digest = extendedData + declaredExtendedDataSize
minimumEnd = digest + 0x20
```

Every product/addition/range must be checked before reading. A nonzero
`contentMetaCount` must not cause re-reading from `contents`. The catalog
preserves the meta references as their own records, including unresolved,
missing, duplicate or unknown types. These references are not a generic parent
table and do not prove AOC membership.

The examined primary accessor exposes the trailer digest without making a
SHA256-of-current-bytes comparison a structural parse prerequisite. P0 should
retain the trailer and any comparison result separately; a checksum mismatch
must not hide otherwise bounded content/reference tables. A comparison is not
NCA signature verification. No normative requirement that ContentId equals
the first 16 bytes of the declared content hash was established by these
sources; keep the ID and expected SHA256 independent. Full expected SHA256
can locate an arbitrarily renamed NCA when whole-file verification is enabled.

### ContentInfo size profile

Before the 15.0.0 change, the six bytes at `+0x30` encode a 48-bit size. In
the current profile the first five bytes encode a 40-bit size and `+0x35`
is the attributes byte. The libnx 15.0.0 commit and Atmosphere before/after
definitions independently establish the change.

The wire header does not carry a reliable schema discriminator for this.
Platform Nx is zero, indistinguishable from a historical reserved zero.
The title version, minimum required system version, and a different title's
extended-header size do not safely identify this record's size interpretation.

Recommended P0 policy: require or report an explicit `legacy-48` /
`modern-40` profile. An unknown profile can retain all six raw bytes; if the
last byte is zero both interpretations yield the same size, otherwise keep size
ambiguous. Never silently turn modern attributes into the high byte of a
48-bit size or truncate an explicitly selected legacy size.

### Extended headers and explicit relations

Offsets below are relative to the extended header at CNMT `+0x20`.
The size field defines where the tables begin. Unknown sizes/types may be
listed, but must not be accepted as a known relation layout.

| Meta type | Known size | Fields and relation |
| --- | --- | --- |
| Application 0x80 | 0x10 | patchId u64 @0; requiredSystem u32 @8; requiredApplication u32 @0xC. The first u64 is a patch relation, not a parent. Application identity is the header ID. |
| Patch 0x81 | 0x18 | applicationId u64 @0; requiredSystem u32 @8; extendedDataSize u32 @0xC; reserved[8] @0x10. |
| AOC 0x82, legacy 1.0.0–14.1.2 | 0x10 | applicationId u64 @0; requiredApplication u32 @8; reserved[4] @0xC. |
| AOC 0x82, 15.0.0+ | 0x18 | same application/version; accessibilities u8 @0xC; reserved[3]; dataPatchId u64 @0x10. |
| Delta 0x83 | 0x10 | applicationId u64 @0; extendedDataSize u32 @8; reserved u32 @0xC. |
| DataPatch 0x84, introduced in 15.0.0 | 0x20 | dataId u64 @0; applicationId u64 @8; requiredApplication u32 @0x10; extendedDataSize u32 @0x14; reserved[8] @0x18. |

libnx documents Application.requiredApplicationVersion as 9.0.0+; the same
word was previously padding. This does not change its `0x10` extent.
AOC `0x10` / `0x18` can be recognized by their actual declared sizes.
DataPatch's first u64 is the associated data/AOC ID; application parent is the
second u64. A single generic optional-header struct is therefore incorrect.

Patch/Delta/DataPatch extended data can remain opaque in P0, with its declared
bounded extent retained. Their role alone never proves a standalone executable.
Unknown extended headers must not trigger an ID mask or title-name heuristic.

### idOffset, programIndex and NCA program ID

Retain idOffset for every content record. For a **Program** record, the fixed
Atmosphere PGL implementation directly uses idOffset as programIndex; record
that provenance when populating the Program descriptor. It is not the PFS
entry ordinal or the ordinal of a Program record in the table.

The NCA header program ID remains a separate observed value. Do not derive an
application parent or program index from its low bits. Control/Data idOffsets
remain content selector offsets unless separately supported. Duplicate Programs
at one index or multiple indices require explicit ambiguity reporting; do not
copy PGL's lowest-idOffset default selection into this catalog.

## Plaintext NCA3 inspection profile

`NcaHeader` is `0x400` bytes; four `NcaFsHeader` blocks of `0x200`
follow at `0x400+i*0x200`, giving a `0xC00` combined header area.

| NCA-relative offset | Field |
| --- | --- |
| 0x200 | magic NCA3 |
| 0x204 / 0x205 | distribution / NCA content type |
| 0x208 | content size u64 |
| 0x210 | program ID u64 |
| 0x218 | content index u32, not a program-index proof |
| 0x230 | rights ID, 0x10 bytes |
| 0x240+i*0x10 | FS start sector u32, end sector u32, hash sectors u32, reserved u32 |
| 0x280+i*0x20 | SHA256 of the corresponding full 0x200-byte FS header |

Sector unit is `0x200`. Validate range/ordering/containment and section
overlaps. NCA content types are Program=0, Meta=1, Control=2, Manual=3,
Data=4, PublicData=5. They differ from CNMT content types.

FS-header fields are u16 version @0, **FS type @2**, **hash type @3**,
**encryption type @4**, metadata-hash type @5. FS type: RomFs=0, PartitionFs=1.
Hash types: None=1, hierarchical SHA256=2, hierarchical integrity=3,
AutoSha3=4, hierarchical SHA3-256=5, integrity SHA3=6. Encryption:
None=1, XTS=2, CTR=3, CTR-Ex=4, CTR skip-hash=5, CTR-Ex skip-hash=6.
The existing repository's field called `fsType=2/3` denotes the byte that the
primary source calls **hash type**; it must not define the new parser's types.

For hash type 2, SHA256 hash data starts at FS `+8`: master hash[32],
u32 block size @0x28, u32 layer count @0x2C, region pairs
(u64 offset, u64 size) starting @0x30 with stride 0x10. Region offsets are
relative to the decoded section. The generic header's target is the last
declared region, but the examined Atmosphere SHA256 driver explicitly
requires two layers: region[0] hash table and region[1] target data. A P0
two-layer profile should enforce that constraint before using region[1].

An honest P0 may locate plain Meta/PFS only for supported hash/encryption/
layer layouts. Encrypted headers, encrypted sections, unsupported magic,
hash algorithms, metadata hashes, sparse/compression/BKTR indirection must
remain recorded with explicit unreadable/unsupported/dependency state.
They must not be scanned as if their physical bytes were a PFS.

Hash observations are independent: FS-header hash, SHA256 master/table/data
checks, whole-file CNMT reference hash, and NCA signatures are distinct.
Computing a whole-file hash does not validate the inner chain; matching a
filename stem validates neither. Even all byte hashes matching is not proof
of a valid RSA signature or runnable Program. The synthetic NCA below is
intentionally unsigned.

A future lawful native/host decoder adapter should supply decoded bounded
section views and explicit key/signature policy while retaining the original
raw NCA reader for CNMT whole-file hashes. Preserve all failures and actual
byte counts. Reusing an authorized existing NCA backend is preferable to
adding catalog-owned key discovery or crypto code. Catalog inspection must
not silently route unsupported contents into the runtime loader.

## Independent golden bytes and minimal plaintext fixture

This oracle was authored before reading the production CNMT parser, using
the source sizes above. It imports no production reader/writer. Both counts
are one. Bytes `[0,0x80)` of the 0xA0-byte Patch CNMT are this **literal**:

```text
00583412000000014433221181001800
01000100000000000000000000000000
00503412000000010000000000000000
00000000000000000001020304050607
08090a0b0c0d0e0f1011121314151617
18191a1b1c1d1e1fa0a1a2a3a4a5a6a7
a8a9aaabacadaeaf7856341200000107
1122334455667788d4c3b2a182040000
```

Append `SHA256(body)` as the last 0x20 bytes. The resulting complete file's
SHA256 is `53adc31ca6d927c2d01cf32e5ec3381f79c3447c455936de36868c226467fbc4`.

| Independent expected value | Value |
| --- | --- |
| CNMT ID / version / role | 0100000012345800 / 0x11223344 / Patch |
| Explicit application parent | 0100000012345000 |
| Content table / meta table / extended data / digest offsets | 0x38 / **0x70** / 0x80 / 0x80 |
| Content ID / size / content type / idOffset | a0a1a2a3a4a5a6a7a8a9aaabacadaeaf / 0x12345678 / Program=1 / 7 |
| ContentMeta ID / version / type / attributes | **8877665544332211** / 0xA1B2C3D4 / AOC=0x82 / 4 |

A reader using the old incorrect meta-table offset instead reads the content
hash's first bytes as the sentinel ID, so a reader/writer sharing that bug
cannot satisfy this literal oracle.

The separate `tests/catalog/fixtures/header-fields-sentinel.cnmt` literal
sets header `+0x14..+0x17` to `04 05 01 01` (attributes, storage ID,
FragmentOnly install type, committed), plus download version `0x04030201`.
Its content and meta tables remain nonempty at `0x38` and `0x70`, with the
same two distinct sentinel IDs. Its full SHA256 is
`3852a0f68ef82426946fed951dfc2a05c24de87a369eddfe01950371ea1e5391`.
The independent real NSP wrapper's SHA256 is
`817957479f04f30cff7ffaec26718842a23697baf8d15dbfb15b27cbb4c0df3f`.
`make_header_fields_golden.py` imports no production reader or fixture writer.
Both direct `ParseCnmt` and the complete NSP/PFS/Meta/CNMT path must retain
both tables and these nonzero fields. FragmentOnly stays structurally parsed
while complete-Program support is explicitly unsupported.

A second policy oracle changes CNMT absolute `0x6C` to `0x34` and
`0x6D` to `0xA5`, then recomputes its trailer. The six size/profile bytes
are `78 56 34 12 34 A5`: modern size `0x3412345678`, raw attributes
`0xA5`; legacy size `0xA53412345678`. Full-file SHA256 is
`6e33129ca27d4b9fe91f16a3df6695ce583701bbe0f290a31a8940b76d9fd26b`.
Unknown profile must retain ambiguity. This is a parser oracle, not a claim
that either huge referenced content is present.

Minimal genuine plaintext NCA/PFS wrapping recipe:

1. Make a 0x200-byte PFS data region. PFS header: count=1,
   stringTableSize=0x18. Entry: data offset=0, size=0xA0, name offset=0.
   Put `fixture.cnmt\0` at PFS+0x28, the CNMT at PFS+0x40; pad with zeroes.
2. Make a 0x1000-byte NCA3. contentType=Meta, contentSize=0x1000,
   programId=0100000012345800, sdkAddonVersion=0x000F0000.
   First FS info: sectors [6,8), all other sections absent.
3. FS header 0 at NCA+0x400: version=2, fsType=PartitionFs(1),
   hashType=hierarchicalSHA256(2), encryption=None(1), metadataHashType=0.
   blockSize=0x200, layerCount=2. Region[0]=(0,0x20);
   region[1]=(0x200,0x200). All sparse/compression/patch structures are zero.
4. Write SHA256(PFS data region) at NCA+0xC00. Put SHA256(that 32-byte
   table) in FS+8. Put the PFS data at NCA+0xE00. Put SHA256(full FS header)
   at NCA+0x280. Signatures and key area remain zero; report unsigned.
5. Wrap the NCA as one outer PFS0 member named
   `851a1ad4dd01f997f7e3772f61683684.cnmt.nca`. Name table size is 0x38,
   outer data start is 0x60, NCA offset=0, size=0x1000.

NCA SHA256:
`851a1ad4dd01f997f7e3772f616836841766f568c7d70f223d6a9507aa22dadc`.
Outer NSP size=4192 and SHA256:
`f1b7c8d337cb6d975cd7b2065d415169f5145a893f3493b2f28da6327533c14b`.

Expected end-to-end result: one Patch group with the sentinel meta reference,
a missing referenced Program, no runnable claim, no signature-verification
claim. This exercises NSP -> NCA3 -> FS header -> PFS -> CNMT -> both tables.
The temporary independently generated artifacts and their JSON expectations
are in `/tmp/pine-nsp-format-review/`; the literal/recipe above is the
portable oracle. Do not count their existence as a parser-test pass.

The independent review harness in
`/tmp/pine-nsp-format-review/review_harness.cpp` was then compiled directly
with the actual `wire.cpp` and `catalog.cpp` using
`g++ -std=c++17 -Wall -Wextra -Werror`. It passed the literal Patch/sentinel
assertions, modern/legacy/unknown size-profile assertions and the complete
plaintext NSP-to-CNMT path, including Program index 7 and the missing content
reference. This is format-parser evidence only; no commercial content or
runtime launch was used. Re-run after changes to the parser.

Additional independent review checks passed against the evolving actual parser:

- Application/Patch/legacy AOC/current AOC/Delta/DataPatch relation fields and
  extended-data boundaries; current AOC padding at absolute 0x2D; short Patch
  extended-header refusal; digest mismatch preserving both bounded tables.
- The real host CLI, built through `tools/build-nsp-catalog.sh`, enumerated the
  independent plaintext NSP and produced the expected 0x70 sentinel offset.
- A plaintext section with a nonzero patch indirect-table extent was retained
  as unsupported/requiring a base and was not exposed as a direct PFS.
- Whole-NCA hashing stayed at zero bytes when its budget was exceeded, even
  when metadata inspection had already skipped the member. A cumulative Meta
  PFS budget failure reported incomplete enumeration instead of claiming a
  successful complete catalog.

Those checks used only independent synthetic bytes under `/tmp`; production
source was not edited by the independent reviewer. Inner section-data hash
verification and NCA signature verification remain explicitly unperformed.

## Independent review gates

Before P0 acceptance, inspect and run the actual production parser against
the literal oracle, not only its own fixture writer. Include both size
profiles, unknown-profile ambiguity, legacy/current AOC, invalid/short
extended headers, two distinct DataPatch relation IDs, and multiple
Program idOffsets.

Keep discovery, parse/readability, reference consistency, byte-hash
verification, section support/base dependence, and runtime capability separate.
Retain malformed Meta/CNMT members with structured failures. One malformed
member must not hide unrelated members. Every successful claim must follow
the checks actually run, and runnable stays unproven without runtime evidence.

Enforce total metadata/CNMT/hash/entry work budgets across the scan in
addition to per-table limits. Source mutation/truncation, short reads,
duplicate names/IDs, missing references and parent mismatch require
deterministic outcomes. Diagnostics settings must not affect association.
