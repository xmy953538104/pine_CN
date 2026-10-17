#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
output=$1
case "$(uname -s)" in Linux) jni_platform=linux ;; Darwin) jni_platform=darwin ;; *) echo 'Host metadata JNI fixture requires Linux/macOS JDK headers' >&2; exit 2 ;; esac
mkdir -p "$(dirname "$output")"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -shared -fPIC \
    -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/$jni_platform" -Iapp/src/main/cpp/skyline \
    tests/diagnostics/metadata_jni_fixture.cpp -o "$output"
