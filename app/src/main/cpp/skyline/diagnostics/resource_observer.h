// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace skyline::diagnostics {
// Values describe observed API boundaries, never inferred NVN capacity or GPU time.
enum class ResourceMetric : size_t {
    GuestMainSvcEntered, GuestMainSvcReturned, GuestOtherSvcEntered, GuestOtherSvcReturned,
    GuestSvcExceptions, GuestLastSvc, GuestLastThread,
    GuestHeapRequestedBytes, GuestHeapLogicalBytes, GuestHeapErrors,
    GuestVirtualHeapBytes, GuestReportedAvailableBytes, GuestReportedUsedBytes,
    GuestSystemResourceLimitBytes, GuestSystemResourceUsedBytes,
    DriverFifoCapacityEntries, DriverFifoAllocations, DriverFifoSubmittedEntries,
    DriverFifoSubmissions, DriverFifoProcessedWords,
    HostSubmitAccepted, HostSubmitErrors, HostFenceWaitEntered, HostFenceWaits, HostFenceWaitUs, HostFenceErrors,
    HostCommandBufferAllocations, HostVmaAllocationBytes, HostVmaAllocations,
    Count
};
// KProcess::CreateThread assigns threads.size()+1: HOS-1 is the main thread.
inline constexpr ResourceMetric SvcEnteredMetric(uint64_t id) noexcept {
    return id == 1 ? ResourceMetric::GuestMainSvcEntered : ResourceMetric::GuestOtherSvcEntered;
}
inline constexpr ResourceMetric SvcReturnedMetric(uint64_t id) noexcept {
    return id == 1 ? ResourceMetric::GuestMainSvcReturned : ResourceMetric::GuestOtherSvcReturned;
}
// Disabled calls only check gates. Enabled calls use fixed atomics; no sink admission,
// formatting, heap allocation, thread enumeration, JNI or file IO on producer paths.
bool ResourceObservationEnabled() noexcept;
void ResourceAdd(ResourceMetric metric, uint64_t value = 1) noexcept;
void ResourceSet(ResourceMetric metric, uint64_t value) noexcept;
// Ordinary configuration thread; separate from and subordinate to Diagnostics enable.
void ConfigureResourceObservation(bool enabled, const char *cohort) noexcept;
void ResourcePresentationPaused(bool paused) noexcept;
// ExitProcess requests normal termination; ExitThread/SIGINT are not crashes.
void ResourceProcessExited() noexcept;
bool EmitHostResourceFields(std::string_view expectedSession, std::string_view expectedLaunch,
    std::span<const std::pair<std::string_view, std::string_view>> fields) noexcept;
}
