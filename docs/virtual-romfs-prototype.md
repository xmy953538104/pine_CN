# Optional read-only Virtual RomFS prototype

This independent P2 prototype starts from the owner-tested `79a4d1a2250585ec64233befab934f0e5ed27ab7` baseline. It has no NSP Catalog dependency. It rebuilds a virtual RomFS image for an explicitly selected, already-final official RomFS backing: replacement/addition file data stay in their owned sources, while only new header/index/metadata bytes are copied. The final effective official input remains the source for every preserved file.

The API is compiled only with `PINE_ENABLE_VIRTUAL_ROMFS_PROTOTYPE=ON`, default **OFF**. No loader, PatchManager, fssrv, AOC, ExeFS, SaveData, Diagnostics or UI hook calls it. It does not select or reconstruct Base/Update content. It does not claim GoldenMod, Framework, arbitrary mod, game or Android compatibility.

## Explicit identity and lifetime boundary

Call `CreateVirtualRomfs(effectiveRomfs, manifest, options)` after the existing official resolution path succeeds. Disabled options return the identical original shared pointer without source reads, guard calls, hashes or allocation of a backing object. The test also asserts zero heap allocations in this fast gate.

Enabling requires all of:

- Nonzero explicit active Application/Program IDs matching the manifest target; neither is derived from a filename, NACP, save owner, mask or display name.
- Complete, locally computed SHA256 of the supplied raw effective Program and final official RomFS matching the manifest's expected hashes. Hashing is streamed within an explicit total budget; it does not copy the original data.
- A separately supplied trusted `IdentityVerifier`. It must independently establish that these actual computed bytes belong to the explicit Application/Program. The default is absent and rejects activation. A manifest assertion or the computed hashes alone cannot authenticate this relationship. The test verifier is an explicit synthetic policy, not Nintendo signature verification.
- A mandatory `SourceGuard`, constructed with and checked against these exact owning Program/RomFS source pointers. Its trusted owner must provide a fixed-generation/snapshot contract, check before/after activation and every active read, and throw on a changed or unknown generation. Its hot-path check must use existing generation evidence without directory scans, hashing or stat calls. No production runtime owner/guard provider is installed here.
- Owned `PayloadSource` snapshots with exact sizes, expected SHA256 and change guards. A source with an unknown snapshot contract is rejected. `OwnPayload` moves an already supplied vector into private const ownership; ownership transfer forbids retaining mutable aliases, and the image builder does not duplicate payload bytes. External sources must provide a stable version or fail, thread-safe reads, in-memory sizes and actual read counts. Guards run around verification and each payload segment read.

`Result` separates the actual computed Program/RomFS/manifest hashes from `externalIdentityAttested`. `runtimeIdentityIntegrationImplemented` remains false. Those fields do not manufacture trust from strings or prove platform authenticity. The caller must inspect `status`; a historical attestation in a rejected result does not activate a virtual view.

Read-only mode and an initial SHA256 do not prove an immutable launch version. The mandatory same-source and payload guards close that boundary for admissible providers. Such providers are part of the trusted embedding contract, not untrusted manifest data. Without an independently established guard, the prototype rejects activation. The tests mutate original Program/RomFS and payload bytes without changing their length, including mutations during reads, and require explicit errors.

The baseline [`BKTR` count contract](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/vfs/bktr.h#L40-L44) is unreliable. This prototype therefore rejects that original backing and returns its identical original view. It does not wrap an unknown count as successful. A reliable controlled already-resolved wrapper tests the boundary; it is not a real CTR/BKTR validation. Existing CTR/BKTR code, keys, checks and selection are unchanged.

## Format, validation and virtual reads

The baseline [RomFS structures](https://github.com/xmy953538104/pine_CN/blob/79a4d1a2250585ec64233befab934f0e5ed27ab7/app/src/main/cpp/skyline/vfs/rom_filesystem.h#L43-L76) define the `0x50` header, `0x18` directory entry and `0x20` file entry. The prototype checks all four tables, alignment, exact entry boundaries, bounded names, parent/tree reachability, siblings, cycles, orphan entries, duplicate/case-colliding paths, file data and metadata overlap, checked 64-bit arithmetic and budgets before exposing new metadata. Input files may precede or follow the metadata tables.

Names use an explicit printable **ASCII-only** profile. Non-ASCII names are unsupported and cause original-view fallback. Relative paths reject empty/dot/up-level components, absolute paths, backslashes, colons, control bytes, trailing dot/space aliases, duplicate operations and file/directory parent conflicts. This is not full Unicode normalization support. There is no host directory, symlink, asset scan, path opening or hot reload in this API: payload sources must already be supplied explicitly.

Primary producer provenance is [switch-tools `22756068dd0ed6ff9734c59cb4f99ebd3f62555b`, `romfs.c`](https://github.com/switchbrew/switch-tools/blob/22756068dd0ed6ff9734c59cb4f99ebd3f62555b/src/romfs.c#L85-L115). Its path hash seeds with parent metadata offset XOR 123456789, then rotates right by five and XORs each name byte. Directory `hash` and file `hash` hold the **next metadata offset** in a collision chain; they are not stored hash values. Buckets and links use `0xffffffff` as empty, while offset zero is a real entry. Name storage aligns to four bytes and file offsets to sixteen. The output uses the producer's bucket-count policy and a `0x200`-aligned data start; table/data offsets are explicit.

`tests/virtual-romfs/fixtures/root-nested.romfs` is a 680-byte literal oracle with `c/a=OK` and `c/d=!`, independently packed without importing either production code or the general fixture writer. SHA256 is `c7d6619a3d10e89e49b292f1d4c9d10cc4479dc95d094bcad4058ccba8176501`. Both directory and file collision chains contain a next-offset of zero. Fixed rotate/parent hash vectors and exact bytes are asserted. An independent Python reader verifies every rebuilt output bucket/link and file byte; successful legacy filesystem traversal alone is not used as index proof.

The builder creates deterministic sorted metadata and a contiguous immutable segment index. Segments reference metadata, zero padding, preserved original ranges or replacement sources. Binary lookup splits arbitrary reads across those segments without allocation, hashes, stat, scans or fallback. `Backing::size` and actual existing `IStorage::GetSize` expose the complete new image size. The virtual backing's `ReadImpl`/`ReadUnchecked` return the actual complete count or throw, including EOF/range and source short reads. The inherited `Backing::Read` convenience wrapper is unchanged and returns the backing's total size, not this operation's byte count; callers must use `ReadUnchecked` or a reliable `ReadObservation` for actual-count evidence. A failure after activation never fills from an old file or returns a partially successful read; the caller's buffer may have been partly written before the exception, as with ordinary I/O.

Input I/O/decryption/resolution exceptions propagate. Opt-in validation rejects return the complete original pointer and an explicit reason; this does not certify that an invalid original image can run. No overlay masks a preexisting NCA failure. Replacements and additions cannot delete files, merge multiple overlays, override DLC/ExeFS/SaveData or change the official composition order.

Default limits: 32,768 combined metadata entries, 4,096 manifest entries, 255-byte names, 4,096-byte paths, depth 32, 8 MiB aggregate paths, 16 MiB metadata, 32 MiB explicit payloads, 512 MiB total activation hashing and 16 TiB virtual size. Source/table containment is checked before allocating. Limits are explicit caller policy; exceeding them returns a rejected original view.

## Host validation and deliverables

```sh
python3 tools/tests/test-virtual-romfs.py --evidence-dir /tmp/virtual-romfs-normal
python3 tools/tests/test-virtual-romfs.py --sanitize --evidence-dir /tmp/virtual-romfs-sanitized
```

Only the installed C++20 compiler, Python standard library and host POSIX support are needed. The suite compiles the **unchanged production** `RomFileSystem.cpp`, `IStorage.cpp` and `Backing` with platform/IPC declarations stubbed. It tests same/smaller/larger/empty replacements, nested additions, preserved/empty files, every offset with multiple lengths, cross-segment reads, EOF, `GetSize`, no-allocation reads, concurrent reads, invalid identity/hash/guards, all-or-original rejection, same-size mutation, read failures, unsafe paths, cycles, bad indexes, malformed lengths/offsets, overflow and budgets. It compares manifest-order permutations and independently checks hashes with Python `hashlib`.

The existing baseline directory listing implementation unconditionally reads absent file/child heads in requested list modes. This prototype does not change that default implementation. Tests use its valid nonempty list modes, and check empty directory existence separately; general empty-directory enumeration is not claimed as fixed.

The dedicated read-only host workflow runs normal and ASan/UBSan suites without Android/APK, keys, commercial content, secret access or publishing. It retains synthetic original/virtual images, raw result, source commit and dirty-source evidence, compile commands, logs, literal oracle and this provenance document. `source_dirty` is expected to be false for a committed clean CI checkout; local development evidence reports actual changes. No owner game, on-device storage, real cryptography, runtime title identity/guard integration or performance claim follows from these synthetic tests.
