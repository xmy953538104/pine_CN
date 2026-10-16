// SPDX-License-Identifier: MPL-2.0
#include "diagnostics.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace skyline::diagnostics;

static void ReadBytes(int descriptor, char *output, size_t count) {
    while (count) {
        const auto result = read(descriptor, output, count);
        if (result < 0 && errno == EINTR) continue;
        assert(result > 0);
        output += result; count -= static_cast<size_t>(result);
    }
}

static int Child(const char *root, const std::string &scope, int start, int ready) {
    assert(scope == "main" || scope == "emulation");
    Config config{(std::filesystem::path(root) / scope).string(), "process-" + scope, 256, 65536, 3, true, {}};
    assert(Start(config));
    assert(write(ready, "r", 1) == 1);
    close(ready);
    char token{};
    ReadBytes(start, &token, 1);
    close(start);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const std::string payload(1024, scope == "main" ? 'm' : 'e');
    const auto event = scope == "main" ? "PROCESS_MAIN" : "PROCESS_EMULATION";
    while (Stats().accepted < 600) {
        assert(Enabled() && std::chrono::steady_clock::now() < deadline);
        Emit("test", event, {{"scope", scope}, {"payload", payload}});
        std::this_thread::yield();
    }
    assert(Flush(10000));
    Stop();
    assert(Stats().diskErrors == 0 && Stats().written == Stats().accepted);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 6 && std::string_view(argv[1]) == "--child")
        return Child(argv[2], argv[3], std::stoi(argv[4]), std::stoi(argv[5]));
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    { std::ofstream sentinel(root / "unrelated.txt"); sentinel << "preserve"; }
    int ready[2]{}, start[2]{};
    assert(pipe(ready) == 0 && pipe(start) == 0);
    const auto program = std::filesystem::absolute(argv[0]).string();
    const auto startDescriptor = std::to_string(start[0]), readyDescriptor = std::to_string(ready[1]);
    pid_t children[2]{};
    for (int i = 0; i < 2; ++i) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (!children[i]) {
            close(ready[0]); close(start[1]);
            // Every child execs a fresh sink before starting any worker threads.
            execl(program.c_str(), program.c_str(), "--child", argv[1], i ? "emulation" : "main",
                  startDescriptor.c_str(), readyDescriptor.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
    }
    close(ready[1]); close(start[0]);
    char tokens[2]{};
    ReadBytes(ready[0], tokens, sizeof(tokens)); // Both independent sinks are active.
    assert(write(start[1], "go", 2) == 2);
    close(ready[0]); close(start[1]);
    for (auto child : children) {
        int status{};
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    for (const auto &scope : {"main", "emulation"}) {
        const auto own = "process-" + std::string(scope);
        const auto other = std::string(scope) == "main" ? "process-emulation" : "process-main";
        for (int i = 0; i < 3; ++i) {
            const auto path = root / scope / ("diagnostics-" + std::to_string(i) + ".jsonl");
            assert(std::filesystem::file_size(path) <= 65536);
            std::ifstream file(path);
            const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            assert(text.find(own) != std::string::npos && text.find(other) == std::string::npos);
            if (!i) assert(text.find("SESSION_SUMMARY") != std::string::npos);
        }
        assert(!std::filesystem::exists(root / scope / "diagnostics-3.jsonl"));
    }
    assert(std::filesystem::file_size(root / "unrelated.txt") == 8);
    std::cout << "PASS: two concurrent exec'd process sinks, independent main/emulation rotation and retention\n";
}
