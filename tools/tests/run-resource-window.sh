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
for name in ('empty','framing','count','bytes','age','clock','session','quoted','maximum-metadata','partial-writes','stale','exception','ingestion-time',
             'layered-dense','layered-count','layered-full-budget','layered-bytes','layered-age','layered-clock','layered-max-time','layered-session','layered-allocation',
             'layered-model-100','layered-model-200','layered-model-400','layered-model-1000'):
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
    retention=meta['retention']; critical=retention['critical']; recent=retention['recent']
    assert retention['policy']=='critical_reserve_recent_borrows_unused'
    assert retention['age_scope']=='both_ingestion' and retention['eviction_scope']=='age_clock_global_and_class_limits'
    assert critical['record_limit']==16 and critical['byte_limit']==32768
    assert 0<=critical['records']<=16 and 0<=critical['bytes']<=32768
    assert critical['records']+recent['records']==meta['retained_records']
    assert critical['bytes']+recent['bytes']==meta['retained_bytes']
    assert recent['record_capacity']==240-critical['records'] and recent['byte_capacity']==129024-critical['bytes']
    assert recent['records']<=recent['record_capacity'] and recent['bytes']<=recent['byte_capacity']
    assert critical['evicted_records']+recent['evicted_records']==sum(meta[key] for key in ('evicted_age','evicted_bytes','evicted_count','evicted_clock'))
    assert critical['evicted_bytes']+recent['evicted_bytes']==sum(meta[key] for key in ('evicted_age_bytes','evicted_budget_bytes','evicted_count_bytes','evicted_clock_bytes'))
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
    if name=='layered-full-budget': assert meta['retained_bytes']==129024 and critical['bytes']==32768
    if name=='layered-bytes': assert critical['bytes']==1024 and recent['bytes']>129024-32768
    if name=='layered-dense' or name=='layered-count' or name.startswith('layered-model-'):
        critical_events={'CRITICAL','GUEST_BREAK_REQUEST','GUEST_SIGNAL','GUEST_RUNTIME_ERROR'}
        critical_rows=[(row,line) for row,line in zip(values[1:],lines[1:]) if row['event'] in critical_events]
        assert len(critical_rows)==critical['records'] and sum(len(line) for _,line in critical_rows)==critical['bytes']
        sequences=[row['sequence'] for row in values[1:]]
        assert sequences==sorted(sequences) and len(sequences)==len(set(sequences))
        assert all(row['session_id']=='synthetic-session' and row['process_run_id']=='synthetic-process' and row['monotonic_us']==1000000-row['sequence'] for row in values[1:])
    if name=='layered-dense': assert critical['records']==3 and 0<recent['records']<100 and meta['reason']=='guest_break_observed'
    if name=='layered-count': assert critical['records']==16 and recent['records']==224 and [row['sequence'] for row in values[1:]]==list(range(2,17))+list(range(33,258))
    if name=='layered-session': assert values[1:]==[] and critical['records']==recent['records']==0
print('PASS: independent Python JSONL parse, exact class/aggregate counts/bytes/evictions, shared reserve/order/age scope, UTF-8 escaping, omissions and fixed file bounds')
PY
