# AbortImpl entry observer foundation (experimental)

This branch starts from PR #22 `eec35784beacb36c508e2089b799cc50e94214c5`.
It preserves PR #23's offline decoder work. Nothing here installs a game hook or
builds a candidate APK. The executable fixture contains only our own functions.

The newly supplied four evidence files were actually read in full. Their embedded
bytes, object hashes, ELF symbol/RELA records, table arithmetic and three whole-JSON
hashes were checked. Full original NSO/segment hashes remain producer provenance:
we were not supplied their complete segment bytes. Library text reads succeeded;
consumer-local file materialization failed technically after one bounded retry per
file (`library file transfer failed: download failed`). No denied operation was
bypassed and no source-file copy is claimed.

## What the new evidence changes

The instruction at SDK `0x2D64E0` branches to **PLT `0x3E97E0`**, whose final `BR x17`
preserves the caller LR. The PLT loads GOT `0x987210`; its live target is unknown.
The outer four-argument `_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i` is defined at
SDK `0x445D0`, size 32, dynsym index 16826. Its later variadic overload is distinct.
Our earlier categorical claim that this internal BL necessarily bypasses a dynsym
hook was incomplete: the guest dynamic linker may consume a rewritten symbol value
when binding that GOT slot. The repository does not implement the game's rtld and
cannot establish its actual lookup/preemption policy.

| Route | Source-specific result | Current scope |
| --- | --- | --- |
| A: existing symbol hook | `nce.cpp` rewrites `st_value`. A PLT route might consume it, but actual guest GOT binding is unknown. `EntryExitHook` changes callee LR and `hostSp`, depending on an exit which a non-returning Abort never reaches. Generic `HookHandler` also dispatches/waits on scheduling. | Not reused. A dedicated entry-only path still needs binding and execution proof. |
| B: authenticated true entry detour | The supplied outer first word is non-PC-relative `SUB SP,SP,#32`. A synthetic B detour can preserve incoming LR, observe before the prologue, restore scratch state, replay that word exactly once, then B to entry+4. | Own controlled AArch64 fixture only. No game installation or arbitrary instruction relocator. |
| C: existing Break/exception | Records the later SVC boundary, its reason and Guest stack. | Cannot reconstruct original outer-entry X0/LR or supply a Host SIGSEGV stack. |

## Reproducible fixture

`python3 tools/run-abort-entry-prototype.py --host-only --output /tmp/entry-host`
runs portable identity/hash/branch/staging/rollback controls. With official Ubuntu
`qemu-user` and `g++-aarch64-linux-gnu` available, use `--require-aarch64` instead.
The dedicated workflow executes that mode and retains source hashes, actual
execution logs and unchanged Diagnostics writer files; missing tools fail the job.
QEMU Linux-user execution is neither an Android app run nor game observation.

The entry trampoline has no C++/JNI call, allocation, I/O, lock, scheduler or exit
frame. It makes one exclusive-store attempt into an already allocated slot and
uses only preserved scratch registers plus a 32-byte controlled valid stack area.
It tests the selected incoming LR before publication. The later normal bridge
thread formats an explicitly synthetic event for the real asynchronous Diagnostics
writer/resource window. It never emits the production NVN event or a game branch
classification.

The fixture compares all X0-X30, Q0-Q31, SP, NZCV, FPCR/FPSR and TLS at the same
original-body point; it exercises normal return, repeated non-returning longjmp,
preloaded disabled/full/busy slot states, competing threads, three simultaneously
live distinct mapped bases and exact rollback. The slot-state preloads are not
actual nested Guest execution; the 800 contending returns check completion and a
single winner's payload/TLS, not full machine state for each return.
These valid-stack, owned-mapping preconditions are **not** proof that an arbitrary
faulting Guest stack is safe for a production trampoline. A `modeledHostSp` sentinel
is a fixture invariant, not execution of Pine's Kill/longjmp implementation.

## Required before any game integration

* Owned immutable unpadded NSO metadata captured before NCE patching and guest rtld
  relocation, independent of the existing non-owning symbol spans.
* Actual Session/Launch/Process/GuestThread and mapping identity; full Build ID,
  exact verified text/rodata windows, and string provenance.
* A lifecycle/range/alignment protected, nonblocking GOT sample with an explicit
  resolver concurrency protocol. `GetChunk` releases its lock before a later read;
  `GetHostSpan` merely translates. Mapping locks do not serialize guest GOT writes.
  A current slot sample cannot prove a past traversal or permanent binding.
* Executed ARM64 state/control/non-return preservation and rollback, followed by
  real structured export. These do not authenticate or enable a real SDK hook.

No forced Owner crash, SDK scanning, SIGTRAP recovery, OverrideHook substitution,
NVN capacity change, loader selection/relocation change, or scheduler dispatch is
part of this experiment. P2 callback pairing remains out of scope.

## Implemented owned input identity boundary

`diagnostics/nso_image.{h,cpp}` now retains at most 16 modules, eight windows per
module and 256 bytes per window. `NsoLoader::LoadNso` checks a default-false atomic;
only an explicitly configured native experiment copies the raw 32-byte Build ID,
hashes the three **unpadded decompressed input** segments (256 MiB aggregate work
cap), copies requested exact windows and records the placement returned by the
unchanged `LoadExecutable`. There is no UI/JNI switch to enable this experiment.
No commercial identity or SDK byte window is bundled with the feature.

The metadata owns all bytes it retains, independent of `Executable` lifetime. It
records the header-declared hashes separately from computed input hashes, and does
not claim that the old ignored LZ4 return or Backing short reads were validated.
`VerifyPreimage` compares an explicit expected full Build ID, three input hashes
and bounded window hashes/locations. `ExpectedPreimage` does not carry expected
segment VA/size: `MatchOnly` is a preimage hash/window match, not proof of exact
segment layout or live identity. It never enables a patch or authenticates the
subsequent NCE/rtld-mutated mapping.
`liveBytesVerified` stays false. Runtime addresses are the Loader-returned Guest
placement, not a dereferenced/live range verification.

Ownership carries actual sink Session/Process IDs and Launch ID plus sink/launch
generations, acquired with two try-locks on an ordinary loader thread. Captures
that cross a logging or launch/configuration boundary are discarded. The registry
holds only a weak KProcess ownership identity; it does not keep a process/mapping
alive. Duplicate module names are ambiguous instead of silently selecting one.
Full/busy/allocation/stale/bounds cases drop metadata, leaving original loading and
exception propagation alone. The ordinary-off path performs no hashing, copying,
metadata allocation, queueing or file I/O.

`LiveGotCapability()` deliberately returns `ResolverProtocolUnavailable`. It
neither reads the original file's placeholder as a live value nor dereferences
`GetChunk`/`GetHostSpan`. A future lease must combine an exact mapped range and
alignment, a pinned owning process/launch/thread generation, nonblocking lifetime
exclusion, and a resolver publication protocol that also serializes Guest writes.
A mapping mutex by itself does not supply that protocol. Even a sound current
snapshot would be labeled a current sample, not historical call-chain proof.

## Actual execution and export scopes

The first successful executed fixture was commit
`0b9340a5a272ba697b4086f50aa3efb4f4ed25cd`, CI
[39527527694](https://github.com/xmy953538104/pine_CN/actions/runs/39527527694):
1007 cumulative assertions, including 101 portable assertions, under controlled
QEMU Linux-user AArch64 execution on an x86_64 runner, not ARM64 hardware. Earlier runs failed a
strict compiler warning and then a fixture comparison between different call-stack
positions. Matching ordinary-return and non-return baseline positions fixed the
comparison; no register/SP comparison was removed. Later revisions add a genuinely
different caller, elapsed-time ranges and owned metadata tests. The old
`c9c8d1ea` result was 1021 cumulative, including 101 portable, not 1021
ARM64-specific checks. Its "three bases" test used sequential mmap/unmap without
asserting uniqueness; that earlier result does not establish three distinct bases.

The review repair keeps three allocations simultaneously live without MAP_FIXED,
asserts pairwise address uniqueness, runs all 3x40 observed longjmp cases at those
distinct bases and retains their actual addresses in `relocation-bases.json`.
`Apply` rechecks the current enabled evidence even after `Prepare` succeeded;
disabled-between-prepare/apply tests cover both supported site kinds and verify
that text and plan publication hash stay unchanged. Disabling still permits exact
rollback. This quiescent synthetic staging API is not a concurrent runtime
revocation protocol. The runner reports portable and post-portable AArch64 counts
separately. The 85 metadata assertions run separately on host and controlled QEMU;
the extracted actual LoadNso body uses controlled host dependencies.

`tools/abort-entry-elf-control.py` builds our own GNU shared library and changes
only one `Elf64_Sym.st_value`. Executed external and internal PLT calls then return
the proxy-control value; the linked hidden direct-call alias still reaches the
original. This is a binding experiment, **not an OverrideHook implementation**, and
says nothing about the actual Horizon rtld's policy.

The dedicated CI feeds the executed fixture's actual Diagnostics JSONL and
resource-window files into `AbortEntryExportTest`, using the production Kotlin
`DiagnosticExport.capture/write` implementation. The existing exporter processes
the resource window first and deduplicates the same session/kind/sequence across
files. The test therefore requires exactly one unchanged entry across the archive,
retained in the resource window; it does not change the exporter to retain duplicate
events. Regular fast JVM CI uses an explicitly offline export-contract input and
labels it as such. Only the dedicated CI can claim the executed-input ZIP test.

The retained performance samples are min/median/max nanoseconds per invocation over
five 10,000-call batches for unmodified, detour-disabled and detour-full paths. They
measure a QEMU/Linux fixture, including its ABI harness, not an Android device,
Pine's rendering threads, game FPS or worst-case hardware latency.

## Remaining gates and minimum next feature

| Gate | What this branch can demonstrate | Still required |
| --- | --- | --- |
| A | Owned Loader-input identity and explicit safe-read unavailability; GNU and controlled PLT routing experiments. | An actual loaded SDK image, post-NCE live-window verification, protected current GOT sample and real rtld binding/traversal evidence. |
| B | Executed owned ARM64 entry X0/original LR, all compared state, ordinary return, repeated non-return and full rollback. | A Pine-specific entry-only ABI using real ThreadContext/GuestThread generations, stack/lifetime safety, and executed real Pine Kill/longjmp/cleanup preservation. |
| C | Actual Diagnostics writer/resource-window and production Kotlin ZIP test over clearly synthetic data. | Real Guest entry events with authenticated module and thread provenance; no such event is produced here. |

The minimum underlying implementation is a dedicated **entry-only** NCE observation
primitive: per-thread preallocated slot with a non-reused lifetime generation;
authenticated module/entry plan prepared while loading is quiescent; complete saved
entry X0/X30/SP and affected state; a bounded noexcept, scheduling-neutral producer;
and release/restoration **before** tail transfer to the original prologue/body.
It must not inherit `HookHandler`, hold a hostSp/guard frame until function return,
or pass through an exit hook. A symbol route additionally requires proof that the
real resolver binds the selected GOT to this entry-only stub. An entry detour instead
requires a verified displaced instruction and branch reachability, with exact
conflict-aware rollback. Neither is wired into games in this PR.

The current fixture uses 32 bytes below a valid Linux stack pointer. It does not
prove safety for every possible Guest stack/guard boundary. Production needs either
a proven compatible stack-range/lifetime contract or a separately tested bounded
scratch/context mechanism that preserves all incoming state without assuming that
extra Guest stack space. This is an explicit prerequisite, not an implicit ABI
permission. The fixture's Linux TLS value is not a Pine GuestThread identifier.

Stop codes for game integration remain `ACTUAL_SDK_IMAGE_NOT_OBSERVED`,
`LIVE_GOT_RESOLVER_PROTOCOL_UNAVAILABLE`, `PINE_ENTRY_CAPTURE_NOT_CONNECTED` and
`GUEST_STACK_LIFETIME_NOT_PROVEN`. These are precise missing capabilities; passing
synthetic tests does not clear them. No experimental APK is produced. Rollback is
to keep this branch unmerged/default-off or revert the follow-up commits; PR #22's
APK/symbols and PR #23's decoder remain unchanged.
