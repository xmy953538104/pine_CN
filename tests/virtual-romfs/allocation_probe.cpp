// SPDX-License-Identifier: MPL-2.0
// Keep the replaceable allocation functions in a separate translation unit.
// GCC 13 otherwise inlines free into allocator callers and reports a spurious
// mismatched-new-delete warning, despite both replacements using malloc/free.
#include <atomic>
#include <cstdlib>
#include <new>

std::atomic<size_t> allocationCount{};

void *operator new(size_t size) {
    ++allocationCount;
    if (auto *pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, size_t) noexcept { std::free(pointer); }
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer, size_t) noexcept { std::free(pointer); }
