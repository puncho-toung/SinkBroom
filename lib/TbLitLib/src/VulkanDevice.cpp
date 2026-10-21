/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "VulkanDevice.h"

#include "lit/DeviceChoice.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

// Mirrored from the engine at 285ea4d1, neo/sys/DeviceManager_VK.cpp; every item is
// listed in lib/TbLitLib/mirror.json, together with what is deliberately not copied.

namespace tb::lit
{
namespace
{

// enabledExtensions.device (:232-235); the swapchain only when presenting
constexpr auto kSwapchainExtension = std::string_view{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
constexpr auto kRequiredExtensions = std::array<std::string_view, 1>{
  VK_KHR_MAINTENANCE1_EXTENSION_NAME,
};

// optionalExtensions.device (:256-274), kept where NVRHI or the plan uses them:
// synchronization2 switches NVRHI's barriers to vkCmdPipelineBarrier2 as in the game
// (nvrhi vulkan-state-tracking.cpp:345), memory_budget is the VRAM budget's source (S9)
constexpr auto kSynchronization2Extension =
  std::string_view{VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
constexpr auto kOptionalExtensions = std::array<std::string_view, 2>{
  kSynchronization2Extension,
  VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
};

struct CoreFeature
{
  std::string_view name;
  vk::Bool32 vk::PhysicalDeviceFeatures::* member;
};

// features createDevice sets to true (:1002-1011); the engine checks the first two in
// pickPhysicalDevice (:688-699), the others fail vkCreateDevice when missing
constexpr auto kRequiredCoreFeatures = std::array<CoreFeature, 5>{{
  {"samplerAnisotropy", &vk::PhysicalDeviceFeatures::samplerAnisotropy},
  {"textureCompressionBC", &vk::PhysicalDeviceFeatures::textureCompressionBC},
  {"shaderImageGatherExtended", &vk::PhysicalDeviceFeatures::shaderImageGatherExtended},
  {"imageCubeArray", &vk::PhysicalDeviceFeatures::imageCubeArray},
  {"dualSrcBlend", &vk::PhysicalDeviceFeatures::dualSrcBlend},
}};

struct Vulkan12Feature
{
  std::string_view name;
  vk::Bool32 vk::PhysicalDeviceVulkan12Features::* member;
};

// VkPhysicalDeviceVulkan12Features set to true (:1013-1019); timelineSemaphore is what
// NVRHI's queues track submissions with
constexpr auto kRequiredVulkan12Features = std::array<Vulkan12Feature, 6>{{
  {"descriptorIndexing", &vk::PhysicalDeviceVulkan12Features::descriptorIndexing},
  {"runtimeDescriptorArray", &vk::PhysicalDeviceVulkan12Features::runtimeDescriptorArray},
  {"descriptorBindingPartiallyBound",
   &vk::PhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound},
  {"descriptorBindingVariableDescriptorCount",
   &vk::PhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount},
  {"timelineSemaphore", &vk::PhysicalDeviceVulkan12Features::timelineSemaphore},
  {"shaderSampledImageArrayNonUniformIndexing",
   &vk::PhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing},
}};

std::string versionString(const std::uint32_t version)
{
  return fmt::format(
    "{}.{}.{}",
    VK_API_VERSION_MAJOR(version),
    VK_API_VERSION_MINOR(version),
    VK_API_VERSION_PATCH(version));
}

bool contains(
  const std::vector<vk::ExtensionProperties>& available, std::string_view name)
{
  return std::ranges::any_of(available, [&](const auto& extension) {
    return std::string_view{extension.extensionName.data()} == name;
  });
}

struct Inspection
{
  DeviceCandidate candidate;
  GpuChoice choice;
};

Inspection inspect(const vk::PhysicalDevice device, const vk::SurfaceKHR surface)
{
  auto result = Inspection{};
  auto& problems = result.candidate.problems;
  auto& choice = result.choice;

  const auto properties = device.getProperties();
  result.candidate.name = properties.deviceName.data();
  result.candidate.discrete =
    properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
  choice.physicalDevice = device;
  choice.name = result.candidate.name;

  const auto hasVulkan12 = properties.apiVersion >= VK_API_VERSION_1_2;
  if (!hasVulkan12)
  {
    problems.push_back(fmt::format(
      "runs Vulkan {}, the preview needs 1.2", versionString(properties.apiVersion)));
  }

  // extensions (:668-686)
  const auto available = device.enumerateDeviceExtensionProperties();
  const auto require = [&](const std::string_view name) {
    if (contains(available, name))
    {
      choice.deviceExtensions.emplace_back(name);
    }
    else
    {
      problems.push_back(fmt::format("missing {}", name));
    }
  };
  for (const auto name : kRequiredExtensions)
  {
    require(name);
  }
  if (surface)
  {
    require(kSwapchainExtension);
  }
  for (const auto name : kOptionalExtensions)
  {
    if (contains(available, name))
    {
      choice.deviceExtensions.emplace_back(name);
      choice.synchronization2 =
        choice.synchronization2 || name == kSynchronization2Extension;
    }
  }

  // features (:688-699, :969-1019)
  auto vulkan12Features = vk::PhysicalDeviceVulkan12Features{};
  auto synchronization2Features = vk::PhysicalDeviceSynchronization2FeaturesKHR{};
  auto features2 = vk::PhysicalDeviceFeatures2{};
  if (hasVulkan12)
  {
    features2.pNext = &vulkan12Features;
    if (choice.synchronization2)
    {
      vulkan12Features.pNext = &synchronization2Features;
    }
    device.getFeatures2(&features2);
  }
  else
  {
    features2.features = device.getFeatures();
  }
  choice.supportedFeatures = features2.features;

  for (const auto& feature : kRequiredCoreFeatures)
  {
    if (!(features2.features.*feature.member))
    {
      problems.push_back(fmt::format("does not support {}", feature.name));
    }
  }
  if (hasVulkan12)
  {
    for (const auto& feature : kRequiredVulkan12Features)
    {
      if (!(vulkan12Features.*feature.member))
      {
        problems.push_back(fmt::format("does not support {}", feature.name));
      }
    }
    // an extension without its feature is not used (NVRHI would then record
    // vkCmdPipelineBarrier2 without the feature enabled)
    if (choice.synchronization2 && !synchronization2Features.synchronization2)
    {
      choice.synchronization2 = false;
      std::erase(choice.deviceExtensions, std::string{kSynchronization2Extension});
    }
  }

  // swapchain format and FIFO (:701-747)
  if (surface)
  {
    const auto formats = device.getSurfaceFormatsKHR(surface);
    if (std::ranges::none_of(
          formats, [](const auto& format) { return format.format == kSwapchainFormat; }))
    {
      problems.push_back("cannot present B8G8R8A8_UNORM images to this window");
    }
    const auto presentModes = device.getSurfacePresentModesKHR(surface);
    if (std::ranges::find(presentModes, vk::PresentModeKHR::eFifo) == presentModes.end())
    {
      problems.push_back("does not offer the FIFO present mode");
    }
  }

  // one queue family that does graphics and present (:749-762). The engine takes the
  // first graphics family and the first present family separately; the preview needs
  // them to be one family, so that the swapchain images stay exclusive to it.
  const auto families = device.getQueueFamilyProperties();
  auto family = std::optional<std::uint32_t>{};
  for (std::uint32_t i = 0; i < std::uint32_t(families.size()) && !family; ++i)
  {
    if (
      families[i].queueCount == 0
      || !(families[i].queueFlags & vk::QueueFlagBits::eGraphics))
    {
      continue;
    }
    auto presents = vk::Bool32{VK_TRUE};
    if (
      surface
      && device.getSurfaceSupportKHR(i, surface, &presents) != vk::Result::eSuccess)
    {
      presents = VK_FALSE;
    }
    if (presents)
    {
      family = i;
    }
  }
  if (family)
  {
    choice.queueFamily = *family;
    // :1077-1080
    choice.timerQueries = properties.limits.timestampPeriod > 0.0f
                          && families[*family].timestampValidBits > 0;
  }
  else
  {
    problems.push_back(
      surface ? "has no queue family that does both graphics and present to this window"
              : "has no graphics queue family");
  }

  return result;
}

} // namespace

Result<GpuChoice> pickPhysicalDevice(
  const vk::Instance instance, const vk::SurfaceKHR surface)
{
  try
  {
    auto inspections = std::vector<Inspection>{};
    for (const auto device : instance.enumeratePhysicalDevices())
    {
      inspections.push_back(inspect(device, surface));
    }

    auto candidates = std::vector<DeviceCandidate>{};
    for (const auto& inspection : inspections)
    {
      candidates.push_back(inspection.candidate);
    }

    if (const auto index = preferredDevice(candidates))
    {
      return std::move(inspections[*index].choice);
    }
    return Error{describeUnusableDevices(candidates)};
  }
  catch (const vk::SystemError& e)
  {
    return Error{fmt::format("Cannot list the Vulkan devices: {}", e.what())};
  }
  catch (const std::exception& e)
  {
    return Error{fmt::format("Cannot list the Vulkan devices: {}", e.what())};
  }
}

Result<vk::Device> createVkDevice(const GpuChoice& gpu)
{
  try
  {
    const auto priority = 1.0f;
    const auto queueInfo = vk::DeviceQueueCreateInfo{}
                             .setQueueFamilyIndex(gpu.queueFamily)
                             .setQueueCount(1)
                             .setPQueuePriorities(&priority);

    // :1002-1011: required features on, the others as supported
    const auto& supported = gpu.supportedFeatures;
    const auto features = vk::PhysicalDeviceFeatures{}
                            .setShaderImageGatherExtended(VK_TRUE)
                            .setShaderStorageImageReadWithoutFormat(
                              supported.shaderStorageImageReadWithoutFormat)
                            .setSamplerAnisotropy(VK_TRUE)
                            .setTessellationShader(supported.tessellationShader)
                            .setTextureCompressionBC(VK_TRUE)
                            .setGeometryShader(supported.geometryShader)
                            .setFillModeNonSolid(supported.fillModeNonSolid)
                            .setImageCubeArray(VK_TRUE)
                            .setDualSrcBlend(VK_TRUE);

    // :979-980, :999
    auto synchronization2Features =
      vk::PhysicalDeviceSynchronization2FeaturesKHR{}.setSynchronization2(VK_TRUE);

    // :1013-1024, without bufferDeviceAddress and Optick's hostQueryReset
    auto vulkan12Features =
      vk::PhysicalDeviceVulkan12Features{}
        .setDescriptorIndexing(VK_TRUE)
        .setRuntimeDescriptorArray(VK_TRUE)
        .setDescriptorBindingPartiallyBound(VK_TRUE)
        .setDescriptorBindingVariableDescriptorCount(VK_TRUE)
        .setTimelineSemaphore(VK_TRUE)
        .setShaderSampledImageArrayNonUniformIndexing(VK_TRUE)
        .setPNext(gpu.synchronization2 ? &synchronization2Features : nullptr);

    auto extensions = std::vector<const char*>{};
    for (const auto& name : gpu.deviceExtensions)
    {
      extensions.push_back(name.c_str());
    }

    const auto info = vk::DeviceCreateInfo{}
                        .setQueueCreateInfos(queueInfo)
                        .setPEnabledFeatures(&features)
                        .setPEnabledExtensionNames(extensions)
                        .setPNext(&vulkan12Features);

    auto device = vk::Device{};
    if (const auto result = gpu.physicalDevice.createDevice(&info, nullptr, &device);
        result != vk::Result::eSuccess)
    {
      return Error{fmt::format(
        "Cannot create the Vulkan device on {}: {}", gpu.name, vk::to_string(result))};
    }

    // :1054
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    return device;
  }
  catch (const vk::SystemError& e)
  {
    return Error{
      fmt::format("Cannot create the Vulkan device on {}: {}", gpu.name, e.what())};
  }
  catch (const std::exception& e)
  {
    return Error{
      fmt::format("Cannot create the Vulkan device on {}: {}", gpu.name, e.what())};
  }
}

} // namespace tb::lit
