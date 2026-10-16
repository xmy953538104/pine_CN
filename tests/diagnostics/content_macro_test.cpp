// SPDX-License-Identifier: MPL-2.0
#include "diagnostics/content.h"
#include <cassert>
#include <map>
#include <stdexcept>
#include <thread>

namespace skyline::diagnostics {
static bool enabled{};
static unsigned emitted{};
static std::string lastLevel, lastCategory;
static std::map<std::string, std::string> lastFields;
bool Enabled() noexcept { return enabled; }
void Emit(std::string_view category, std::string_view,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view level) noexcept {
    ++emitted;
    lastLevel = level;
    lastCategory = category;
    lastFields.clear();
    for (const auto &[key, value] : fields) lastFields.emplace(key, value);
}
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
    PINE_DIAG_STORAGE("DISABLED_READ", {"field", field()});
    assert(evaluated == 1 && emitted == 2);
    enabled = true;
    {
        ContentScope outer("base");
        PINE_DIAG_CONTENT("BASE");
        assert(lastFields.at("loader_source") == "base" && lastFields.at("loader_index") == "0");
        try {
            ContentScope inner("dlc", 42);
            PINE_DIAG_STORAGE_ERROR("DLC_ERROR", {"message", "controlled"});
            assert(lastCategory == "storage" && lastLevel == "ERROR");
            assert(lastFields.at("loader_source") == "dlc" && lastFields.at("loader_index") == "42");
            throw std::runtime_error("original loader failure");
        } catch (const std::runtime_error &) {}
        assert(contentContext.source == "base" && contentContext.index == "0");
        std::thread other([] {
            assert(contentContext.source.empty() && contentContext.index.empty());
            ContentScope scope("update", std::numeric_limits<size_t>::max());
            assert(contentContext.index == std::to_string(std::numeric_limits<size_t>::max()));
        });
        other.join();
        assert(contentContext.source == "base" && contentContext.index == "0");
    }
    assert(contentContext.source.empty() && contentContext.index.empty());
    assert(ContentTitleId(0x1234) == "0000000000001234");
    assert(ContentTitleId(UINT64_MAX) == "FFFFFFFFFFFFFFFF");
    assert(ContentBasename("/storage/emulated/0/private title/name.nca") == "name.nca");
    assert(ContentBasename("C:\\Users\\someone\\name.nca") == "name.nca");
    assert(ContentBasename("name.nca") == "name.nca");
}
