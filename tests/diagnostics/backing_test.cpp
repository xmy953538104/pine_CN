// SPDX-License-Identifier: MPL-2.0
// Compiles the real Backing header with only platform dependencies stubbed.
#include "vfs/backing.h"
#include <cassert>
#include <array>
using namespace skyline;
struct ControlledBacking : vfs::Backing {
    size_t result{4};
    bool fail{};
    int calls{};
    ControlledBacking() : Backing({true, false, false}, 64) {}
    size_t ReadImpl(span<u8>, size_t) override {
        ++calls;
        if (fail) throw exception("controlled IO error");
        return result;
    }
};
int main() {
    ControlledBacking backing;
    std::array<u8, 4> buffer{};
    size_t actual = 999;
    assert(backing.Read(buffer, 0, &actual) == 64); // Historic return is NOT byte count.
    assert(actual == 4 && backing.calls == 1);
    backing.result = 2;
    assert(backing.Read(buffer, 0, &actual) == 64);
    assert(actual == 2 && backingWarnings == 1); // No change to warning/return semantics.
    backing.fail = true;
    actual = 999;
    try { backing.Read(buffer, 0, &actual); assert(false); } catch (const exception &) {}
    assert(actual == 999); // Failed read cannot become a successful byte count.
    const int calls = backing.calls;
    try { backing.Read(buffer, 65, &actual); assert(false); } catch (const exception &) {}
    assert(backing.calls == calls && actual == 999);
    backing.fail = false;
    backing.result = 4;
    assert(backing.Read(buffer) == 64); // Existing non-diagnostic call unchanged.
}
