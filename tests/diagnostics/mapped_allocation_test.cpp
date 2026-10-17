// SPDX-License-Identifier: MPL-2.0
// Real pinned VMA API/Debug assertions with controlled Vulkan callbacks.
// This does not emulate a physical GPU or claim Android/game execution.
#ifdef NDEBUG
#error This regression requires active VMA Debug assertions.
#endif
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include "gpu/mapped_allocation.h"
#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

struct VkBuffer_T { VkDeviceSize size; };
struct VkDeviceMemory_T { std::vector<unsigned char> bytes; };
namespace {
bool hostVisible = true;
void VKAPI_PTR Properties(VkPhysicalDevice, VkPhysicalDeviceProperties *out) {
    *out = {};
    out->apiVersion = VK_API_VERSION_1_0;
    out->limits.bufferImageGranularity = 1;
    out->limits.nonCoherentAtomSize = 64;
    out->limits.maxMemoryAllocationCount = 4096;
}
void VKAPI_PTR MemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *out) {
    *out = {};
    out->memoryTypeCount = out->memoryHeapCount = 1;
    out->memoryHeaps[0] = {16 * 1024 * 1024, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    out->memoryTypes[0] = {VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        (hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)), 0};
}
VkResult VKAPI_PTR Allocate(VkDevice, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *, VkDeviceMemory *out) {
    *out = new VkDeviceMemory_T{std::vector<unsigned char>(info->allocationSize)};
    return VK_SUCCESS;
}
void VKAPI_PTR Free(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) { delete memory; }
VkResult VKAPI_PTR Map(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void **out) {
    assert(offset < memory->bytes.size());
    *out = memory->bytes.data() + offset;
    return VK_SUCCESS;
}
void VKAPI_PTR Unmap(VkDevice, VkDeviceMemory) {}
VkResult VKAPI_PTR Sync(VkDevice, uint32_t, const VkMappedMemoryRange *) { return VK_SUCCESS; }
VkResult VKAPI_PTR BindBuffer(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VkResult VKAPI_PTR BindImage(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
void VKAPI_PTR BufferRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements *out) { *out = {buffer->size, 64, 1}; }
void VKAPI_PTR ImageRequirements(VkDevice, VkImage, VkMemoryRequirements *out) { *out = {4096, 64, 1}; }
VkResult VKAPI_PTR CreateBuffer(VkDevice, const VkBufferCreateInfo *info, const VkAllocationCallbacks *, VkBuffer *out) {
    *out = new VkBuffer_T{info->size};
    return VK_SUCCESS;
}
void VKAPI_PTR DestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks *) { delete buffer; }
VkResult VKAPI_PTR CreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
void VKAPI_PTR DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *) {}
void VKAPI_PTR Copy(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy *) {}

VmaAllocator Allocator(bool visible = true) {
    hostVisible = visible;
    VmaVulkanFunctions functions{};
    functions.vkGetPhysicalDeviceProperties = Properties;
    functions.vkGetPhysicalDeviceMemoryProperties = MemoryProperties;
    functions.vkAllocateMemory = Allocate; functions.vkFreeMemory = Free;
    functions.vkMapMemory = Map; functions.vkUnmapMemory = Unmap;
    functions.vkFlushMappedMemoryRanges = Sync; functions.vkInvalidateMappedMemoryRanges = Sync;
    functions.vkBindBufferMemory = BindBuffer; functions.vkBindImageMemory = BindImage;
    functions.vkGetBufferMemoryRequirements = BufferRequirements; functions.vkGetImageMemoryRequirements = ImageRequirements;
    functions.vkCreateBuffer = CreateBuffer; functions.vkDestroyBuffer = DestroyBuffer;
    functions.vkCreateImage = CreateImage; functions.vkDestroyImage = DestroyImage;
    functions.vkCmdCopyBuffer = Copy;
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = reinterpret_cast<VkPhysicalDevice>(1);
    info.device = reinterpret_cast<VkDevice>(2);
    info.instance = reinterpret_cast<VkInstance>(3);
    info.vulkanApiVersion = VK_API_VERSION_1_0;
    info.pVulkanFunctions = &functions;
    VmaAllocator allocator{};
    assert(vmaCreateAllocator(&info, &allocator) == VK_SUCCESS);
    return allocator;
}
VkBufferCreateInfo BufferInfo() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = 4096;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return info;
}
void LegacyAbort(const char *executable, const char *mode) {
    int descriptors[2]; assert(pipe(descriptors) == 0);
    const auto pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(descriptors[0]);
        assert(dup2(descriptors[1], STDERR_FILENO) >= 0);
        close(descriptors[1]);
        execl(executable, executable, mode, nullptr);
        _exit(99);
    }
    close(descriptors[1]);
    std::string error;
    char block[1024];
    for (ssize_t count; (count = read(descriptors[0], block, sizeof(block))) > 0;) error.append(block, count);
    close(descriptors[0]);
    int status{}; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    assert(error.find("When using VMA_ALLOCATION_CREATE_MAPPED_BIT") != std::string::npos);
}
}
int main(int argc, char **argv) {
    namespace memory = skyline::gpu::memory;
    rlimit noCore{0, 0}; assert(setrlimit(RLIMIT_CORE, &noCore) == 0);
    if (argc == 2) {
        auto allocation = std::string(argv[1]) == "legacy-staging" ? memory::MappedStagingAllocationInfo() : memory::MappedBufferAllocationInfo();
        allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT; // Exact original missing host-access declaration.
        auto allocator = Allocator(); auto bufferInfo = BufferInfo();
        VkBuffer buffer{}; VmaAllocation backing{};
        vmaCreateBuffer(allocator, &bufferInfo, &allocation, &buffer, &backing, nullptr);
        return 98; // Debug VMA must abort before this point.
    }
    LegacyAbort(argv[0], "legacy-general"); LegacyAbort(argv[0], "legacy-staging");
    for (const auto allocation : {memory::MappedBufferAllocationInfo(), memory::MappedStagingAllocationInfo()}) {
        auto allocator = Allocator(); auto info = BufferInfo();
        VkBuffer buffer{}; VmaAllocation backing{}; VmaAllocationInfo mapped{};
        assert(vmaCreateBuffer(allocator, &info, &allocation, &buffer, &backing, &mapped) == VK_SUCCESS);
        assert(mapped.pMappedData);
        auto bytes = static_cast<unsigned char *>(mapped.pMappedData);
        bytes[3] = 17; bytes[2048] = 42; // Both shared APIs support CPU writes AND reads.
        assert(bytes[3] == 17 && bytes[2048] == 42);
        assert(!(allocation.flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT));
        vmaDestroyBuffer(allocator, buffer, backing); vmaDestroyAllocator(allocator);
    }
    auto allocator = Allocator(false); auto info = BufferInfo();
    auto allocation = memory::MappedBufferAllocationInfo(); VkBuffer buffer{}; VmaAllocation backing{};
    assert(vmaCreateBuffer(allocator, &info, &allocation, &buffer, &backing, nullptr) == VK_ERROR_FEATURE_NOT_PRESENT);
    vmaDestroyAllocator(allocator);
    std::cout << "PASS: real pinned Debug VMA reproduces both original SIGABRT contracts; fixed production configs map/read/write and reject unavailable host memory\n";
}
