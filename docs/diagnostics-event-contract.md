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

## Content events and their meaning

| Event | Evidence required |
|---|---|
| `PATCH_SELECTED` | Actual entry-point selection; first-enabled and URI fallback may select differently. This does not change selection policy. |
| `PATCH_ROMFS_OPENED` | Actual patch RomFS opening path. A call to PatchRomFS alone does not prove BKTR use; correlate the NCA CTR/BKTR branch event. |
| `DLC_REGISTERED` | DLC loader registration, not storage open or successful reads. |
| `DLC_STORAGE_OPENED` | A requested Data ID matched an opened backing. |
| `DLC_DATA_READ` | Actual low-level read result, not Backing::Read's total-size return value. A short read must not be represented as full requested-byte success. |
| `DLC_RUNTIME_ERROR` | An observed DLC read/open error, retaining original propagation. |

A candidate NCA, selected NCA, overwritten candidate and rejected NCA are distinct
observations. Base ROM type use for update/DLC and the original updateFd > 0 test
remain unchanged in v1. Version and Title ID are reported only when available.

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
