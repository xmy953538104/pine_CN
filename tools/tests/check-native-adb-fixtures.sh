#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ $# -gt 1 || ( $# == 1 && $1 != --update ) ]]; then
    echo 'usage: check-native-adb-fixtures.sh [--update]' >&2
    exit 2
fi
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
core=app/src/main/cpp/skyline/diagnostics
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -I"$core" \
    "$core/diagnostics.cpp" tests/diagnostics/adb_fixture.cpp -o "$build/adb_fixture"
"$build/adb_fixture" "$build/capture" capture > "$build/native-sink.jsonl"
"$build/adb_fixture" "$build/sessions" sessions > "$build/native-sessions.jsonl"
python3 tools/tests/check-native-adb-fixtures.py "$build" "$@"
