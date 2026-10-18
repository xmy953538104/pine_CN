// SPDX-License-Identifier: MPL-2.0
// Fresh process: force the first production ring's actual new[] to fail.
#include "diagnostics.h"
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <new>
static std::atomic<bool> failArray{};
void *operator new[](size_t bytes) {
    if (failArray.exchange(false)) throw std::bad_alloc();
    if (auto *result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete[](void *value, size_t) noexcept { std::free(value); }
int main(int argc, char **argv) {
    using namespace skyline::diagnostics;
    assert(argc == 2);
    Config config{argv[1], "first-allocation", 1024, 65536, 2, false, {}};
    failArray = true;
    assert(!Start(config));
    assert(!failArray && !Enabled() && !std::filesystem::exists(config.directory));
    Stop(); // Safe even though no ring has ever existed.
    assert(Start(config)); Emit("lifecycle", "SESSION_METADATA"); Stop();
    assert(Stats().accepted == 1 && Stats().written == 1 && Stats().diskErrors == 0);
}
