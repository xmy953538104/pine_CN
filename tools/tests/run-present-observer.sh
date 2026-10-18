#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
vkhpp=${PINE_VULKAN_HPP_ROOT:-app/libraries/vkhpp}
core=app/src/main/cpp/skyline/diagnostics
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pthread -DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0 \
    -I"$vkhpp" -I"$vkhpp/Vulkan-Headers/include" -I"$core" \
    "$core/diagnostics.cpp" tests/diagnostics/present_wrapper_test.cpp -o "$build/present"
"$build/present" "$build/logs"
echo 'PASS: actual pinned Queue::presentKHR SUCCESS/SUBOPTIMAL/OUT_OF_DATE/DEVICE_LOST, original typed exceptions and observed results'
