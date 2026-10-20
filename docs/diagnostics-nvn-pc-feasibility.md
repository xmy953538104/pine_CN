# Precise NVN abort observation: P0 STOP, offline contract only

Baseline: PR22 `eec35784beacb36c508e2089b799cc50e94214c5`.
This branch adds host tools/tests/docs only. No runtime observer is connected,
no guest code is patched, and the existing APK/symbols/rollback are unchanged.
The Python tool cannot admit or publish a runtime capture, even with complete
supplied identities and registers. P2 two-point observation is not implemented.

## Evidence provenance and explicit gaps

Library `libfile_a495333124488191812cd439045a759f` resolved to a 13301-byte ZIP.
Its standard materialization helper returned exit1, `download failed`: initially,
once on the same transfer, then once after a fresh preparation pinned to the same
file identity. Preparation succeeded; no explicit permission denial or safety
rejection was returned. The underlying download cause is unknown. No alternate
download route was used. This worker did not obtain or inspect the ZIP bytes.

The parent task independently read all four text/CSV/JSON files and supplied its
review. Its reported ZIP SHA256 is
`73d5f48e4f9fab93cb604fec0ae58a78a8d512500e80bef4fecda1390001bd8a`;
this is attribution, not this worker's local hash verification. The original
`handoff-interface.json` schema was not available here. The offline input below
is a separate, documented format derived from the owner's addresses and that
review summary, not an implementation claiming exact original-schema compliance.

The supplied `sdk.nso` NSO VA `0x2D64E0`, bytes `c04c0494`, decodes to a BL to
`0x3E97E0`. This encoding/arithmetic was checked with Pine's actual instruction
header. It does not verify any running module. Text VA0 is a producer statement,
not a discovered runtime base. No Program Content ID substitutes for NSO Build ID.

The parent found no complete helper/neighbor window bytes/hash or exact
machine-readable boundaries. A possible `[0x2D6464,0x2D64E4)` range implied by
prose is **not** an accepted signature specification. Referenced binary-identity,
instruction-evidence, assembly and validation files are absent from the package.
There is no independently verified binary/control-flow or runtime mapping here.

## Baseline capability table

Paths/lines below refer to the fixed PR22 baseline, not a modified runtime.

| Surface | What exists | Why it cannot supply the requested snapshot now |
| --- | --- | --- |
| `nce.cpp:358` GetPatchData | Scans SVC and selected MRS/MSR operations | The exact supplied BL is not selected; no arbitrary-PC observer |
| `nce/guest.h:20`, `guest.S:6` | GpRegisters and SaveCtx store X0-X18 | No X19/X20 fields or PC/LR/guest-SP snapshot; hostSp is a host transition value |
| `kernel/types/KThread.h:159` | KNceThread.ctx describes the last SVC | It is not continuously current at an internal SDK instruction |
| `hle/symbol_hook_table.h:16` | Zero symbol hooks | Hook support matches global function symbols, not an arbitrary internal instruction |
| `nce.cpp:539` WriteHookSection | Changes symbol st_value, proxy/entry/exit trampolines | Not a read-only pre-BL observation API; HookHandler also services pending scheduling |
| `nce.cpp:167`, `common/signal.cpp:165` | ucontext at a delivered signal | Existing guest SIGTRAP is a crash/exit path, including Kill/longjmp, not a resumable breakpoint |
| `nce.cpp:205` host fault/debugger handling | Host SIGSEGV/debugger path | Does not arrange a stop at the requested SDK PC; no hardware-PC observer implementation |
| `loader/nso.h:56`, `loader/nso.cpp:33` | Reads a 32-byte NSO header Build ID | LoadNso does not retain/pass it to executable diagnostic identity |
| `loader/loader.cpp:14,65,95` | Prefix patch/hook sections then actual text | Mapping base can differ from text base; return base alone is insufficient |
| `loader/loader.h:67` | Symbolic executable ranges/name | No retained Build ID, exact text length/window attestation or public observation descriptor |
| Diagnostics/resource window | Existing async bounded writer and identity metadata | No preallocated register-capture adapter or per-launch successful-publication latch |

Reading X19/X20 with C++ inline assembly after a dispatcher call would not prove
their pre-BL values. ABI callee-save rules preserve values across a completed
call, not their location inside an arbitrary C++ frame. See the primary
[AAPCS64 register rules](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst#general-purpose-registers).
Likewise a later SVC Break/host SIGSEGV context cannot reconstruct these earlier
caller registers. No signal handler, instruction or scheduling behavior changes
are justified by this audit.

## Required runtime admission before any future implementation

Every missing or mismatched condition is STOP; there is no name-only, CID-only,
nearest-PC, zero-filled-register or expanded-scan fallback.

1. Obtain complete machine-readable identity/evidence: exact 32 raw NSO Build ID
   bytes, module/SDK version, executable text segment VA/size, exact bounded
   neighbor/helper ranges, bytes/hash algorithm and validation stage. Define
   pre/post NCE patching, relocation and mutable-data semantics explicitly.
2. Bind those values to the actual effective loaded sdk module and current
   Session/Launch/Process/thread generation. Validate integer arithmetic, actual
   RX text bounds and instruction/window before enabling a trap or detour. In the
   current Loader, text is `load base + patch.size + hookSize + text.memoryOffset`;
   an ExeFS filename or earlier source header alone is not runtime attestation.
3. Establish an ARM64 observation entry that captures original PC/LR/SP and
   X0/X8/X9/X10/X19/X20 before scratch-register, stack or C++ use. Prove preservation
   of all affected registers, flags, SIMD/system state, TLS/stack and original BL
   target/link semantics, including unexpected AbortImpl return. No such proof or
   entry exists in this branch. Official assembly/disassembly is not execution.
4. Prove bounded, preallocated storage and a race-safe successful-publication
   limit of one valid snapshot per Launch. Define nullable identity policy,
   producer lifetime, stale-generation rejection, reentrancy, interrupted/failed
   publication and writer ownership. Do not consume a successful-record allowance
   merely by entering a failing capture. None of this is asserted implemented.
5. Only then connect normal-thread serialization to existing diagnostics/window
   limits. No现场 JSON/allocation/I/O/unsafe lock, Lua or NVN calls. Continue the
   original Abort/Kill/longjmp/cleanup without adding scheduling policy. No claim
   of crash-atomic persistence follows from an admitted record.

Stop codes in the offline tool are unconditional:
`STOP_RUNTIME_NOT_CONNECTED`, `STOP_MODULE_IDENTITY_UNVERIFIED`,
`STOP_WINDOW_SPEC_AND_EVIDENCE_MISSING`, `STOP_NO_SAFE_EXACT_PC_OBSERVER`.
Malformed offline input adds `STOP_INPUT_INVALID`. Environment lacks ARM64
execution/KVM/DRI/ADB targets, so preservation/runtime overhead is also untested.

## Offline format, classification and limits

`python3 tools/nvn_abort_contract.py` consumes one UTF-8 JSON object on stdin,
maximum16384 bytes. Unknown/duplicate fields and override arguments are rejected.
Unsigned64 values must be exact lowercase `0x` plus16 hexadecimal digits, never
JSON numbers/floats. Unknown or missing registers/associations remain null with
explicit unavailable lists, never fabricated zero or IDs. The output is
`offline_nvn_abort_contract_decode`, with `runtime_connected=false`, admission
STOP and classification scope `offline_contract_candidate_only_not_verified_control_flow`.
It is not an Android diagnostics event/resource window/export bundle.

Input keys: `module` has `name`, optional `build_id` (64 lowercase hex digits),
`image_base`, `text_base`, `text_size`; `registers` has `pc`, `lr`, `sp`, `x0`,
`x8`, `x9`, `x10`, `x19`, `x20`; `instruction_le` is an optional8-hex claim;
`context` may carry `session_id`, `launch_id`, `process_run_id`, `guest_thread_id`.
Every supplied identity/instruction is a claim, never a runtime verification.
The offline arithmetic requires text_base=image_base for the supplied text-VA0
contract, the full instruction inside the supplied text range, exact supplied PC
and instruction claim. Missing/overflow/mismatched pointers are UNCLASSIFIED.

| Contract candidate | Conditions and permitted algebra |
| --- | --- |
| NO_CALLBACK | X0=image_base+0x7A0371 and X8=0; raw X9/X10 may be retained, but no interpretation/derivation |
| INSUFFICIENT_AFTER_RETURN | X0=image_base+0x7A032A, upper32(X10)=0, signed64(X8)<signed64(X19), required derivation registers present |
| UNCLASSIFIED | Missing or inconsistent supplied conditions, with a bounded reason |

For the insufficient candidate only, p18=(X9-X8) mod2^64,
p20=(X9-4*X10) mod2^64 and r70=u32(X10) are labeled algebraic candidates from
one supplied object. No separate object/time is read. X20/LR/SP remain raw values,
not inputs to invented predicates. A label does not prove a callback ran/returned,
its target/state, zero growth, backend allocation failure, VRAM/RAM exhaustion,
or any relationship to Host SIGSEGV. Complete supplied identities still STOP.

## Reproduction and scope of measurements

```sh
python3 tools/tests/test-nvn-abort-contract.py
python3 tools/tests/test-nvn-pc-capability.py
python3 tools/tests/test-nvn-pc-capability.py --sanitize
```

The decoder suite covers signed extrema, upper32, wraparound, 10000 deterministic
comparisons/algebra invariants, unavailable fields, no-callback exclusions,
pointer/PC/text mismatch, full64-bit JSON and real ZIP roundtrip, bounds and CLI
errors. The host C++ probe executes verbatim GetPatchData with actual instruction/
context headers and controlled clock/utility dependencies:20 cases, no target BL
admission. ASan/UBSan explicitly disables leak detection; not complete LSan.
Existing fast CI discovers these `test-*.py` files without workflow changes.

Local official NDK27.3 compiles original guest.S to an ARM64 ELF object and
llvm-objdump verifies instruction output, including the X0-X18 stores. It is not
an ARM64 execution, preservation or Abort test. Only offline decoder/parser host
timing is measured; no Android/game/FPS, callback/pairing or capture overhead is
claimed. App/CMake/Gradle/runtime paths have no diff, so no observer work is added
to guest/render producers. Numeric game performance remains unmeasured.

P2 remains STOP_PAIRING_UNPROVEN: points0x2D6498/0x2D64AC are not hooked. Any
future two-point design needs exact same-thread/same-helper invocation identity,
reentrancy and nonreturn/missing-state handling; a thread ID alone is insufficient.

Rollback requires no APK change: keep PR22 at its frozen SHA. Its [APK](https://github.com/xmy953538104/pine_CN/actions/runs/39457824170/artifacts/12280779022),
[symbols](https://github.com/xmy953538104/pine_CN/actions/runs/39457824170/artifacts/12281926158)
and PR21 rollback remain untouched. This offline branch can simply be omitted.
No Owner crash reproduction or new NSP is requested.
