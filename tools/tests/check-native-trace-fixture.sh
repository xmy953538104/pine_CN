#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -Iapp/src/main/cpp/skyline/diagnostics \
    app/src/main/cpp/skyline/diagnostics/diagnostics.cpp tests/diagnostics/trace_snapshot_test.cpp -o "$build/trace-snapshot"
"$build/trace-snapshot" "$build/logs"
python3 - "$build/logs/critical-state.jsonl" "${1:-}" <<'PY'
import json,pathlib,sys
rows=[json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
row=next(item for item in rows if item['snapshot_metadata']['observed_event']=='PREVIOUS_NATIVE_TRACE')
assert len(row['fields'])==17 and row['fields']['frames_seen']=='16'
assert row['fields']['frames'].count('libskyline.so+0x1234@')==16
assert row['snapshot_metadata']['fields_omitted']==0
row.update(sequence=1,wall_clock_ms=1792166400000,monotonic_us=123456,thread='native-controlled',dropped_count=0)
row['drop_counts']={key:0 for key in row['drop_counts']}
actual=json.dumps(row,ensure_ascii=False,separators=(',',':'))+'\n'
fixture=pathlib.Path('tools/tests/fixtures/native-trace-snapshot.jsonl')
if sys.argv[2]=='--update': fixture.write_text(actual)
else: assert fixture.read_text()==actual,'Regenerate native-trace-snapshot fixture after inspecting the real sink diff'
print('PASS: production 16-field native trace survives snapshot and complete normal-log rotation')
PY
