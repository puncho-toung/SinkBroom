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

#include "Swapchain.h"

#include "VulkanDevice.h"
#include "lit/LitConfig.h"
#include "lit/SwapchainRules.h"

#include <fmt/format.h>

#include <algorithm>

// The swapchain parameters mirror neo/sys/DeviceManager_VK.cpp:1156-1244 at 285ea4d1, as
// listed in mirror.json; the present mode and the generation split are the plan's.

namespace tb::lit
{
namespace
{

vk::CompositeAlphaFlagBitsKHR compositeAlpha(
  const vk::SurfaceCapabilitiesKHR& capabilities)
{
  // the engine always asks for opaque (:1209); a surface that cannot do it gets the
  // first mode it can do
  for (const auto alpha :
       {vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::eInherit,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied})
  {
    if (capabilities.supportedCompositeAlpha & alpha)
    {
      return alpha;
    }
  }
  return vk::CompositeAlphaFlagBitsKHR::eOpaque;
}

vk::SurfaceTransformFlagBitsKHR preTransform(
  const vk::SurfaceCapabilitiesKHR& capabilities)
{
  // the engine always asks for identity (:1208)
  return capabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity
           ? vk::SurfaceTransformFlagBitsKHR::eIdentity
           : capabilities.currentTransform;
}

} // namespace

Result<std::unique_ptr<SwapchainGeneration>> createSwapchainGeneration(
  const SwapchainSpec& spec)
{
  auto generation = std::make_unique<SwapchainGeneration>();
  generation->number = spec.number;

  try
  {
    auto presents = vk::Bool32{VK_FALSE};
    if (
      spec.physicalDevice.getSurfaceSupportKHR(spec.queueFamily, spec.surface, &presents)
        != vk::Result::eSuccess
      || !presents)
    {
      return Error{
        "The preview's GPU cannot present to this window, which can follow a screen "
        "change on a hybrid-GPU laptop. Press Start again."};
    }

    const auto capabilities = spec.physicalDevice.getSurfaceCapabilitiesKHR(spec.surface);
    const auto extent = chooseSwapchainExtent(capabilities, spec.width, spec.height);
    if (!extent)
    {
      return std::unique_ptr<SwapchainGeneration>{};
    }

    const auto formats = spec.physicalDevice.getSurfaceFormatsKHR(spec.surface);
    const auto format = std::ranges::find_if(
      formats, [](const auto& f) { return f.format == SwapchainFormat; });
    if (format == formats.end())
    {
      return Error{"This window cannot show B8G8R8A8_UNORM images"};
    }

    auto offeredModes = std::vector<VkPresentModeKHR>{};
    for (const auto mode : spec.physicalDevice.getSurfacePresentModesKHR(spec.surface))
    {
      offeredModes.push_back(VkPresentModeKHR(mode));
    }

    generation->extent = *extent;
    generation->presentMode =
      vk::PresentModeKHR(choosePresentMode(offeredModes, spec.forceFifo));

    // :1197-1212; one queue family, so the images are exclusive to it
    const auto info =
      vk::SwapchainCreateInfoKHR{}
        .setSurface(spec.surface)
        .setMinImageCount(chooseSwapchainImageCount(capabilities))
        .setImageFormat(format->format)
        .setImageColorSpace(format->colorSpace)
        .setImageExtent(generation->extent)
        .setImageArrayLayers(1)
        .setImageUsage(
          vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst
          | vk::ImageUsageFlagBits::eSampled)
        .setImageSharingMode(vk::SharingMode::eExclusive)
        .setPreTransform(preTransform(capabilities))
        .setCompositeAlpha(compositeAlpha(capabilities))
        .setPresentMode(generation->presentMode)
        .setClipped(VK_TRUE)
        .setOldSwapchain(nullptr);

    if (const auto result =
          spec.device.createSwapchainKHR(&info, nullptr, &generation->swapchain);
        result != vk::Result::eSuccess)
    {
      generation->swapchain = nullptr;
      return Error{fmt::format("Cannot create the swapchain: {}", vk::to_string(result))};
    }

    generation->images = spec.device.getSwapchainImagesKHR(generation->swapchain);

    // :1228-1238
    for (const auto image : generation->images)
    {
      const auto desc = nvrhi::TextureDesc{}
                          .setWidth(generation->extent.width)
                          .setHeight(generation->extent.height)
                          .setFormat(nvrhi::Format::BGRA8_UNORM)
                          .setDebugName("Lit preview swapchain image")
                          .setInitialState(nvrhi::ResourceStates::Present)
                          .setKeepInitialState(true)
                          .setIsRenderTarget(true);
      auto texture = spec.nvrhiDevice->createHandleForNativeTexture(
        nvrhi::ObjectTypes::VK_Image, nvrhi::Object(static_cast<VkImage>(image)), desc);
      if (!texture)
      {
        destroySwapchainGeneration(spec.device, *generation);
        return Error{"Cannot wrap the swapchain images for NVRHI"};
      }
      generation->imageTextures.push_back(std::move(texture));
    }

    for (std::size_t i = 0; i < FramesInFlight; ++i)
    {
      generation->acquireSemaphores.push_back(
        spec.device.createSemaphore(vk::SemaphoreCreateInfo{}));
    }
    for (std::size_t i = 0; i < generation->images.size(); ++i)
    {
      generation->presentSemaphores.push_back(
        spec.device.createSemaphore(vk::SemaphoreCreateInfo{}));
    }

    return generation;
  }
  catch (const std::exception& e)
  {
    destroySwapchainGeneration(spec.device, *generation);
    return Error{fmt::format("Cannot create the swapchain: {}", e.what())};
  }
}

void releaseSwapchainWrappers(SwapchainGeneration& generation)
{
  generation.imageTextures.clear();
}

void destroySwapchainGeneration(const vk::Device device, SwapchainGeneration& generation)
{
  // destroy functions do not fail, also on a lost device
  releaseSwapchainWrappers(generation);
  if (generation.swapchain)
  {
    device.destroySwapchainKHR(generation.swapchain);
    generation.swapchain = nullptr;
  }
  generation.images.clear();
  for (const auto semaphore : generation.acquireSemaphores)
  {
    device.destroySemaphore(semaphore);
  }
  generation.acquireSemaphores.clear();
  for (const auto semaphore : generation.presentSemaphores)
  {
    device.destroySemaphore(semaphore);
  }
  generation.presentSemaphores.clear();
}

} // namespace tb::lit
