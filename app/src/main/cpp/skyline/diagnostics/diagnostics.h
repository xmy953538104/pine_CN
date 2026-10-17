// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace skyline::diagnostics {
struct Config {
    std::string directory;
    std::string sessionId;
    size_t queueCapacity{1024};
    size_t maxFileBytes{2 * 1024 * 1024};
    size_t maxFiles{4};
    bool verbose{};
    // Empty selects all. Comma-separated category tokens; lifecycle always included.
    std::string categories;
    std::string processRunId{};
};
struct Statistics {
    uint64_t accepted{}, dropped{}, written{}, diskErrors{};
    uint64_t droppedContention{}, droppedFull{}, droppedAllocation{}, droppedDisk{}, criticalDropped{}, snapshotErrors{}, snapshotEvictions{};
};
bool Enabled() noexcept;
bool Verbose() noexcept;
// Lifecycle calls are for ordinary threads only. Start replaces/drains an existing session.
bool Start(const Config &config) noexcept;
void Stop() noexcept;
// Ordinary threads only. Wait for the currently accepted snapshot without
// stopping the session; false on timeout, disk failure, or a busy lifecycle call.
bool Flush(uint32_t timeoutMilliseconds = 250) noexcept;
// Never call from a raw signal handler. Fields are copied, bounded and escaped.
void Emit(std::string_view category, std::string_view event,
          std::initializer_list<std::pair<std::string_view, std::string_view>> fields = {},
          std::string_view level = "INFO") noexcept;
// Ordinary threads only. Current-game identity is retained while diagnostics are
// disabled and across Start/Stop; subsequent records inherit it. Empty is unknown.
void SetTitleId(std::string_view titleId) noexcept;
void SetContentIdentity(std::string_view applicationId, std::string_view programId,
                        std::string_view cnmtId, std::string_view programContentId = {}) noexcept;
std::string ApplicationTitleId();
void SetLaunchId(std::string_view launchId, bool newLaunch = false) noexcept;
// True game boundaries only: clear at launch and after the final session drains.
void ClearTitleId() noexcept;
void Stage(std::string_view stage) noexcept;
std::string LastStage();
Statistics Stats() noexcept;
std::string Sanitize(std::string_view text, size_t limit = 2048);
}
