#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
compiler=${CXX:-c++}
"$compiler" -std=c++20 -Itests/diagnostics/backing_stubs -Iapp/src/main/cpp/skyline tests/diagnostics/backing_test.cpp -o "$build_dir/backing-test"
"$build_dir/backing-test"
"$compiler" -std=c++20 -Iapp/src/main/cpp/skyline tests/diagnostics/content_macro_test.cpp -o "$build_dir/content-macro-test"
"$build_dir/content-macro-test"
echo 'Content diagnostics tests passed (production Backing and diagnostic fast gate).'
