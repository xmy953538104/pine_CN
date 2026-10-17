// Controlled host faults, not APK/ARM64/game acceptance. No private input files.
#include "initialization.h"
#include <cassert>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace diag = skyline::diagnostics;
namespace {
std::string Read(const std::filesystem::path &path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
template<typename F> void Accepted(F operation) {
    const auto before = diag::Stats().accepted;
    for (size_t i{}; i < 10000; ++i) {
        operation();
        if (diag::Stats().accepted > before) return;
    }
    throw std::runtime_error("controlled event could not be accepted");
}
struct NonMovable {
    int value;
    explicit NonMovable(int value) : value(value) {}
    NonMovable(const NonMovable &) = delete;
    NonMovable(NonMovable &&) = delete;
};
struct OriginalFailure {};
}
int main(int argc, char **argv) {
    assert(argc == 2 || argc == 3);
    const std::filesystem::path root{argv[1]};
    if (argc == 3) {
        rlimit noCore{0, 0};
        assert(setrlimit(RLIMIT_CORE, &noCore) == 0);
        assert(diag::Start({root.string(), "synthetic-abrupt", 1024, 65536, 2, false, {}}));
        Accepted([] { diag::Stage("JNI_ENTERED"); });
        assert(diag::Flush(2000));
        Accepted([] { diag::Emit("lifecycle", "STAGE_STARTED", {{"stage", "SYNTHETIC_GPU_MEMBER"}}); });
        assert(diag::Flush(2000));
        std::raise(SIGABRT); // Real host process death: no Stop(), guard unwind or fake summary.
        return 99;
    }
    int calls{};
    auto disabled = diag::ObserveInitialization("DISABLED", [&] { ++calls; return NonMovable(7); });
    assert(disabled.value == 7 && calls == 1 && !std::filesystem::exists(root));
    assert(diag::Start({root.string(), "synthetic-normal", 1024, 65536, 2, false, {}}));
    Accepted([&] {
        auto value = diag::ObserveInitialization("SYNTHETIC_CONSTRUCT", [&] { ++calls; return NonMovable(11); });
        assert(value.value == 11);
    });
    assert(diag::Flush(2000));
    OriginalFailure *original{};
    try {
        diag::ObserveInitialization("SYNTHETIC_THROW", [&] {
            OriginalFailure failure;
            original = &failure;
            throw original;
        });
        assert(false);
    } catch (OriginalFailure *caught) {
        assert(caught == original); // Preserve exact exception type/object, not a diagnostic replacement.
    }
    diag::Stop();
    const auto normal = Read(root / "diagnostics-0.jsonl");
    assert(normal.find("STAGE_STARTED") != std::string::npos);
    assert(normal.find("STAGE_COMPLETED") != std::string::npos);
    assert(normal.find("\"level\":\"ERROR\"") == std::string::npos); // Unclassified unwind is not a crash.
    const auto abrupt = root / "abrupt";
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], abrupt.c_str(), "fault", nullptr);
        _exit(98);
    }
    int status{};
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    const auto evidence = Read(abrupt / "diagnostics-0.jsonl");
    assert(evidence.find("SYNTHETIC_GPU_MEMBER") != std::string::npos);
    assert(evidence.find("JNI_ENTERED") != std::string::npos);
    assert(evidence.find("SESSION_SUMMARY") == std::string::npos);
    assert(evidence.find("EMULATION_ENDED") == std::string::npos);
    std::cout << "PASS: original construction/exception semantics, disabled gate, real host SIGABRT retains flushed startup evidence without inventing normal end\n";
}
