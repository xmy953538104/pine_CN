#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
core=app/src/main/cpp/skyline/diagnostics
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I"$core" "$core/diagnostics.cpp" tests/diagnostics/core_test.cpp -o "$build/core_test"
"$build/core_test" "$build/logs"
python3 - "$build/logs" <<'PY'
import json,pathlib,sys
for path in pathlib.Path(sys.argv[1]).glob('*.jsonl'):
    rows=[json.loads(line) for line in path.read_text(encoding='utf-8').splitlines()]
    assert rows and all(row['schema']==1 and row['session_id']=='native-test' for row in rows)
    assert all(a['sequence']<b['sequence'] for a,b in zip(rows,rows[1:]))
print('PASS: JSON schema, UTF-8 decoding, session and ordered sequence')
PY
if [[ -f tests/diagnostics/guest_test.cpp ]]; then
    "${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I"$core" -Iapp/src/main/cpp -Iapp/src/main/cpp/skyline "$core/diagnostics.cpp" tests/diagnostics/guest_test.cpp -o "$build/guest_test"
    "$build/guest_test" "$build/guest-logs"
fi
