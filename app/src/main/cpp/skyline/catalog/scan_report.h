// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog.h"
#include "scan_observations.h"
#include <array>
#include <span>
#include <string_view>

namespace skyline::catalog::observed {
inline constexpr size_t MaxReportBytes = 16 * 1024, MaxRows = 32, MaxIssueCodes = 8;
enum class Stage { ProfileValidation, KeyStoreLoad, FdDuplicate, CatalogScan, CatalogSerialize };
enum class PhaseStatus { Begun, Completed, Failed };
struct Phase { Stage stage; uint64_t startUs{}, durationUs{}; PhaseStatus status{PhaseStatus::Begun}; };
std::string_view StageName(Stage stage) noexcept;
std::string_view SafeCode(std::string_view code) noexcept;
std::span<const std::string_view> AllowedCodes() noexcept;

// Single synchronous scan only. Strings supplied by the scanner are never
// copied except for validated parsed public IDs and fixed allowlisted tokens.
class ScanReport {
  public:
    using Clock = uint64_t (*)() noexcept;
    explicit ScanReport(Clock clock = nullptr) noexcept;
    void Begin(Stage stage);
    void Complete(Stage stage);
    void Finish();
    void Fail(std::string_view code) noexcept;
    bool Completed() const noexcept;
    std::span<const Phase> Phases() const noexcept { return {phases.data(), phaseCount}; }
    observed::KeyState &Keys() noexcept { return keyState; }
    NativeKeyObserver &KeyObserver() noexcept { return keyLookups; }
    std::string ReportJson(const Catalog *catalog = nullptr) const;
    // Existing Catalog JSON is transient and comes only from catalog::ToJson.
    // It is deliberately separate from the bounded report and is null on error.
    std::string Envelope(const std::string &transientCatalogJson, const Catalog *catalog = nullptr) const;
  private:
    Clock clock;
    uint64_t started;
    std::array<Phase, 5> phases{};
    size_t phaseCount{};
    enum class State { Running, Completed, Failed } state{State::Running};
    std::optional<Stage> failedStage;
    std::string_view errorCode{"other_native_issue"};
    observed::KeyState keyState;
    KeyLookups keyLookups;
    uint64_t Elapsed() const noexcept;
};
}
