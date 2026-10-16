// SPDX-License-Identifier: MPL-2.0
#include "diagnostics/content.h"
#include <cassert>
#include <stdexcept>

namespace skyline::diagnostics {
static bool enabled{};
static unsigned emitted{};
static std::string_view lastLevel;
bool Enabled() noexcept { return enabled; }
void Emit(std::string_view, std::string_view,
          std::initializer_list<std::pair<std::string_view, std::string_view>>,
          std::string_view level) noexcept { ++emitted; lastLevel = level; }
}
int main() {
    using namespace skyline::diagnostics;
    unsigned evaluated{};
    auto field = [&]() -> std::string { ++evaluated; return "value"; };
    PINE_DIAG_CONTENT("DISABLED", {"field", field()});
    assert(evaluated == 0 && emitted == 0);
    enabled = true;
    PINE_DIAG_CONTENT("ENABLED", {"field", field()});
    assert(evaluated == 1 && emitted == 1 && lastLevel == "INFO");
    auto failure = []() -> std::string { throw std::bad_alloc(); };
    PINE_DIAG_CONTENT("ALLOCATION_FAILURE", {"field", failure()});
    assert(emitted == 1); // Diagnostic formatting failure never escapes to guest.
    PINE_DIAG_CONTENT_ERROR("LOADER_FAILED", {"reason", "controlled"});
    assert(emitted == 2 && lastLevel == "ERROR");
    enabled = false;
    PINE_DIAG_CONTENT_ERROR("DISABLED_ERROR", {"field", field()});
    assert(evaluated == 1 && emitted == 2);
}
