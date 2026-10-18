# Launch observations / DFX1 candidate

The application candidate is PR#4 (`feat/diagnostics-black-screen-v1`). Exact
source, APK/symbol hashes and terminal CI results are maintained in that PR.
The owner confirmed908c/O2 simulation speed restored. That earlier device result
is not acceptance of this changed APK. No private device logs or game files are
committed. No main merge or Release is performed.

## What the records mean

| Record/field | Actual observation and limit |
|---|---|
| FIRST_PRESENT_OBSERVED | First accepted Vulkan present result observed while diagnostics was enabled for this launch. SUCCESS/SUBOPTIMAL may still display a blank image. No visible-pixel, menu or Lua claim. |
| LAUNCH_PROGRESS | Writer timer every5s, with cumulative observed present results, wrapper errors, elapsed time and last load/guest stage. Timer progress is diagnostic-writer health, not guest execution progress. |
| LAUNCH_HOST_EXIT_REQUESTED | Accepted Activity generation's host/menu exit request, captured before logger drain/process kill. Menu uses `request_source=menu_exit_confirmed`. It is not an observed native return or crash. |
| LAUNCH_ENDED | Actual JNI execution control flow returned/caught an exception. `end_reason` describes only that path; host stop request is separate. Worker errors and unobserved process death must not be relabeled as successful game exits. |
| base_program_nca | Selected base Program container ID. Legacy `program_nca_id` retains that scope. |
| effective_exefs_program_nca/source | ExeFS actually selected by the native loader/patch-manager path; can be base, update or direct NCA. Missing ExeFS has an empty effective identity and separate candidate ID. Loading/executing it is a later observation. |

The present hook updates atomics only; JSON/queue/disk work is on the diagnostic
writer. The pinned Vulkan-Hpp wrapper's thrown SystemError codes are observed
and the same exceptions rethrown. `counter_snapshot` identifies independently
sampled atomic lower bounds, which may overlap a present. Enabling midway through
a launch cannot recover earlier frames or prove the first-ever game present.
The existing game/loader/update selection, guest filesystem semantics and wrapper
error propagation stay in place. No large input-document hash is computed.

## Retention, admission and failure limits

`critical-state.jsonl` plus fixed `critical-state-1..7.jsonl` preserve up to the
configured retention count (maximum8) across sessions. Each file is at most64KiB;
each current cache has at most32 last-state entries, including up to8 accepted
launch endings. These are state snapshots, not event replay or a lossless journal.
Export includes only these fixed names and the existing fixed log allowlist.
Oldest state/oversized state can be evicted; summary counters expose this.

Queue storage is bounded. Admission no longer contends on the writer/metadata
mutex: ring reservation and context observation have64-attempt budgets. Producer
registration is **lock-free, not wait-free** and may retry; infrequent ordinary
metadata/lifecycle setters may wait. A preempted reserved producer can delay the
writer. Full queue, retry exhaustion, allocation and disk failure can still lose
records, including critical ones. Counts and critical drops remain explicit.
Flush waits by reservation/completed ticket rather than a racy accepted count.
First allocation failure and late producer registration across Stop/Start have
dedicated production fault-injection regressions; hooks are excluded from APKs.
No JSON/allocation/lock/JNI/join was added to a raw signal handler.

Prior process-exit associations that were unknown remain unknown. A missing
ending/snapshot cannot classify a crash, nor prove that a game phase did not run.
The [DFX1 contract](diagnostics-dfx1-protocol.md) covers only genuine guest messages
and a game-side calibration procedure. `TheSim:LuaPrint` and other candidate Lua
exits are not established bridges. Large events stay complete in bounded local
JSONL; logcat can carry a brief `oversized_event_in_jsonl` record instead.

## Owner checks with existing lawful content

1. Install the new same-signer O2 APK over the908c preview; retain application
   data/content. Confirm its displayed source matches PR#4's APK identity. Do not
   substitute an O0/old APK or reinstall/clear data to hide a signer mismatch.
2. Enable normal diagnostics and retain enough sessions. Run a known working
   title briefly, then an already available black-screen case unchanged. Observe
   first-present/5s counters and the real content/ExeFS/CTR/BKTR events. No new
   CleanPatch, mod, game rebuild or ROM modification is requested.
3. Confirm exit through Pine's menu. Reopen/export the allowlisted bundle and
   inspect the prior launch's `LAUNCH_HOST_EXIT_REQUESTED` and final progress.
   An actual later JNI return, if captured, is a separate event. Missing/unknown
   values do not become a crash diagnosis.
4. Capture USB/already-authorized wireless ADB locally:

```powershell
.\tools\pine-diag.ps1 -ListDevices
.\tools\pine-diag.ps1 -Serial YOUR_DEVICE -IncludeCrashLog -OutputDirectory .\pine-diag
```

5. Use one fresh unique DFX1 probe from one game-side candidate exit, then an
   isolated controlled error with actual script fields, then more stages. The
   [offline verifier](../tools/verify-dfx1-probe.py) requires raw/derived payload
   equality and refuses redacted/unknown fields. No raw private logs need leave
   the owner's machine.

Host/native/JVM/fake-ADB checks are not Android/game execution. The known KVM
restriction is not bypassed or repeatedly retried. This candidate still needs
authorized ARM64 installation, startup/menu/SAF validation and a real game bridge
calibration; these remain separate from green CI and historical908c results.
