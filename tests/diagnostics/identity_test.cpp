// SPDX-License-Identifier: MPL-2.0
#include "diagnostics/content.h"
#include "diagnostics/identity_observer.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>

int main(int argc, char **argv) {
    using namespace skyline::diagnostics;
    assert(argc == 2);
    constexpr auto app = "0100751007ADA000", patch = "0100751007ADA800", program = "0100751007ADA001";
    assert(ResolveApplicationTitle(app, program, patch, app, ApplicationIdentityKind::Patch) == app);
    assert(ResolveApplicationTitle("", program, app, "", ApplicationIdentityKind::Application) == app);
    assert(ResolveApplicationTitle(app, program, patch, "", ApplicationIdentityKind::Patch) == program);
    assert(ResolveApplicationTitle("", program, "", "", ApplicationIdentityKind::Unknown) == program);
    assert(ResolveApplicationTitle("", "", patch, "", ApplicationIdentityKind::Patch).empty());
    assert(ResolveApplicationTitle("0100000000001111", program, "", "", ApplicationIdentityKind::Unknown) == program);
    assert(ResolveApplicationTitle("0100000000001111", "", "", "", ApplicationIdentityKind::Unknown).empty());
    assert(NcaContentId("/private/ABCDEF0123456789abcdef0123456789.nca") == "abcdef0123456789abcdef0123456789");
    assert(NcaContentId("private-name.nca").empty());
    Config config{argv[1], "session-one", 1024, 65536, 2, false, {}};
    config.processRunId = "process-one";
    SetLaunchId("launch-one", true);
    int callbacks = 0;
    {
        ScopedContentIdentityObserver observer({[](void *count, std::string_view title, std::string_view source) noexcept {
            assert(title == "0100751007ADA000" && source == "cnmt_patch_parent");
            // This getter locks the sink; notification must occur outside its mutex.
            assert(ApplicationTitleId() == title);
            ++*static_cast<int *>(count);
        }, &callbacks});
        SetContentIdentity(app, program, patch, "abcdef0123456789abcdef0123456789", "cnmt_patch_parent");
    }
    assert(callbacks == 1);
    SetContentIdentity(app, program, patch, "abcdef0123456789abcdef0123456789", "cnmt_patch_parent");
    assert(callbacks == 1); // Scoped callback cannot outlive the JNI call.
    for (int session = 0; session < 2; ++session) {
        config.sessionId = session ? "session-two" : "session-one";
        assert(Start(config));
        for (;;) {
            const auto before = Stats().accepted;
            Emit("storage", "DLC_DATA_READ", {{"title_id", "0100751007ADB001"}});
            if (Stats().accepted > before) break;
        }
        Stop();
        std::ifstream file(std::filesystem::path(argv[1]) / "diagnostics-0.jsonl");
        const std::string text{std::istreambuf_iterator<char>(file), {}};
        assert(text.find("\"title_id\":\"0100751007ADA000\"") != std::string::npos);
        assert(text.find("\"application_title_id\":\"0100751007ADA000\"") != std::string::npos);
        assert(text.find("\"application_identity_source\":\"cnmt_patch_parent\"") != std::string::npos);
        assert(text.find("\"cnmt_title_id\":\"0100751007ADA800\"") != std::string::npos);
        assert(text.find("\"program_id\":\"0100751007ADA001\"") != std::string::npos);
        assert(text.find("\"launch_id\":\"launch-one\"") != std::string::npos);
        assert(text.find("\"process_run_id\":\"process-one\"") != std::string::npos);
        assert(text.find("\"last_guest_stage\":\"unknown\"") != std::string::npos);
    }
    SetLaunchId("launch-two", true);
    ClearTitleId();
    assert(ApplicationTitleId().empty());
}
