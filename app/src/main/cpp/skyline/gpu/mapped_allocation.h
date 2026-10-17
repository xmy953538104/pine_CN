// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <vk_mem_alloc.h>

namespace skyline::gpu::memory {
// Both APIs expose a persistent CPU mapping and support readback as well as
// uploads. AUTO usage requires an explicit host access declaration. RANDOM
// includes CPU reads; SEQUENTIAL_WRITE would misdescribe the shared APIs.
inline VmaAllocationCreateInfo MappedBufferAllocationInfo() {
    return {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    };
}
inline VmaAllocationCreateInfo MappedStagingAllocationInfo() {
    return {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    };
}
}
