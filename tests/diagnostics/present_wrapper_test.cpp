// SPDX-License-Identifier: MPL-2.0
// Actual pinned Vulkan-Hpp Queue wrapper, controlled dispatch, no GPU or Android.
#include <vulkan/vulkan_raii.hpp>
#include "present.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
static VkResult result{};
static int calls{};
static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue, const VkPresentInfoKHR *) { ++calls; return result; }
static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice, const VkAllocationCallbacks *) {}
static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance, const VkAllocationCallbacks *) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProc(VkDevice, const char *name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
    if (!std::strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(Present);
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    return nullptr;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL InstanceProc(VkInstance, const char *name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(DeviceProc);
    if (!std::strcmp(name, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
    return nullptr;
}
int main(int argc, char **argv) {
    using namespace skyline::diagnostics;
    assert(argc == 2);
    vk::raii::Context context(InstanceProc);
    vk::raii::Instance instance(context, reinterpret_cast<VkInstance>(1));
    vk::raii::PhysicalDevice physical(instance, reinterpret_cast<VkPhysicalDevice>(2));
    vk::raii::Device device(physical, reinterpret_cast<VkDevice>(3));
    vk::raii::Queue queue(device, reinterpret_cast<VkQueue>(4));
    Config config{argv[1], "pinned-present-wrapper", 1024, 65536, 2, false, {}};
    SetLaunchId("controlled-present", true); assert(Start(config));
    for (auto next : {VK_SUCCESS, VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_DEVICE_LOST}) {
        result = next;
        bool caught{};
        try {
            const auto observed = ObservePresent<vk::SystemError>([&] { return queue.presentKHR(vk::PresentInfoKHR{}); });
            assert(static_cast<int32_t>(observed) == next);
        } catch (const vk::SystemError &error) {
            caught = true; assert(error.code().value() == next);
            if (next == VK_ERROR_OUT_OF_DATE_KHR) assert(dynamic_cast<const vk::OutOfDateKHRError *>(&error));
            else assert(dynamic_cast<const vk::DeviceLostError *>(&error));
        }
        assert(caught == (next < 0));
    }
    assert(calls == 4); EndLaunch("controlled_wrapper_test"); Stop();
    std::ifstream stream(std::filesystem::path(argv[1]) / "diagnostics-0.jsonl");
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    assert(text.find("\"present_returns\":\"4\"") != std::string::npos);
    assert(text.find("\"present_accepted\":\"2\"") != std::string::npos);
    assert(text.find("\"present_other_results\":\"2\"") != std::string::npos);
    assert(text.find("\"present_wrapper_exceptions\":\"2\"") != std::string::npos);
    assert(text.find("\"last_present_result\":\"-4\"") != std::string::npos);
    assert(text.find("vulkan_hpp_system_error_code") != std::string::npos);
}
