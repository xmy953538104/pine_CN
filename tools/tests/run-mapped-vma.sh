#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"${CXX:-c++}" -std=c++20 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -pthread \
    -isystem app/libraries/vkma/include -isystem app/libraries/vkhpp/Vulkan-Headers/include \
    -Iapp/src/main/cpp/skyline tests/diagnostics/mapped_allocation_test.cpp -o "$build/mapped-allocation-test"
timeout 30 "$build/mapped-allocation-test"
