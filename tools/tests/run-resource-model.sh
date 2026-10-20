#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
resource_build=$(mktemp -d)
trap 'rm -rf "$resource_build"' EXIT
resource_flags=(-std=c++20 -O2 -Wall -Wextra -Werror)
if [[ ${PINE_DIAG_SANITIZE:-0} == 1 ]]; then
    resource_flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${resource_flags[@]}" -Iapp/src/main/cpp/skyline/diagnostics \
    tests/diagnostics/resource_model_test.cpp -o "$resource_build/resource-model"
timeout 30 "$resource_build/resource-model"
