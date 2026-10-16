// Host transport fixture generator. These are controlled API inputs, not device
// or game observations; every output byte comes from the production JSONL sink.
#include "diagnostics.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace diag = skyline::diagnostics;
namespace {
constexpr auto BaseTitle = "0100751007ADA000";
constexpr auto DlcTitle = "0100751007ADB001";
constexpr auto OtherTitle = "0200751007ADA000";

template<typename F> void Record(F emit) {
    const auto before = diag::Stats().accepted;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        emit();
        if (diag::Stats().accepted == before + 1) {
            if (!diag::Flush(2000)) throw std::runtime_error("fixture flush failed");
            return;
        }
        // A try-lock drop is valid sink behavior, including for a lone producer.
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("fixture record was not accepted");
}

template<typename F> void Session(const std::filesystem::path &root, size_t index, const std::string &id, F emit) {
    const auto directory = root / std::to_string(index);
    diag::Config config{directory.string(), id, 1024, 65536, 1, false, {}};
    if (!diag::Start(config)) throw std::runtime_error("fixture session did not start");
    emit();
    diag::Stop();
    const auto stats = diag::Stats();
    if (stats.diskErrors || stats.written != stats.accepted) throw std::runtime_error("fixture output failed");
    std::ifstream file(directory / "diagnostics-0.jsonl", std::ios::binary);
    if (!file) throw std::runtime_error("fixture output is missing");
    std::cout << file.rdbuf();
    if (!std::cout) throw std::runtime_error("fixture copy failed");
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: adb_fixture TEMP_DIRECTORY capture|sessions");
        const std::filesystem::path root(argv[1]);
        if (std::filesystem::exists(root)) throw std::runtime_error("fixture directory must not already exist");
        const std::string scenario(argv[2]);
        if (scenario == "capture") {
            Session(root, 0, "native-adb-a", [] {
                Record([] { diag::Emit("lifecycle", "BEFORE_TITLE", {{"message", "base is not known yet"}}); });
                Record([] { diag::Emit("content", "CONTENT_ID_WITHOUT_CONTEXT", {{"title_id", BaseTitle}, {"message", "content identity before launch context"}}); });
                diag::SetTitleId(BaseTitle);
                Record([] { diag::Emit("content", "BASE_SELECTED", {{"title_id", BaseTitle}, {"message", "\xe4\xb8\xad\xe6\x96\x87 \xe6\x97\xa5\xe5\xbf\x97"}}); });
                Record([] { diag::Emit("storage", "DLC_DATA_READ", {{"title_id", DlcTitle}, {"parent_title_id", BaseTitle}, {"actual_bytes", "8"}}); });
                Record([] { diag::Emit("guest", "GUEST_DEBUG_MESSAGE", {{"source", "OutputDebugString"}, {"message", "controlled host fixture"}}); });
                Record([] { diag::Stage("content_ready"); });
            });
            Session(root, 1, "native-adb-b", [] {
                diag::SetTitleId(OtherTitle);
                Record([] { diag::Emit("content", "BASE_SELECTED", {{"title_id", OtherTitle}, {"message", "other title"}}); });
                Record([] { diag::Emit("guest", "GUEST_DEBUG_MESSAGE", {{"message", "other session"}}); });
            });
            Session(root, 2, "native-adb-unknown", [] {
                Record([] { diag::Emit("content", "CONTENT_ID_WITHOUT_CONTEXT", {{"title_id", DlcTitle}, {"message", "content identity without a base"}}); });
            });
        } else if (scenario == "sessions") {
            for (size_t i{}; i < 40; ++i) {
                const auto id = i == 0 ? "collision/a" : i == 1 ? "collision_a" : i == 2 ? "Case" : i == 3 ? "case"
                    : i == 4 ? std::string(90, 'x') + "a" : i == 5 ? std::string(90, 'x') + "b" : i == 6 ? "overflow" : "session-" + std::to_string(i);
                Session(root, i, id, [i] {
                    diag::SetTitleId(BaseTitle);
                    Record([] { diag::Emit("content", "SESSION_GROUPING_FIXTURE", {{"message", "controlled host fixture"}}); });
                    if (!i) Record([] { diag::Emit("content", "SESSION_GROUPING_FIXTURE", {{"message", "same session"}}); });
                });
            }
        } else {
            throw std::runtime_error("unknown fixture scenario");
        }
        return 0;
    } catch (const std::exception &error) {
        diag::Stop();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
