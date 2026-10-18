// SPDX-License-Identifier: MPL-2.0
// Production PatchManager + Backing, with controlled NCA construction and host DeviceState.
#include "vfs/patch_manager.h"
#include "diagnostics/content.h"
#include <cassert>
#include <map>
#include <vector>

namespace skyline::diagnostics {
bool enabled{};
struct Observation {
    std::string event, level;
    std::map<std::string, std::string> fields;
};
std::vector<Observation> observations;
std::string effectiveProgram, effectiveContent, effectiveSource;
void SetEffectiveExeFs(std::string_view program, std::string_view content, std::string_view source) noexcept {
    effectiveProgram = program; effectiveContent = content; effectiveSource = source;
}
bool Enabled() noexcept { return enabled; }
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields,
          std::string_view level) noexcept {
    assert(category == "content");
    Observation observation{std::string(event), std::string(level), {}};
    for (const auto &[key, value] : fields) observation.fields.emplace(key, value);
    observations.push_back(std::move(observation));
}
}
using namespace skyline;
struct TestBacking : vfs::Backing {
    TestBacking() : Backing({true, false, false}, 64) {}
    size_t ReadImpl(span<u8> output, size_t) override { return output.size(); }
};
int main() {
    using namespace diagnostics;
    DeviceState state{std::make_shared<PatchTestLoader>(), std::make_shared<PatchTestLoader>(), std::make_shared<PatchTestOS>()};
    state.os->keyStore = std::make_shared<crypto::KeyStore>();
    state.loader->programNca.emplace();
    state.loader->programNca->romFs = std::make_shared<TestBacking>();
    state.updateLoader->programNca.emplace();
    state.loader->diagnosticProgramContentId = "base-content";
    state.updateLoader->diagnosticProgramContentId = "update-content";
    auto &update = *state.updateLoader->programNca;
    update.header.titleId = 0x1234;
    update.romFs = std::make_shared<TestBacking>();
    update.exeFs = std::make_shared<vfs::FileSystem>();
    auto originalExeFs = std::make_shared<vfs::FileSystem>();
    vfs::PatchManager manager;
    assert(manager.PatchExeFS(state, originalExeFs) == update.exeFs);
    assert(effectiveProgram == "0000000000001234" && effectiveContent == "update-content" && effectiveSource == "update");
    assert(observations.empty());
    enabled = true;
    ContentScope scope("update");
    assert(manager.PatchExeFS(state, originalExeFs) == update.exeFs);
    assert(observations.back().event == "PATCH_EXEFS_SELECTED");
    assert(observations.back().fields.at("title_id") == "0000000000001234");
    assert(observations.back().fields.at("base_program_nca") == "base-content");
    assert(observations.back().fields.at("effective_exefs_program_nca") == "update-content");
    assert(manager.PatchRomFS(state, update, 123) == update.romFs);
    assert(vfs::NCA::receivedBase == state.loader->programNca->romFs.get());
    assert(vfs::NCA::receivedKeyStore == state.os->keyStore.get() && vfs::NCA::receivedOffset == 123);
    assert(observations.back().event == "PATCH_ROMFS_OPENED" && observations.back().fields.at("status") == "ok");
    assert(observations.back().fields.at("loader_source") == "update");

    update.romFs.reset();
    assert(!manager.PatchRomFS(state, update, 456));
    assert(observations.back().event == "PATCH_ROMFS_FAILED" && observations.back().level == "ERROR");
    assert(observations.back().fields.at("status") == "missing");
    update.exeFs.reset();
    assert(!manager.PatchExeFS(state, originalExeFs));
    assert(observations.back().event == "PATCH_EXEFS_MISSING");

    vfs::NCA::fail = true;
    try { manager.PatchRomFS(state, update, 789); assert(false); } catch (const vfs::ControlledPatchFailure &) {}
    assert(observations.back().event == "PATCH_ROMFS_FAILED" && observations.back().level == "ERROR");
    assert(observations.back().fields.at("status") == "exception");
    assert(observations.back().fields.at("reason") == "controlled patch error");
    assert(state.loader->programNca->romFs && !state.updateLoader->programNca->romFs);
    const auto observed = observations.size();
    enabled = false;
    try { manager.PatchRomFS(state, update, 789); assert(false); } catch (const vfs::ControlledPatchFailure &) {}
    assert(observations.size() == observed);
}
