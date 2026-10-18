# Read-only Multi-title NSP Catalog v1

P0 provides an independent native catalog, a host CLI and an isolated Pine decoder bridge. It does not change ordinary `NspLoader` selection, JNI/UI import, Diagnostics settings, Vulkan defaults, AOC registration or launch behavior. Catalog output is input for a future explicit selection interface; it does not make a multi-title NSP runnable.

## Run the host tool

Only a C++20 compiler and the standard/POSIX libraries are required. No keys, Android SDK, CMake, APK, signing credentials or game files are needed for the synthetic checks.

```sh
bash tools/build-nsp-catalog.sh /tmp/nsp-catalog
/tmp/nsp-catalog --profile modern-40 --verify-hashes /path/to/authorized-input.nsp > /tmp/catalog.json
python3 tools/tests/test-nsp-catalog.py
python3 tools/tests/test-nsp-catalog-native.py
python3 tools/generate-nsp-catalog-example.py --output-dir /tmp/catalog-evidence
```

The additional real-AES host check requires the repository's exact pinned mbedTLS sources and OpenSSL development headers. It compiles these sources locally; it does not install a system crypto library or use commercial keys:

```sh
git submodule update --init --depth 1 app/libraries/mbedtls
python3 tools/tests/test-nsp-catalog-native.py --real-aes --mbedtls-root app/libraries/mbedtls
```

The CLI opens its input read-only and nonblocking before `fstat` rejects non-regular files, so a FIFO without a writer cannot hang the initial open. It rejects a symlink as the final path component, uses exact `pread` byte counts, and never modifies the container. Exit 0 means a structured catalog was produced; inspect each member/group status for failures. Exit 2 produces structured `fatal` JSON for an invalid container or option. Exit 3 means no complete result was produced. Member failures are retained alongside valid groups.

The host inspector supports plaintext **NCA3** headers and unencrypted version-2 PartitionFs sections with exactly two hierarchical SHA256 layers. It enumerates every Meta NCA and tries every Meta-PFS member at least `0x20` bytes as a CNMT candidate, including arbitrarily renamed files; `.cnmt` members are tried even when truncated. It checks ranges, table counts, names, duplicate names, overlaps and filesystem-header hash linkage. It does not verify the section's master/table/data hash chain, NSO/NPDM contents, RomFS integrity, signatures or executable capability.

Encrypted headers/sections and unsupported formats remain discovered/unknown/unsupported. An arbitrary member name is not evidence that it is non-NCA: every root member at least `0xc00` bytes is tried as a candidate, and plaintext NCA magic is detected regardless of filename. Smaller unclassified files remain in `other_entries`; any unclassified member keeps `all_metadata_enumerated` false. No keys are sought or synthesized. Plaintext fixtures are not evidence of support for an owner's encrypted NSP.

## Wire profiles and identity

CNMT has no reliable self-describing field for the pre/post-15.0.0 `ContentInfo` size layout. Select the source-appropriate profile explicitly:

| Profile | Interpretation of the six bytes at record `+0x30` |
| --- | --- |
| `unknown` (default) | Preserve all six raw bytes; decoded size and attributes are null. |
| `legacy-48` | All six bytes form a little-endian size. |
| `modern-40` | Five little-endian size bytes followed by content attributes. |

Title version and platform 0 do not choose a profile. An unknown profile can still expose common header fields, safe table extents and references; size consistency remains unknown. Nonzero modern content attributes are preserved and marked unsupported.

`packaged_header` retains all 32 raw header bytes and the fixed-source interpretation of platform, meta attributes, storage ID (`+0x15`), install type (`+0x16`), committed (`+0x17`) and required download system version. These three nonzero bytes are not reserved. Unknown enum values are preserved without hiding references, and `committed` is a decoded boolean only for raw 0/1. Non-Full installation types remain parsed with unsupported Program capability. Metadata identity includes install type; metadata-reference resolution uses the primary accessor's explicit default Full selector, not a fabricated wire field. The independent `header-fields-sentinel.cnmt` and actual NSP golden contain nonzero storage/install/committed fields and both nonempty tables.

Application's first extended-header ID is `patch_id`, not a parent. Patch and AOC retain their explicit `application_id`; DataPatch has distinct `data_id` and `application_id`. Delta and DataPatch identity can be enumerated while their execution/data-patching support remains unsupported. Patch/Delta/DataPatch extended-data lengths are checked exactly; nonempty extended data is opaque and unsupported. Unknown roles, platforms, layouts and extended-header sizes are never guessed.

Program descriptors retain CNMT `id_offset`, `program_index` with evidence `cnmt_program_id_offset`, and the separate actual NCA `program_id`. No ID mask or filename derives a parent. Multiple Programs or multiple CNMTs with the same title/version/role are explicit ambiguities; no candidate is chosen by file order, lowest index or highest version. AOC Data and PublicData NCA types remain distinct; the current runtime Data limitation is reported separately.

`Group.id` is SHA256 of complete CNMT bytes. It identifies metadata bytes, not authenticated content. With `--verify-hashes`, an NCA member ID is its actual complete raw NCA SHA256, and full CNMT expected hashes can locate members after arbitrary renaming. A Content ID is retained as a reference identifier; equality with a SHA256 prefix is not imposed as a format rule. Without hashing, filename Content IDs are only unverified location hints. Identical duplicate members and conflicting hints remain ambiguous.

The container token is a snapshot of the opened file's device/inode/size/mtime/ctime. The opened FD and named inode are checked at scan completion and serialization. `Catalog` retains its owning `Source`; callers must call `CheckUnchanged()` again before using descriptors. This rejects truncation, mutation and replacement but is not a content-addressed identity, immutable snapshot, authenticated cache key or authorization token. Later selection must keep/recheck the same source and confirm the chosen members. Reader implementations without a snapshot token report identity `unknown`.

## Interpret states separately

| Field | What it establishes |
| --- | --- |
| `physical_entry_scan_complete` | The outer PFS directory was enumerated. |
| `bounded_scan_complete` | The configured scan and association budgets did not stop work. |
| `all_metadata_enumerated` | No encrypted, unreadable, unknown, unclassified or budget-skipped metadata candidate remains. |
| `reference_consistency` | Unique candidates agree with declared type/size where the profile is known. |
| `content_hash_verification` | Complete raw NCA hashes match the untrusted CNMT references, when requested. |
| `fs_header_hash_verification` | FS-header bytes match the unsigned NCA header's recorded hash. |
| `cnmt_trailer_digest_observation` | Current CNMT bytes agree with its embedded trailer; a mismatch does not hide structurally valid tables. |
| `content_support` | Structural reading/support boundary, independent of discovery and hashes. |
| `runnable` / `runtime_integration` | Never a verified launch; runtime integration is `not_implemented`. |

`signature_verification` is always `not_verified`. `section_data_hash_verification` is always `not_performed`. All signatures/trailers/references are untrusted inputs. A verified raw NCA hash does not authenticate a title, prove a complete Program, validate BKTR reconstruction or establish AOC registration. AOC registration is always `not_performed`.

The JSON schema is [nsp-catalog-v1.schema.json](nsp-catalog-v1.schema.json); [nsp-catalog-example.json](nsp-catalog-example.json) is an actual generated seven-group, nine-NCA synthetic result. Filenames use byte-preserving `\u00xx` escapes: decode each resulting code point as one original byte, then apply a caller-chosen display encoding. This is not a claim that arbitrary filenames are valid UTF-8. IDs are lower-case hex strings; numeric byte counts require an unsigned-64-bit-capable consumer.

## Native reuse and bounds

`catalog::Reader`/`Source` own exact, bounded read-only sources. `Region` retains the original source and enforces its range. `ReadPfs`, `ParseCnmt`, `NcaInspector`, `Scan`, `Catalog`, `Group`, `ContentReference` and `ProgramDescriptor` are reusable C++ interfaces. The optional decoder owns key/signature policy; raw NCA bytes remain the source for whole-member hashes. No plaintext NCA copies or synthetic “decrypted” success objects are used by the production bridge.

The isolated Pine bridge is an explicit opt-in API, compiled only with `-DPINE_ENABLE_NSP_CATALOG=ON` (default OFF); it is not wired to the current UI/FD launch path. Prefer `ScanNative(OpenFile(path), existingProvidedKeyStore, options)` so the same owning source carries raw ciphertext and the opened-file identity checks. `MakeBackingSource` and the `shared_ptr<vfs::Backing>` overload retain ownership and actual read counts but report snapshot identity unknown. The opt-in preflight checks protect the legacy NCA path before its PFS parser runs; normal NCA callers supply no hooks.

The default host bridge checks use controlled non-cryptographic ciphers to test read failures and control flow. The separate `--real-aes` check compiles production `crypto/aes_cipher.cpp`, NCA, CTR and PFS code with clean pinned mbedTLS 2.16.11 (`d895668359eb8ba7536a04f5e2be1e5268c86b33`). Independent OpenSSL EVP code encrypts synthetic XTS headers, ECB key-area and CTR PFS bytes using public fixture keys. Android facilities and the caller-provided KeyStore remain host substitutes. This verifies actual AES on synthetic input; it does not establish ordinary encrypted game-package compatibility, RSA authentication, Android execution or game compatibility. No owner's encrypted package has been tested in this catalog delivery.

Default limits are 65,536 PFS entries, 8 MiB string tables, 1,024 bytes per filename, 8 MiB aggregate names per PFS, 4,096 CNMT files, 16 MiB per CNMT, 65,536 content references and 65,536 metadata references across the scan, 128 MiB cumulative metadata and 64 GiB explicit whole-NCA hashing. A conservative one-million-comparison association bound also prevents duplicate candidates from expanding into unbounded output. Budget failures retain discovered items and set partial states; no fallback chooses content. Hash budget preflight covers all candidate NCAs and hashes none when the complete set is over budget. CLI budget options and native `Options::limits` allow explicit caller policy.

The dedicated host workflow builds no APK and has read-only repository permissions. It runs the real synthetic NSP tests, mandatory JSON Schema validation using the official [jsonschema 4.25.1 package](https://pypi.org/project/jsonschema/4.25.1/), and isolated bridge tests. Artifacts contain command/source commit/dirty-source evidence, generated NSP and catalog, schema, format provenance and test logs. Generated artifacts live outside the checkout so they do not masquerade as source changes.

Fixed primary-source provenance and the independently authored literal golden fixture are documented in [nsp-catalog-format-sources.md](nsp-catalog-format-sources.md). The fixture checks both nonempty content and metadata tables; it does not share the production parser/writer and specifically catches the old overlapping-table offset error.
