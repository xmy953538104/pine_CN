# Native sink transport fixtures

`native-sink.jsonl` and `native-sessions.jsonl` are raw output copied from files
written by the production `app/src/main/cpp/skyline/diagnostics/diagnostics.cpp`.
The controlled host generator is `tests/diagnostics/adb_fixture.cpp`. It calls the
public Start/Emit/SetTitleId/Stage/Flush/Stop API; it contains no JSON serializer.
The fake ADB implementations replay these records unchanged. Only discovery,
connection failures and deliberately invalid input are simulated by the fakes.

The capture corpus has three sessions and includes unknown base context, known
base context, a distinct DLC content ID, UTF-8, guest and lifecycle records. The
session corpus has forty sessions, including IDs that collide after filename
sanitization, truncation or Windows case folding, plus real sink summaries.
The IDs and payloads are controlled test inputs. A `DLC_DATA_READ` fixture means
the generator called Emit with that event name; it is not evidence of a device,
guest or game performing a read.

Run the production sink again and verify the checked-in contract:

```sh
bash tools/tests/check-native-adb-fixtures.sh
```

After an intentional contract change, inspect the new output and regenerate:

```sh
bash tools/tests/check-native-adb-fixtures.sh --update
```

`tools/tests/run-native-diagnostics.sh` runs the verification in CI. The checker
compares parsed records while retaining all keys, value types, event payloads,
title context, sequence, order and final accepted/written/disk-error counters.
It normalizes only wall/monotonic clocks, host thread identity and try-lock drop
counts, which can vary with scheduling. The tracked JSONL keeps those actual
recorded values. A field or nesting change fails verification until reviewed.
