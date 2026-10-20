# Pine Diagnostics event contract (schema 1)

The additive [DFX1 wire contract](diagnostics-dfx1-protocol.md) documents the
versioned guest parser, legacy compatibility, budgets and calibration procedure.
DFX1 does not independently establish a game-side Lua/native logging bridge.

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
An Application CNMT or a Patch/AOC CNMT's typed parent establishes the application
relation; otherwise a known Program header ID is used. Save ownership, an untyped
UI association and bit masking do not establish that relation. The root
`application_identity_source` distinguishes native CNMT/Program resolution from an
early `ui_metadata_hint_not_runtime_verified`; cached UI metadata is not evidence
of the runtime loader's choice. Per-event `fields.title_id` still
identifies the content being observed, including DLC. Base and Update selection
fields retain their own Program IDs. Unknown NCA IDs stay empty: an entry stem is
not a verified full-content hash and matching Title/version does not prove equal bytes.

`launch_id` groups one emulation attempt across diagnostic session/level changes;
a subsequent launch gets a new ID. `process_run_id` identifies the emitting process
incarnation. Both are anonymous and absent before observed initialization. Enabling
diagnostics during an existing launch emits `LAUNCH_CONTEXT_OBSERVED` with
`start_observed=false`, without fabricating a launch-begin event. Old fields remain
readable. The ADB collector prefers root `application_title_id`, then an explicit event
`fields.application_title_id` owner (for imports before a game is running), then legacy
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
owned drain and exit survive Activity destruction or coroutine cancellation. The
actual JNI return/failure also ends the Kotlin launch by generation ownership,
independently of whether navigation was accepted. A destroyed A cannot resurrect
its launch UUID on a later settings apply, or clear the identity of replacement B.
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
observed session separately. Debug shares the main process, so settings reconfigure
that live sink. Release's separate emulation process reads changes at its next start;
there is no live cross-process settings IPC.
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

## Process association and historical native traces

On API 30+, an enabled process coalesces anonymous state into the platform's
`ActivityManager.setProcessStateSummary`: 84 bytes containing process/launch/session
UUIDs, source commit, known application Title, scope and observation flags. PDX2
uses one reserved byte for the closed native identity source. The initial launch
summary has no application Title until the synchronous runtime loader callback
resolves it; that callback also runs with diagnostics off for a later opt-in.
The Java callback uses the same Diagnostics instance monitor as launch begin/end
and failure capture; a static synchronized Class monitor would not protect that
state transition. The TLS observer belongs only to the ordinary JNI launch caller, holds no global
Java references, and notifies Java after releasing the sink mutex. Old PDX1
summaries retain their anonymous session/launch association but their untyped
Title is deliberately unknown, since it may be a shared SaveDataOwnerId. The
single daemon issues at most one call per second; a blocked Binder call keeps only
one pending replacement. No sensitive text, document location, PID or credentials
are encoded. Default-off startup creates no publisher thread or Binder call.
Updates are asynchronous and best effort: a rapid crash can leave an older or
absent association. Disabling requests clearing, without waiting on the UI.

Historical own-package/UID exits carry a stable anonymous `exit_record_id`,
`historical_record=true` and explicit `association_status`. Known previous IDs
are in `previous_*` fields; the root session/launch still belongs to the importer.
A private try-locked 128-entry store deduplicates successful written imports across
processes/restarts. Failed/busy persistence is reported and can allow re-import;
export also deduplicates known exit IDs across logging sessions. Older records
without IDs cannot be safely merged. Historical crash observations are counted
separately from current-session error records; neither count is a complete crash
count when logs are missing.

Exit identity version 2 uses timestamp/reason/PID/process scope, excluding the
mutable Android status. A later status observation is
`PREVIOUS_PROCESS_EXIT_REFINED` (INFO), with `previous_status` and the same stable
`exit_record_id`; it is not another crash. Legacy version-1 imports retain their
initial/current status aliases in `legacy_initial_exit_record_id` and
`legacy_exit_record_id`. Migration publishes one non-crash refinement with
`migration_reason=legacy_status_identity_v1_to_v2` even if that status was already
observed. A bounded private pending marker survives failed admission/flush until
the bridge is written. Existing historical rows are not silently rewritten, and
the merged trace retry budget remains at most three reads per exit.
The export's historical-observation count still deduplicates exact IDs; old
version-1 aliases can remain separate observations. Use the explicit bridge to
correlate them, and never interpret that counter as a count of unique crashes.

On API 31+, at most one most-recent own native exit per process is queried for a
trace. The platform may return null because its shared circular buffer was
reused. `PREVIOUS_NATIVE_TRACE` explicitly reports null, unsupported, read limit,
timeout, ownership mismatch, malformed input or a safe summary. The observer waits
at most 1500 ms for that task. A fixed-depth streaming AOSP protobuf subset uses a
4 KiB input buffer, an 8 MiB scan ceiling, 32768 work operations and a 1000 ms
parsing deadline. Unknown fields are skipped without buffering their contents;
at most 256 thread summaries and 64 KiB of candidate frame text are retained.
An uninterruptible platform task can
remain as one daemon until process death; the UI and session shutdown never join it.

Only fault-thread module basenames from a fixed list, relative PCs and validated
hex Build IDs are kept (up to 16 frames and 1700 bytes). Build IDs remain observed,
not authenticated module provenance. Known abort text can map to an enum such as
`ART_GC_INVALID_REFERENCE`, without exporting the text. Raw input is never saved
or included in ZIPs: no registers, absolute PCs, memory/stack dumps, maps, FDs,
command lines, fingerprint, user paths or raw abort message. These summaries do
not identify the corrupting writer and do not change signal/JNI/VMM handling.
No new permission, protobuf dependency, root setting or system security change is
required. The [Android API](https://developer.android.com/reference/android/app/ApplicationExitInfo#getTraceInputStream())
and [AOSP schema](https://android.googlesource.com/platform/system/core/+/refs/heads/master/debuggerd/proto/tombstone.proto)
define the supported source; unavailable trace data is never replaced by invented frames.

`trace_complete`, `trace_capture_status` and `trace_bytes_read` distinguish a
complete summary from input/byte/work/thread/deadline/read truncation. A stopped
scan can retain only completed allowlisted frames whose positive PID, UID,
fault TID, thread-map key and `Thread.id` were observed and agree. Incomplete
frames are omitted. Missing ownership suppresses frames; conflicting ownership,
malformed wire data and invalid UTF-8 do not yield a usable partial summary.
Partial trace attempts remain retryable within the existing three-start budget.
An outer observer timeout reports `observer_deadline`; it does not invent a
prefix which the blocked task has not returned.

## UI associations and owned legacy errors

`CONTENT_ASSOCIATION` and `CONTENT_IMPORT_RESULT` now carry typed Title/parent,
version/type, loader result and decision fields. A duplicate import still follows
the existing reported-success policy even if metadata parsing failed. Metadata is
actually parsed before that decision; diagnostics does not claim duplicates skip
parsing. Empty IDs have an explicit metadata status/reason. An eligible decision
is not a completed save: `CONTENT_ASSOCIATION_SAVED` follows actual stream closure,
while `CONTENT_ASSOCIATION_SAVE_FAILED` observes the existing IOException catch
without changing its return/propagation policy or promising fsync durability.

The owned AudioCore and Shader error adapters additionally emit
`AUDIO_BACKEND_ERROR` / `SHADER_COMPILER_ERROR` before the existing legacy logger
moves the message. Each component has a non-waiting four-per-second budget;
`prior_rate_suppressed` and the terminal `legacy_error_rate_suppressed` counter
expose omitted repeats. This is separate from queue/drop counts. The bridge is
inactive with diagnostics off and does not attach to every native log/hot path.
`fatality=unknown` is deliberate: a minimum-latency query failure can recover via
the audio backend's existing fallback and is not evidence of the crash cause.

## Provenance and timeline boundaries

`association_status` is `verified` only for the platform-retained anonymous state
with matching own process scope/UID; it verifies that retained state identity,
not its freshness at the crash or a causal game attribution. `unknown` never gains
a launch by guessing from timestamps. `time_correlated` is reserved for a future
explicit time-window collector, not currently emitted. ADB title/launch filters
use verified `previous_*` identities for historical records; they never attach
an old crash to the importer's current title/launch. Session filters retain the
legacy root-session behavior and additionally accept verified previous sessions.
Raw ADB crash context is still separate evidence; automatic live PID/window
association is not implemented or silently presented as verified.

`last_load_stage` preserves the compatible `last_stage` value. `last_guest_stage`
starts at `unknown`, changes to `guest_message_observed` only on a real guest
logging call, and can then retain one of the four parsed DFX progress markers.
It is a guest-reported progress observation, not independent validation of Lua,
menu/world state, or a frame presented to the display. Emulator loading stages
never synthesize those game stages. Lua errors retain their real message/source
fields without inventing a stack. TheSim:LuaPrint/LuaError forwarding, first guest
thread and submit/present timing remain unverified extensions.

`CONTENT_ROLE_OBSERVED` separates the input slot from its selected CNMT role.
A Patch CNMT in the base slot is an observation that can also occur in working
merged packages, not by itself a mismatch or cause of failure. No loading policy
changes. Each `LOADER_READY` explicitly reports full-document hash as
`not_computed`; NCA entry IDs never stand in for that hash. Optional exact SHA-256
hashing is deferred: it needs background cancellation, changed-input/failure
states, scope/algorithm/completion evidence and conservative cache invalidation.
SAF metadata is not immutable-byte proof; any future reader must use an independent
descriptor/position and never move the loader's shared FD offset. Import-time
verification must remain distinct from verification of a later launch.

The fixed snapshot replacement temporary file is also bounded to 64 KiB. Sudden
death can leave both the previous state and that temporary file, so the per-process
space allowance is 128 KiB beyond rotating logs. Only the finished JSONL is exported.
CI archives the same job's unstripped ARM64 `libskyline.so` in a separate artifact.
GNU Build ID must match the packaged ELF, debug info must exist, and exact source,
APK/ELF/symbol SHA-256 values are recorded in `native-symbols.json`. Ambiguous or
stripped-only candidates fail the job; symbols are never assigned to an older APK.

## Reviewed trace retention corrections

A null native trace can mean a transient service failure as well as overwritten
data. Native trace failures do not create a successful terminal marker. The
private store permits at most three persisted bounded attempts across process
starts, at most one attempt in any process; successful safe summaries and rejected
foreign ownership are terminal. Old ambiguous `t:` markers are not trusted.
`trace_attempt` and `trace_attempt_limit` make the budget visible. Export deduplicates
trace evidence by status/attempt/frame digest, allowing a later successful summary
to upgrade an earlier negative result, while historical crash identity remains
strictly deduplicated.

Producers accept up to 24 bounded fields under the same 4096-byte value budget.
Critical snapshots retain all accepted producer fields instead of taking the first
12. Snapshot-only metadata is in `snapshot_metadata`, including explicit
`fields_omitted=0`; legacy `fields.observed_event` remains readable. Whole old rows
may still be evicted to satisfy 32-row/64-KiB limits. A production native 16-field
trace fixture is generated after ordinary-log rotation and checked through the
actual export pipeline; frames at field13 and their counts remain in the ZIP.

## Separate UI metadata identities

`loader_jni` still fills legacy `AppEntry.titleId` from NACP SaveDataOwnerId;
selection, save paths and AppEntry serialization are unchanged. Diagnostic-only
JNI fields separately carry CNMT Title/parent/version, Program header ID, save
owner and selected Program archive entry ID through the real RomFile initializer.
Import, association and selected-update fields use those typed identities; legacy
values are explicitly named `legacy_*`, never called CNMT Title IDs.

A diagnostics-enabled metadata parse keeps at most 256 observations in process
memory, indexed by an internal digest of format/document location. That index is
not exported or described as a content hash. The original AppEntry is marked
`native_metadata_scan`; a later deserialized/same-location entry is
`cached_metadata_scan_not_verified_current`. A fresh process, old catalog, cleared
cache or default-off parse provides unknown diagnostic identities; no ROM is read
again just to fill a log field. Runtime native resolution, rather than this cache,
updates the application identity used by the platform process-state summary.

`MetadataJniMappingTest` compiles controlled host JNI code using production metadata
setters, the actual RomFile initializer and the production Java callback observer.
It distinguishes CNMT/Program/save-owner IDs and round-trips the actual summary
codec. It is neither an Android APK run nor a commercial NCA/cryptography test.

## Java launch failures during cleanup

The production emulation wrapper observes a Java `Throwable` before its owned
launch cleanup. `LAUNCH_FAILURE_CONTEXT` is an informational critical context
record, followed by a best-effort native flush with the existing 250ms deadline.
The original Throwable is rethrown without wrapping; normal `finally` cleanup
still ends only its generation. A per-thread weak Throwable association carries
an immutable last-owned observation into the installed uncaught handler.
`ANDROID_UNCAUGHT_EXCEPTION` retains that observation in explicit `failure_*`
fields, shares its `failure_id`, and delegates the same object to the previous
handler. The informational precursor is not an additional error observation.

`failure_application_title_id` has the native identity source shown alongside it.
Before native resolution it stays unknown; `failure_ui_application_title_id` can
separately retain an unverified UI metadata hint. If B has replaced A, A's failure
fields describe A's last owned observation while the root sink context can be B.
No old identity is written back into current native or platform state. Platform
history may therefore remain unknown after orderly cleanup; these local failure
records are separate evidence, not a fabricated verified exit association.
Sink failure, admission loss or abrupt death can still prevent persistence.

### Opt-in resource observer

See [diagnostics-resources-v1.md](diagnostics-resources-v1.md) for exact sources,
units, scope, availability and acceptance boundaries. New events use schema 1
and category `resource`: `RESOURCE_CAPABILITIES`, `RESOURCE_GUEST_SAMPLE`,
`RESOURCE_GPU_SAMPLE`, `HOST_MEMORY_SAMPLE`,
`RESOURCE_POST_PROGRESS_NO_NEW_PRESENT`, `RESOURCE_PRESENT_RECOVERED`,
`RESOURCE_WINDOW_PERSIST_FAILED`, `RESOURCE_WINDOW_PERSIST_RECOVERED`.
Cohort is a user label; NVN user buffer metrics and loaded NSO BuildID remain
explicitly unavailable/not observed. Resource window metadata has its own
`record_kind=resource_window_metadata` and no invented sequence number.

The writer reports the first resource-window persistence failure, then at most
once per minute while failures persist. `failure_stage`, `failure_reason`,
`error_code` and numeric `errno` are fixed privacy-safe codes; they contain no
path, session text, exception message or `strerror` output. The cumulative
`window_persist_errors` belongs to this writer/process, independent of main
JSONL `disk_errors` and aggregate `snapshot_errors`. A successful subsequent
checkpoint emits one recovery event with the previous failure codes. It proves
that diagnostic checkpoint completed, not guest storage success or durability
across power loss. Checkpoint cadence and resource sampling rates are unchanged.

### Guest Break request classification

`GUEST_BREAK_REQUEST` observes the real `kernel::svc::Break` reason before the
existing stack formatter. `notification_only=true` means reason bit 31 was set;
the other branch requests guest termination. This record neither establishes an
Android host signal nor diagnoses NVN/RAM exhaustion. A formatter exception can
prevent the original guest termination code from being reached; the existing
exception propagation, Kill/longjmp and Loader implementation are preserved.
See [P0 trace/window evidence](diagnostics-trace-window-p0.md).
