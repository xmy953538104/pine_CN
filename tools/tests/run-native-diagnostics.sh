#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
core=app/src/main/cpp/skyline/diagnostics
flags=(-std=c++20 -O2 -Wall -Wextra -Werror -pthread)
if [[ ${PINE_DIAG_SANITIZE:-0} == 1 ]]; then
    flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" -I"$core" -c "$core/diagnostics.cpp" -o "$build/diagnostics.o"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/core_test.cpp -o "$build/core_test"
"$build/core_test" "$build/logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/process_test.cpp -o "$build/process_test"
"$build/process_test" "$build/process-logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" -Iapp/src/main/cpp -Iapp/src/main/cpp/skyline "$build/diagnostics.o" tests/diagnostics/guest_test.cpp -o "$build/guest_test"
"$build/guest_test" "$build/guest-logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/startup_test.cpp -o "$build/startup_test"
"$build/startup_test" "$build/startup-logs"
"${CXX:-c++}" "${flags[@]}" -Iapp/src/main/cpp/skyline "$build/diagnostics.o" tests/diagnostics/identity_test.cpp -o "$build/identity_test"
"$build/identity_test" "$build/identity-logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/admission_test.cpp -o "$build/admission_test"
"$build/admission_test" "$build/admission-logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" tests/diagnostics/queue_test.cpp -o "$build/queue_test"
"$build/queue_test"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/launch_progress_test.cpp -o "$build/launch_progress_test"
"$build/launch_progress_test" "$build/launch-progress-logs"
"${CXX:-c++}" "${flags[@]}" -I"$core" "$build/diagnostics.o" tests/diagnostics/legacy_error_test.cpp -o "$build/legacy_error_test"
"$build/legacy_error_test" "$build/legacy-logs"
for test in pending_signal utf16; do
    "${CXX:-c++}" "${flags[@]}" -Iapp/src/main/cpp/skyline "tests/diagnostics/${test}_test.cpp" -o "$build/${test}_test"
    "$build/${test}_test"
done
python3 - "$build" <<'PY'
import json,pathlib,sys
root=pathlib.Path(sys.argv[1])
for folder,session in [('logs','native-test'),('guest-logs','guest-test'),
                       ('process-logs/main','process-main'),('process-logs/emulation','process-emulation')]:
    paths=[path for path in (root/folder).rglob('*.jsonl')
           if path.is_file() and not path.is_symlink() and path.parent.name!='write-failure' and not path.name.startswith('critical-state')]
    assert paths
    for path in paths:
        rows=[json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
        assert rows and all(row['schema']==1 and row['session_id'] in ({'guest-test','guest-marker-test'} if folder=='guest-logs' else {session}) for row in rows), path
        assert all(a['sequence']<b['sequence'] for a,b in zip(rows,rows[1:])), path
        assert all(isinstance(row['title_id'],str) and isinstance(row['fields'],dict) for row in rows)
        assert all(isinstance(key,str) and isinstance(value,str) for row in rows for key,value in row['fields'].items())
        if folder!='guest-logs': assert path.stat().st_size<=65536
    if folder.startswith('process-logs/'):
        scope=folder.rsplit('/',1)[1]
        assert len(paths)==3
        retained=[]
        for path in sorted(paths,reverse=True):
            retained.extend(json.loads(line) for line in path.read_text(encoding='utf-8').splitlines())
        assert all(a['sequence']+1==b['sequence'] for a,b in zip(retained,retained[1:])), scope
        assert all(row['fields'].get('scope')==scope for row in retained if row['event']!='SESSION_SUMMARY')
        summary=retained[-1]
        assert summary['event']=='SESSION_SUMMARY' and summary['fields']['accepted']==summary['fields']['written']
rows=[json.loads(line) for line in (root/'logs'/'encoding.jsonl').read_text(encoding='utf-8').splitlines()]
context=next(row for row in rows if row['event']=='TITLE_CONTEXT')
assert context['title_id']=='0100000000001000' and context['fields']['title_id']=='separate-dlc-id'
assert any(row['event']=='THREAD_UTF8' and '\ufffd' in row['thread'] for row in rows)
assert all('hidden' not in row['thread'] for row in rows)
print('PASS: strict JSON/UTF-8, schema, per-session sequence, title context, field types and file bounds')
PY
bash tools/tests/check-native-adb-fixtures.sh

bash tools/tests/check-native-trace-fixture.sh
