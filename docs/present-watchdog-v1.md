# No-new-Present observer: offline design v1

Status: **design and synthetic Python model only, default OFF**. No CMake,
Android, presentation engine, NCE, sampler or APK integration is added. The
model never reads a real counter, thread, register, fence, file or clock.

The only real-case evidence supplied for this task is Owner reporting:

- Owner verified that Present=18,401 did not advance for 69.528262 seconds,
  then manually exited. This is the non-advancing interval's duration.
- Owner supplied correlated evidence for both the official build and R11:
  674 → SvcBreak → Kill, associated with Android reason=2, status=11.

These reports are not inputs to the model and are not reproduced here. They do
not identify GPU, Lua, a deadlock or another cause. A watchdog could preserve
correlated observations earlier; it cannot supply the missing causal evidence.

## Existing evidence and missing providers

Source baseline: `40419e0e04578a91de00f5709a0cdb4633f81107`.
[PresentResult/EmitProgress](../app/src/main/cpp/skyline/diagnostics/diagnostics.cpp),
[wrapper observer](../app/src/main/cpp/skyline/diagnostics/present.h), and
[public API comments](../app/src/main/cpp/skyline/diagnostics/diagnostics.h)
define the current semantics. Present counters are diagnostics-enabled,
per-launch observations, read as independent atomic lower bounds. They are not
a simultaneous GPU snapshot or evidence of visible pixels/gameplay/Lua.

| Desired field | Baseline evidence | Future safe acquisition; model behavior now |
| --- | --- | --- |
| present_returns | Existing atomic count of Vulkan wrapper returns, including caught SystemError observations | Primary input supplied synthetically; no reader connected |
| accepted/other/result/wrapper exceptions | Existing host-result counters; accepted includes SUCCESS/SUBOPTIMAL without wrapper exception | Read-only epoch-bound snapshot API still needed; fields default not_connected |
| last successful stage | Existing LastStage API uses a mutex | Future bounded copied publication; do not call a blocking getter from Present |
| process/launch/diagnostics session | Existing run/launch/context and Activity generation concepts | Future coordinator must bind all three and mint a new local session token on any scope change |
| normal gameplay progressing | First Present and loader success cannot establish this | Explicit authorized phase/Owner confirmation plus fresh Present advances; unknown blocks arming |
| guest/host thread state | No general coherent, safe watchdog snapshot exists | Future cooperative safe point, fixed role/state codes only; no forced all-thread suspension |
| CPU progress | No model-connected guest heartbeat | Future documented safe-point count; host CPU time alone is not guest/Lua progress |
| GPU submission/fence | Engine objects exist, not a safe observer packet | Future nonblocking published submit/fence observation with age; never wait on a fence to sample it |
| recent exception | Existing exception events are not a coherent resident last-exception snapshot | Future fixed code/time publication on ordinary safe paths; no raw messages or stacks |

The detector watches **present_returns**, not accepted-only frames: returning
errors are still new Present observations. Continuing error returns need their
own interpretation; they do not satisfy this detector's no-new-observation
condition. Increasing returns do not establish successful submission or visible
picture updates: a frozen picture with continuing returns can escape detection.
Stopped growth establishes only no new return/exception observation at this
layer, not GPU/Lua causality. No extra per-Present counter or hot-path operation
is proposed here.

## Arming, timing and lifecycle

`start_session(token, now_ns)` is an explicit lifecycle operation. Tokens are
positive, increasing, opaque model generations, not PIDs or reusable title IDs.
An Observation cannot start a session. A future coordinator must reject stale
process/launch/logging callbacks before assigning this token. Logging off/on,
new launch/process and counter-domain replacement each require a new generation;
the existing counters must never be treated as live while diagnostics are off.

After a baseline, require **normal_progress_confirmed=True and two consecutive
eligible polls with fresh counter increases** before watching. All samples must affirm unpaused,
foreground and surface-available. This explicit start gate is not implemented
in the app; without a valid provider the real design remains unarmed. A menu,
loading screen, FIRST_PRESENT_OBSERVED or a renderer counter alone cannot set it.

Default timing is a supplied monotonic nanosecond clock, 1 Hz observation and a
4-second threshold (configuration accepts 3–5 seconds). The interval starts at
the last fresh *observed* increase, not an independently known GPU/scanout time.
Detection latency depends on cadence and freshness; it is not an exact device
4-second response guarantee. Calls faster than the configured interval do not
accumulate qualification. The model has no scheduler, sleeps or clock reads.

| Transition | Effect |
| --- | --- |
| OFF | No session, events, sampling or inspected tick inputs; current production tree has no connection at all |
| Two eligible increases | waiting_for_progress → watching |
| Eligible unchanged count for ≥ threshold | no_new_present_observed; cause stays undetermined |
| First fresh increase after a reported episode | One present_observation_resumed event; require another increase before watching again |
| Pause/background/surface unavailable or any unknown eligibility bit | Suspend and discard elapsed time immediately, including a rate-limited tick; resumption requires new qualification |
| Missing/stale/future/repeated primary sample, counter rollback, clock rollback, or observer gap >2 seconds | Discard progress; no stall inferred from the unobserved interval; require new advances |
| Exit requested/end_session | Terminal for that token; late callbacks cannot warn or restart it |
| New explicit session | Reset episode/cooldown/budget/counter state; old-token observations and packets cannot cross into it |

One warning per episode; no periodic repetition of the same warning. Distinct
episodes have a 30-second warning cooldown. If still stalled when cooldown
expires, one deferred warning is allowed. At most three warnings and their
three corresponding recovery observations per session. A pause, data loss or
session change is not recorded as recovery. Neither event means that the game
stopped or recovered. The model returns records without keeping an event log.

## Bounded optional snapshot

The model consumes a preformed Packet; it never calls a sampler callback. A
Packet may supply at most 11 fixed evidence fields (hard construction limit 32),
with fixed enum codes or bounded integers. Unknown/free-form fields, exception
messages, paths, credentials, registers and stack bodies are not accepted.
Thread fields represent a future designated role, not an enumeration of every
thread. There is no thread dump, filesystem search or upload in this prototype.

Each field retains its origin and observed/unavailable/unsupported state.
Expired/future timestamps become stale with no value. Absent adapters are
not_connected; another session is session_mismatch; duplicated fields are
rejected as a packet. Fixed schema status rows remain available even when no
field values are supplied. The data_kind is always synthetic_offline.

The optional packet's **claimed**, not measured, cost must be ≤5 ms. The model
cannot prove this acquisition time. Over-budget or oversized packets drop their
values while retaining the primary no-new-Present observation. JSON is capped
at 4,096 bytes including the event header (configurable down to 1,024); an
over-budget snapshot becomes an empty byte_budget_exceeded record. Processing
has fixed field limits, no iterable callbacks and no retained history.

Before future production integration, a separate review must implement and
measure a bounded normal-thread observer: one pending request, bounded queue,
epoch/token check before and after copied data, and deadline-limited reads of
already published safe-point packets. It must mark unavailable/busy/expired
instead of waiting for threads. The 5 ms proposal concerns resident snapshot
admission/copy, not a promise that all threads reach a safe point in 5 ms. The
observer must never synchronously suspend all guest/host threads, change game
execution, call blocking fence APIs, or add raw-signal-handler allocation,
locks, stack unwinding, I/O or JNI. No automatic kill/restart/export is proposed.

The outer feature gate must precede any scheduler, publisher or allocation.
Pause/surface/exit delivery and shutdown ownership need device tests; a testable
Python state machine does not establish those lifecycle guarantees or hot-path
overhead. A synthetic claimed packet is not independently authenticated evidence.

## Reproduce offline

Python 3.10+ standard library only:

```sh
python3 tools/tests/test-present-watchdog-model.py
python3 tools/present_watchdog/model.py --demo
```

The demo generates counter 0→1→2, silence, then 3→4. It prints only a synthetic
warning and recovery with optional fields not_connected. No file argument,
directory scan, real log replay, device or private data is used. Tests cover
startup, progression, threshold boundary, lifecycle, cooldown, deduplication,
session/clock/counter changes, unavailable sampling and all packet budgets.

The dedicated read-only workflow runs Python 3.10/3.12, binds test logs and the
synthetic demo to the exact checkout SHA and file hashes, and builds no APK.
This is not real thread capture, a GPU/Lua diagnosis or Android/device evidence.
