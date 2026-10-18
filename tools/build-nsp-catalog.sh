#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
output=${1:?Usage: tools/build-nsp-catalog.sh OUTPUT_BINARY}
compiler=${CXX:-c++}
"$compiler" -std=c++20 -O2 -Wall -Wextra -Werror -Iapp/src/main/cpp/skyline \
    app/src/main/cpp/skyline/catalog/wire.cpp \
    app/src/main/cpp/skyline/catalog/catalog.cpp \
    app/src/main/cpp/skyline/catalog/json.cpp \
    tools/nsp-catalog.cpp -o "$output"
