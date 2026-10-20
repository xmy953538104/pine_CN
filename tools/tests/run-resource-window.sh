#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
resource_window_build=$(mktemp -d)
trap 'rm -rf "$resource_window_build"' EXIT
resource_window_flags=(-std=c++20 -O2 -Wall -Wextra -Werror)
if [[ ${PINE_DIAG_SANITIZE:-0} == 1 ]]; then
    resource_window_flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${resource_window_flags[@]}" -Iapp/src/main/cpp/skyline/diagnostics \
    tests/diagnostics/resource_window_test.cpp -o "$resource_window_build/resource-window"
timeout 45 "$resource_window_build/resource-window" "$resource_window_build/fixtures"
python3 - "$resource_window_build/fixtures" <<'PY'
import json
from pathlib import Path
import sys
root=Path(sys.argv[1])
for name in ('empty','framing','count','bytes','age','clock','session','quoted','maximum-metadata','partial-writes','stale','exception','ingestion-time'):
    path=root/name/'resource-window.jsonl'
    data=path.read_bytes(); lines=data.splitlines(keepends=True)
    assert data.endswith(b'\n') and len(data)<=131072 and len(lines[0])<=2048, name
    values=[json.loads(line) for line in lines]
    meta=values[0]
    assert meta['record_kind']=='resource_window_metadata' and 'sequence' not in meta
    assert meta['scope']=='writer_retained_recent_window_not_complete_history'
    assert meta['time_scope']=='writer_ingestion_monotonic_not_event_execution'
    assert meta['retained_records']==len(values)-1<=240
    assert meta['retained_bytes']==sum(map(len,lines[1:]))<=129024
    assert all(len(line)<=32768 for line in lines[1:])
    assert meta['earliest_monotonic_us'] is None if not values[1:] else meta['earliest_monotonic_us']<=meta['latest_monotonic_us']
    if values[1:]: assert meta['retained_span_us']<60000000
    if name=='count': assert [v['i'] for v in values[1:]]==list(range(1,241)) and meta['evicted_count']==1
    if name=='bytes': assert meta['evicted_bytes']==1 and meta['omitted_oversize']==2
    if name=='age': assert [v['i'] for v in values[1:]]==[1,2] and meta['retained_span_us']==1
    if name=='clock': assert meta['clock_resets']==1 and meta['evicted_clock']==2
    if name=='session': assert meta['session_id']=='session-c' and values[1:]==[{'session_c':True}]
    if name=='quoted': assert meta['session_id']=='session"\\\n\t测试'
    if name=='exception': assert meta['reason']=='exception_observed' and values[1:]==[{'event':'GUEST_SIGNAL','category':'exception','sequence':44}]
    if name=='ingestion-time': assert meta['retained_span_us']==1 and [v['monotonic_us'] for v in values[1:]]==[9999999,1]
print('PASS: independent Python JSONL parse, exact retained counts/bytes, age span, UTF-8 escaping, omission metadata and fixed file bounds')
PY
