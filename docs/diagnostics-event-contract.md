# Pine Diagnostics event contract (schema 1)

Diagnostics is opt-in. Events describe observations at the named call site, not a
claim that a title or DLC is playable. JSONL records and the `PineDiag` logcat tag
share the session. The native sink owns copied field values, bounds text, escapes
JSON, and asynchronously writes records. A bounded queue may drop records; inspect
drop counters before assuming an absent event means a path was never executed.
Sequence numbers describe accepted producer order; monotonic time is appropriate
for elapsed time and wall time for correlation. Thread metadata names the host
emitting thread; `guest_thread` is separate and exists only when supplied by guest.

## Record shape

```json
{"schema":1,"session_id":"example","sequence":3,"wall_clock_ms":1792166400000,"monotonic_us":123456,"dropped_count":0,"thread":"HOS-1","category":"guest","event":"GUEST_DEBUG_MESSAGE","level":"INFO","last_stage":"content_ready","fields":{"source":"OutputDebugString","message":"hello"}}
```

`wall_clock_ms` is Unix epoch milliseconds, not an ISO string. `monotonic_us`
uses the steady clock; compare deltas within a process, not across boots.
Event-specific values live in `fields` and are strings. Empty or absent values
mean unknown/not supplied. Consumers must tolerate additional fields and events.
`last_stage` is the most recently recorded successful stage, not a complete stack.

Root `title_id` remains the application filter for schema-1 consumers;
`application_title_id` is its explicit equivalent. `program_id` is the selected
base Program NCA header ID, `cnmt_title_id` its selected CNMT ID (which may be a
Patch), and `program_nca_id` its 32-hex archive entry stem when available.
An Application CNMT or a Patch CNMT's typed parent establishes the application
relation; otherwise the UI association or known Program ID is used. Save ownership
and bit masking do not establish that relation. Per-event `fields.title_id` still
identifies the content being observed, including DLC. Base and Update selection
fields retain their own Program IDs. Unknown NCA IDs stay empty: an entry stem is
not a verified full-content hash and matching Title/version does not prove equal bytes.

`launch_id` groups one emulation attempt across diagnostic session/level changes;
a subsequent launch gets a new ID. `process_run_id` identifies the emitting process
incarnation. Both are anonymous and absent before observed initialization. Enabling
diagnostics during an existing launch emits `LAUNCH_CONTEXT_OBSERVED` with
`start_observed=false`, without fabricating a launch-begin event. Old fields remain
readable. The ADB collector prefers `application_title_id`, falls back to legacy
root/field Title IDs, and optionally filters `-LaunchId`; it never guesses an old
Patch-to-application relation by clearing bits.

## Content events and their meaning

| Event | Evidence required |
|---|---|
| `PATCH_SELECTED` | Actual entry-point selection; first-enabled and URI fallback may select differently. This does not change selection policy. |
| `PATCH_ROMFS_OPENED` | Actual patch RomFS opening path. A call to PatchRomFS alone does not prove BKTR use; correlate the NCA CTR/BKTR branch event. |
| `DLC_REGISTERED` | DLC loader registration, not storage open or successful reads. |
| `DLC_STORAGE_OPENED` | A requested Data ID matched an opened backing. |
| `DLC_DATA_READ` | A completed read observation. `actual_bytes` is an exact byte count only when `byte_count_evidence` is `reliable_backing_result`; otherwise both the byte count and status are explicitly `unknown`. Backing::Read's total-size return is never successful-byte evidence. |
| `DLC_RUNTIME_ERROR` | An observed DLC read/open error, retaining original propagation. |

A candidate NCA, selected NCA, overwritten candidate and rejected NCA are distinct
observations. Base ROM type use for update/DLC and the original updateFd > 0 test
remain unchanged in v1. Version and Title ID are reported only when available.
NSP selection events follow the completed slot assignment. The last eligible NCA
of each type still wins; `replaced_title_id` describes the previous occupant.
Archive entry names are reduced to basenames for diagnostics while the loader
continues to open the original entry name.

Events inside a synchronous loader call carry `loader_source` (`base`, `update`,
or `dlc`) and its zero-based `loader_index`. The index distinguishes several DLC
inputs without exposing file descriptors or document URIs. Those fields are empty
outside an active loader context. `LOADER_READY` follows successful construction;
`BASE_SELECTED`, `PATCH_SELECTED`, and `DLC_REGISTERED` follow the actual assignment
or registration. `cnmt_version` is the integer CNMT version, `display_version` is
the control metadata's display text, and either can be unknown. Parent IDs are
reported only for the matching Patch or AddOnContent CNMT type. NACP save ownership
is reported as `save_data_owner_id`, not assumed to be a program Title ID.

Selection, NCA and AOC events use category `content`. Storage requests, successful
service opens, and read/error observations use category `storage`, including
`DLC_DATA_READ` and `DLC_RUNTIME_ERROR`. Missing backing pointers produce failure
events rather than an `OPENED` event; this does not repair or replace that backing.
`ROMFS_BACKING_OPENED.path` identifies the actual CTR, BKTR or direct branch. A
`PATCH_ROMFS_OPENED` record alone cannot establish BKTR use.

Ordinary tracing records the first enabled DLC read attempt and every 128th attempt
thereafter if it completes successfully. Detailed tracing uses the same policy
with a period of 16 attempts, limiting formatting work in both modes. Successful
read records explicitly report `sampling_period` as `128` or `16`; they are samples
and must not be interpreted as a complete read count. Known short reads and caught
standard exceptions are observed at either level
without sampling, subject to the sink's queue/drop limits and category filter.
Their `sampling_period` is `1`.
`read_ordinal` is per IStorage instance and counts enabled attempts, including
failures. `request_size` preserves the guest IPC argument; `buffer_size` is the
actual buffer read by the existing implementation. Diagnostics continues to use
that buffer unchanged even when the two sizes differ. `actual_bytes` comes from
a reliable completed backing result and is `unknown` on an exception or when
the backing cannot prove the count. A known completed short read retains the
original warning and total-size return behavior.

OsBacking and AndroidAssetBacking provide reliable counts. RegionBacking and
CtrEncryptedBacking preserve that capability only if their underlying backing
does. BKTR uses legacy nested `Backing::Read` values that can hide a short source
read, so it and wrappers around it explicitly emit `status=unknown`,
`actual_bytes=unknown`, and `byte_count_evidence=unavailable_for_backing`. Such a
record is not evidence of successful or complete bytes, even if the original
service call returns success. Unknown completed reads use the same 16/128
sampling periods. New or unfamiliar backing types default to unknown. Diagnostics
performs no extra read and does not change BKTR's existing returns or warnings.

## Guest messages and the Don'tStarve-fix bridge

`GUEST_DEBUG_MESSAGE` captures OutputDebugString or LogManager text.
`GUEST_RUNTIME_ERROR` is a LogManager Error/Critical message; this means the guest
reported an error, not necessarily that it terminated. `source` distinguishes
`OutputDebugString` and `LogManager`. A LogManager record carries `guest_file`
(basename), `guest_line`, `guest_function`, and `guest_thread` when supplied.
OutputDebugString has text only: its source location fields are empty, not inferred.
Guest messages and source values are bounded and escaped by the sink. Empty
source values mean unavailable. Native stack frames are not Lua frames.

The game-side bridge must send one of these exact leading tokens through an
existing guest OutputDebugString or LogManager call:

- `DFX_MAIN_ENTER`
- `DFX_MODINDEX_READY`
- `DFX_MODMAIN_OK`
- `DFX_HAMLET_ENTER`
- `DFX_LUA_ERROR`

A token must end the message or be followed by a space, colon, tab, CR or LF.
For example, `DFX_LUA_ERROR: scripts/example.lua:42: message` produces a marker
record in addition to the original guest record. Embedded tokens, bracketed
prefixes, unknown names and suffixes such as `DFX_LUA_ERROR_FAKE` do not match.
Marker records retain the message and true guest source fields. Parsing a marker
is not independent verification of its contents: it is evidence the guest emitted
that marker. `DFX_LUA_ERROR` has ERROR level. Pine never generates these markers
from loader success, DLC presence or inferred game state.

Lua print/error data is visible only if game/native glue routes it into these
existing guest log interfaces. This implementation cannot extract closed-source
Lua stacks, locals, or swallowed errors. A future game-side bridge can explicitly
serialize them into a bounded message; no commercial game resource is modified.

## Exceptions and crash boundaries

`SVC_EXCEPTION`, `HOOK_EXCEPTION`, `CPP_EXCEPTION` and
`NATIVE_SIGNAL_EXCEPTION` are emitted at existing safe C++ catch boundaries.
SVC/hook records include the known call name. Native signal exceptions exclude
SIGINT; normal ExitException is excluded from generic C++ diagnostic errors.
The original catch, kill and longjmp behavior remains intact. Raw signal handlers
perform no new diagnostics calls. Consequently a guest raw-signal/longjmp exit
may have no structured fatal record; previously flushed events and the original
emulation log remain the available evidence. An enqueued record is not a promise
of durability after abrupt process termination. No global exception swallowing
or crash-handler allocation is introduced.

## Validation boundaries

`tests/diagnostics/guest_test.cpp` checks marker recognition, false positives,
empty text, embedded NUL and UTF-8. It also sends messages and a safe signal
exception through the real sink, checking source fields, marker/error events and
the disabled gate. Native sink tests validate serialization and
queue/file behavior separately. These are host unit tests, not Android APK or
commercial-game execution. Owner testing must verify actual guest log transport
and DFX output with their own lawful game-side build.

`tools/tests/run-content-diagnostics.sh` invokes the production Backing, IStorage
and PatchManager code with platform/IPC or controlled NCA-construction stand-ins.
It checks actual read counts, short/zero/error behavior, unchanged ignored IPC
request-size semantics, category and tracing-level sampling, and patch returns and
exception propagation. `composite_backing_test.cpp` invokes production BKTR,
RegionBacking, CTR and OsBacking: it reproduces a short base read hidden by a full
legacy BKTR return, checks patch and partition return values, and verifies that
the service emits unknown byte evidence without changing output, warnings or I/O
counts. It also checks real OS EOF observation. Cipher APIs in that test use an
identity stand-in and do not validate AES. `nsp_content_test.cpp` exercises the exact candidate helper
called by the production NSP loop with an ordered set of controlled NCA candidates:
all four selected types, ineligible sections, later replacements, standard-error
continuation, and loader-error propagation. These tests do not parse a complete
NSP/PFS0, decrypt an NCA, validate keys/tickets or demonstrate game compatibility.

## Game identity and signal/exit evidence

`title_id` at the top level is the running base program context once actually
known. `fields.title_id` can identify a candidate, update or DLC. ADB `-TitleId`
uses known base context first and falls back to the content field before that
context exists. Import/early-loader events can have no running game context.

The current-game identity is independent of the diagnostic sink session. Actual
base selection updates it even while diagnostics is off; Stop/Start and changes
to level, categories or retention preserve it. JNI clears it at a true game launch
and only after the final native session has drained at game end. A later settings
session or unknown-title game cannot inherit the previous game's identity.

Android completion owns a process-wide game generation. Session apply and drain
serialize on background threads under that ownership; navigation/process exit
checks the generation again on the main thread. A replacement game invalidates
old completions immediately. Activity destruction closes that generation to new
completion requests before asking native emulation to stop, including callbacks
made synchronously by that stop. If a normal ending was already accepted, its
owned drain and exit survive Activity destruction or coroutine cancellation.
Destruction must not create a new ending. Late same-game apply and duplicate
endings are rejected. This preserves orderly shutdown without letting an old
game's suspended completion stop or kill its replacement.

`GUEST_SIGNAL` is emitted after the existing guest signal longjmp returns to the
ordinary KThread entrypoint. The handler only captures a `sig_atomic_t` signal
number. SIGINT does not produce a crash event. No JSON/locking/allocation/JNI is
added to the raw handler. Existing signal handling and process termination stay
in place. Normal emulation completion stops/drains the diagnostic writer before
returning; the explicit exit UI drains on a background thread before process kill.
Forced kills or power loss can still lose queued events.

Native faults before the diagnostic library/session is initialized can only be
observed through external Android crash/logcat evidence. They do not have a
diagnostic session or a completed initialization stage invented after the fault.

The main and release emulation Android processes use fixed `main/` and
`emulation/` subdirectories, each with its own session and bounded rotation. The
settings screen identifies current-process stats and lists the other scope's last
observed session separately. Changes apply to the next emulation process start.
The export includes only fixed filenames from these scopes and legacy diagnostics,
with per-value JSON redaction; incomplete/malformed records are counted/omitted.
Recognized private locations are removed through their decoded string's line end,
including filenames with spaces or quotes. Earlier context, other JSON fields
and relative guest paths survive; later same-line detail is conservatively omitted.
# Startup observation clarification

See [startup crash evidence and acceptance](diagnostics-startup-crash.md).
`STAGE_STARTED` means entered, not completed. `INITIALIZATION_UNWOUND` is
unclassified unwind information and does not classify normal cancellation as
a crash. UI selections are associations (`execution_confirmed=false`), and
`loader_source=metadata_scan` does not establish runtime execution. Historical
`PREVIOUS_PROCESS_EXIT`/`PREVIOUS_PROCESS_CRASH` records preserve Android's
reported reason; the previous build/session/title are unknown.

## Bounded admission and critical state

The default 1024-slot queue reserves 64 slots for selected lifecycle, exception,
content selection and storage-open/error events. Normal samples cannot occupy
that reserve. Critical events still drop if the queue is full, admission is
contended or allocation fails. Category selection still applies. Guest message
floods have no reserved admission merely because their text claims an error.
`drop_counts` separates `contention`, `queue_full`, `allocation`, and
`disk_failure`; `critical` is a subset of those losses, not an extra count.
Counters are cumulative per logging session and final counters require normal
sink closure. Missing terminal records leave only lower bounds.

The writer maintains `critical-state.jsonl`, at most 32 accepted last-state rows
and 64 KiB per process, replacing it at most twice per second and on clean drain.
It retains recent selections/stages/errors across ordinary log rotation, subject
to eviction and write failure. `record_kind=critical_snapshot` and
`event=CRITICAL_STATE_SNAPSHOT` distinguish state from new events;
`fields.observed_event` identifies the original event with its original sequence,
time and identity. Export preserves both state and event rows with a shared
sequence but counts state separately, without replaying errors or crashes.
Snapshot failures disable only that optional writer and increment
`snapshot_errors`; no producer or signal handler writes files. There is no
lossless persistence or filesystem durability guarantee across sudden death.
