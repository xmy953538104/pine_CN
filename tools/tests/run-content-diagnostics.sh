#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
compiler=${CXX:-c++}
"$compiler" -std=c++20 -Itests/diagnostics/backing_stubs -Iapp/src/main/cpp/skyline tests/diagnostics/backing_test.cpp -o "$build_dir/backing-test"
"$build_dir/backing-test"
"$compiler" -std=c++20 -Wall -Wextra -Werror -pthread -Iapp/src/main/cpp/skyline tests/diagnostics/content_macro_test.cpp -o "$build_dir/content-macro-test"
"$build_dir/content-macro-test"
"$compiler" -std=c++20 -Wall -Wextra -Werror -Iapp/src/main/cpp/skyline tests/diagnostics/nsp_content_test.cpp -o "$build_dir/nsp-content-test"
"$build_dir/nsp-content-test"
"$compiler" -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -pthread -Itests/diagnostics/storage_stubs -Iapp/src/main/cpp/skyline \
    app/src/main/cpp/skyline/services/fssrv/IStorage.cpp tests/diagnostics/storage_test.cpp -o "$build_dir/storage-test"
"$build_dir/storage-test"
"$compiler" -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -Wno-reorder -pthread \
    -Itests/diagnostics/composite_stubs -Itests/diagnostics/storage_stubs -Iapp/src/main/cpp/skyline \
    app/src/main/cpp/skyline/vfs/bktr.cpp app/src/main/cpp/skyline/vfs/ctr_encrypted_backing.cpp \
    app/src/main/cpp/skyline/vfs/os_backing.cpp app/src/main/cpp/skyline/services/fssrv/IStorage.cpp \
    tests/diagnostics/composite_backing_test.cpp -o "$build_dir/composite-backing-test"
"$build_dir/composite-backing-test"
"$compiler" -std=c++20 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -pthread -Itests/diagnostics/patch_stubs -Iapp/src/main/cpp/skyline \
    app/src/main/cpp/skyline/vfs/patch_manager.cpp tests/diagnostics/patch_test.cpp -o "$build_dir/patch-test"
"$build_dir/patch-test"
echo 'Content diagnostics tests passed (NSP selection/catches, production storage/backings/patching, reliable or unknown byte evidence and sampling).'
