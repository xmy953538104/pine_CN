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
disabled/full/busy states, competing threads, three mapped bases and exact rollback.
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
