# DFX1 guest channel contract

Pine observes real guest OutputDebugString/LogManager messages. This protocol
does **not** establish that `TheSim:LuaPrint`, `WorldSim:LuaPrint`, Lua `print`
or a game's existing error handler reaches those interfaces. They are candidate
exits requiring calibration with actual game source. No DFX is synthesized from
loading content, DLC reads, a present, or memory text searches. Missing DFX is
inconclusive about `main.lua` execution.

## Wire and compatibility

One message begins exactly with `DFX1|`, followed by at most eight unique
`key=value` fields separated by `|`. Optional final LF/CRLF is accepted; leading
prefixes, substring matches and multiple physical records are not.

```text
DFX1|event=DFX_PROBE|probe_id=pine-cal-20261018-01|message=channel%20calibration
```

Events: `DFX_PROBE`, `DFX_MAIN_ENTER`, `DFX_MODINDEX_READY`, `DFX_MODMAIN_OK`,
`DFX_HAMLET_ENTER`, `DFX_LUA_ERROR`. Legacy strict first-token messages such as
`DFX_MAIN_ENTER: detail` remain supported. DFX1 is a separate parser, rather
than weakening the original token rule.

Values are UTF-8 with optional `%HH` escapes. Escape `%`, `|`, CR/LF and TAB;
`+` is literal. Invalid UTF-8, NUL, malformed escapes, duplicate/unknown keys
and unknown events are rejected. The original bounded guest message remains in
`GUEST_*`; admitted rejections also emit `DFX_PROTOCOL_REJECTED`.

| Field | Decoded bytes | Meaning |
|---|---:|---|
| event |32|Required allowlisted event|
| probe_id |64|Required unique ASCII `[A-Za-z0-9_.-]`, fresh per calibration|
| file |192|Actual game-provided script path; omit if unknown|
| line |10|Actual positive decimal line, at most2147483647; omit if unknown|
| function |96|Actual game-provided function name|
| message |384|Original game message/error|
| traceback |768|Game-provided traceback, escaped on wire|
| phase |64|Game-provided label, never an emulator inference|

Maximum wire size2048 bytes including optional terminator; combined decoded
values1024 bytes. Derived legacy/DFX1/rejection records share a budget of16
messages/second/session. Original guest records still use the general bounded
queue and2048-byte text/redaction limit. Rate suppression is separate from queue
drops: `protocol_prior_rate_suppressed` appears on the next admitted DFX1 record;
`SESSION_SUMMARY.guest_protocol_rate_suppressed` gives the session total.

Derived records retain `message` (original bounded/redacted wire), `source`,
`bridge_protocol=DFX1`, `probe_id`, and `script_fields_source=guest_DFX1_payload`.
Payload locations are named `game_script_file/line/function`; text is
`game_message`, `game_traceback`, `game_phase`. LogManager's independently supplied
`guest_file/line/function/thread` stay separate. OutputDebugString never invents
source positions. Export also redacts percent-escaped private paths/credentials
in this known protocol. Rotation/rate/queue/disk loss limits every absence claim.

## Minimal game-project handoff

Inject the game's **verified** logging exit as `emit`; its binding is not assumed.
This documentation snippet is not a ROM patch or a replacement error handler.

```lua
local function dfx_escape(s)
    return (s:gsub("[%%|\r\n\t]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end
local function dfx_probe(emit, unique_id)
    emit("DFX1|event=DFX_PROBE|probe_id=" .. dfx_escape(unique_id)
        .. "|message=channel%20calibration")
end
```

1. Emit one unique probe through one candidate exit. Obtain raw and derived
   records for the same Session/Launch/source. Raw-only means inspect the wire
   prefix/transport; neither means inspect the game native bridge. Neither result
   proves that Lua did not execute.
2. After calibration, use an isolated authorized controlled error through the
   existing game error exit. Supply its real script path/line/error/traceback,
   preserving existing propagation/error UI. Do not replace `require` or
   `coroutine.resume`, swallow errors, change save callback ordering, or introduce
   frequent persistence.
3. Expand guest stage markers only after both checks pass. A stage is a claim
   carried by the game message, not Pine's independent proof of a menu/world.

Offline consistency check; no upload or extraction:

```powershell
python tools/verify-dfx1-probe.py pine-diagnostics.zip --probe-id pine-cal-20261018-01 --source OutputDebugString
python tools/verify-dfx1-probe.py pine-diagnostics.zip --probe-id YOUR_NEW_ERROR_ID --expect-event DFX_LUA_ERROR --expect-file ACTUAL_SCRIPT_PATH --expect-line ACTUAL_LINE
```

The verifier requires original and derived events; snapshots alone do not pass.
It validates log consistency, not device authenticity or game source correctness.
Host fixtures/JVM/Windows tests cover Pine parsing/export/collection only. A real
game-to-native Lua bridge remains unrun until a calibrated producer is supplied.
