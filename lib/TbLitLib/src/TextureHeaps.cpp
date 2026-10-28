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

#include "TextureHeaps.h"

#include "lit/EngineImageEnums.h"
#include "lit/EngineTables.h"
#include "lit/LitConfig.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <map>

namespace tb::lit
{
namespace
{

// LP6.4a's test hook: the classes a test refused, each classCode, 0 for none;
// constant-initialised (C9) and process-wide like the session's other static test hooks
constexpr auto MaxRefusedClasses = std::size_t{8};
constinit auto s_refusedClasses =
  std::array<std::atomic<std::uint32_t>, MaxRefusedClasses>{};
// and the heaps' size a test asks for, 0 for HeapBytes
constinit auto s_heapBytesForTesting = std::atomic<std::uint64_t>{0};

std::uint32_t classCode(const FormatClass& formatClass)
{
  return ((std::uint32_t(formatClass.format) << 8u)
          | std::uint32_t(formatClass.dimension))
         + 1u;
}

// idImage::IsCompressed's formats as engineTextureFormat maps them: 4 × 4 blocks
bool blockCompressed(const nvrhi::Format format)
{
  return format == nvrhi::Format::BC1_UNORM || format == nvrhi::Format::BC3_UNORM
         || format == nvrhi::Format::BC6H_UFLOAT;
}

std::string_view dimensionName(const nvrhi::TextureDimension dimension)
{
  switch (dimension)
  {
  case nvrhi::TextureDimension::Texture2D:
    return "2D";
  case nvrhi::TextureDimension::TextureCube:
    return "cube";
  case nvrhi::TextureDimension::Texture2DArray:
    return "array";
  default:
    return "other";
  }
}

} // namespace

std::string formatClassName(const FormatClass& formatClass)
{
  return fmt::format(
    "{} {}",
    nvrhi::getFormatInfo(formatClass.format).name,
    dimensionName(formatClass.dimension));
}

std::vector<FormatClass> engineFormatClasses(const bool d24s8Supported)
{
  // engineTextureFormat's formats (Image_NVRHI.cpp:349-459), each once, in the engine
  // formats' order; the dimensions engineTextureDesc makes (Image_NVRHI.cpp:596-614)
  auto formats = std::vector<nvrhi::Format>{};
  for (auto format = engine_image::FormatNone; format <= engine_image::FormatRgba16s;
       ++format)
  {
    if (const auto texture = engineTextureFormat(format, d24s8Supported);
        texture && std::ranges::find(formats, *texture) == formats.end())
    {
      formats.push_back(*texture);
    }
  }
  auto classes = std::vector<FormatClass>{};
  for (const auto dimension :
       {nvrhi::TextureDimension::Texture2D,
        nvrhi::TextureDimension::TextureCube,
        nvrhi::TextureDimension::Texture2DArray})
  {
    for (const auto format : formats)
    {
      classes.push_back(FormatClass{format, dimension});
    }
  }
  return classes;
}

FormatClassTable probeFormatClasses(
  nvrhi::IDevice& device,
  const vk::Device vkDevice,
  const vk::PhysicalDevice physicalDevice,
  const bool d24s8Supported)
{
  const auto start = std::chrono::steady_clock::now();
  auto table = FormatClassTable{};
  // NVRHI's createHeap (vulkan-device.cpp:536-575) asks allocateMemory for
  // memoryTypeBits ~0u and DEVICE_LOCAL: the first type whose flags hold it
  // (vulkan-allocator.cpp:107-113)
  const auto memory = physicalDevice.getMemoryProperties();
  for (std::uint32_t i = 0; i < memory.memoryTypeCount && i < 32u; ++i)
  {
    if (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)
    {
      table.heapMemoryType = i;
      break;
    }
  }
  // the session's dispatcher (C9: the loader is never linked); core in Vulkan 1.1
  const auto requirements2 =
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetImageMemoryRequirements2 != nullptr;
  for (const auto& formatClass : engineFormatClasses(d24s8Supported))
  {
    auto entry = FormatClassEntry{};
    entry.formatClass = formatClass;
    const auto cube = formatClass.dimension == nvrhi::TextureDimension::TextureCube;
    const auto layers =
      formatClass.dimension == nvrhi::TextureDimension::Texture2D ? 1u : 6u;
    const auto side = blockCompressed(formatClass.format) ? 4u : 1u;
    // the image NVRHI makes of a slot's desc (vulkan-texture.cpp:102-128, 204-220):
    // optimal tiling, TRANSFER_SRC | TRANSFER_DST | SAMPLED, CUBE_COMPATIBLE for a cube
    const auto vkFormat = vk::Format(nvrhi::vulkan::convertFormat(formatClass.format));
    const auto usage = vk::ImageUsageFlagBits::eTransferSrc
                       | vk::ImageUsageFlagBits::eTransferDst
                       | vk::ImageUsageFlagBits::eSampled;
    const auto flags = cube
                         ? vk::ImageCreateFlags{vk::ImageCreateFlagBits::eCubeCompatible}
                         : vk::ImageCreateFlags{};
    auto properties = vk::ImageFormatProperties{};
    entry.supported = vkFormat != vk::Format::eUndefined
                      && physicalDevice.getImageFormatProperties(
                           vkFormat,
                           vk::ImageType::e2D,
                           vk::ImageTiling::eOptimal,
                           usage,
                           flags,
                           &properties)
                           == vk::Result::eSuccess
                      && properties.maxArrayLayers >= layers
                      && properties.maxExtent.width >= side
                      && properties.maxExtent.height >= side;
    if (!entry.supported)
    {
      entry.reason = "the device does not make it";
      table.entries.push_back(std::move(entry));
      continue;
    }
    if (!requirements2 || !table.heapMemoryType)
    {
      entry.reason = !requirements2 ? "vkGetImageMemoryRequirements2 is not loaded"
                                    : "the device has no DEVICE_LOCAL memory type";
      table.entries.push_back(std::move(entry));
      continue;
    }
    auto desc = nvrhi::TextureDesc{}
                  .setDebugName("Lit preview format-class probe")
                  .setDimension(formatClass.dimension)
                  .setWidth(side)
                  .setHeight(side)
                  .setArraySize(layers)
                  .setFormat(formatClass.format)
                  .setMipLevels(1)
                  .setInitialState(nvrhi::ResourceStates::ShaderResource)
                  .setKeepInitialState(true);
    desc.isVirtual = true;
    const auto texture = device.createTexture(desc);
    const auto image =
      texture ? VkImage(texture->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer)
              : VK_NULL_HANDLE;
    if (image == VK_NULL_HANDLE)
    {
      entry.reason = "NVRHI made no probe image";
      table.entries.push_back(std::move(entry));
      continue;
    }
    auto dedicated = vk::MemoryDedicatedRequirements{};
    auto requirements = vk::MemoryRequirements2{};
    requirements.pNext = &dedicated;
    const auto info = vk::ImageMemoryRequirementsInfo2{}.setImage(vk::Image{image});
    vkDevice.getImageMemoryRequirements2(&info, &requirements);
    entry.probed = true;
    entry.memoryTypeBits = requirements.memoryRequirements.memoryTypeBits;
    entry.alignment = requirements.memoryRequirements.alignment;
    entry.requiresDedicated = dedicated.requiresDedicatedAllocation == VK_TRUE;
    entry.prefersDedicated = dedicated.prefersDedicatedAllocation == VK_TRUE;
    const auto typeAllowed = (entry.memoryTypeBits >> *table.heapMemoryType) & 1u;
    entry.heapable = typeAllowed != 0 && !entry.requiresDedicated;
    if (!typeAllowed)
    {
      entry.reason = fmt::format(
        "the heaps' memory type {} is not in its memoryTypeBits 0x{:x}",
        *table.heapMemoryType,
        entry.memoryTypeBits);
    }
    else if (entry.requiresDedicated)
    {
      entry.reason = "it requires a dedicated allocation";
    }
    table.entries.push_back(std::move(entry));
  }
  table.milliseconds =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
  return table;
}

std::string formatClassLine(const FormatClassTable& table)
{
  // the probed classes grouped by alignment, each format once with its dimensions
  // ("RGBA8_UNORM 2D/cube/array"), then the classes kept dedicated with why
  auto byAlignment =
    std::map<std::uint64_t, std::vector<std::pair<nvrhi::Format, std::string>>>{};
  auto dedicated = std::vector<std::string>{};
  auto unsupported = std::vector<std::string>{};
  auto prefers = std::vector<std::string>{};
  for (const auto& entry : table.entries)
  {
    const auto name = formatClassName(entry.formatClass);
    if (!entry.supported)
    {
      unsupported.push_back(name);
      continue;
    }
    if (entry.probed)
    {
      auto& group = byAlignment[entry.alignment];
      const auto format = std::ranges::find(
        group, entry.formatClass.format, &std::pair<nvrhi::Format, std::string>::first);
      const auto dimension = dimensionName(entry.formatClass.dimension);
      if (format == group.end())
      {
        group.emplace_back(entry.formatClass.format, name);
      }
      else
      {
        format->second += fmt::format("/{}", dimension);
      }
      if (entry.prefersDedicated)
      {
        prefers.push_back(name);
      }
    }
    const auto forced = formatClassRefusedForTesting(entry.formatClass);
    if (!entry.heapable || forced)
    {
      dedicated.push_back(fmt::format(
        "{} ({})", name, forced && entry.heapable ? "refused by a test" : entry.reason));
    }
  }
  auto alignments = std::vector<std::string>{};
  for (const auto& [alignment, group] : byAlignment)
  {
    auto names = std::vector<std::string>{};
    for (const auto& [format, name] : group)
    {
      names.push_back(name);
    }
    alignments.push_back(
      fmt::format("alignment {}: {}", alignment, fmt::join(names, ", ")));
  }
  return fmt::format(
    "texture heaps (LP6.4a): {} classes probed in {:.1f} ms, the heaps in memory type "
    "{}; {}; kept dedicated: {}; prefer a dedicated allocation (a note): {}; not made by "
    "the device: {}",
    std::ranges::count_if(table.entries, &FormatClassEntry::probed),
    table.milliseconds,
    table.heapMemoryType ? fmt::format("{}", *table.heapMemoryType) : "none",
    fmt::join(alignments, "; "),
    dedicated.empty() ? std::string{"none"}
                      : fmt::format("{}", fmt::join(dedicated, ", ")),
    prefers.empty() ? std::string{"none"} : fmt::format("{}", fmt::join(prefers, ", ")),
    unsupported.empty() ? std::string{"none"}
                        : fmt::format("{}", fmt::join(unsupported, ", ")));
}

void setFormatClassRefusedForTesting(const FormatClass& formatClass, const bool refused)
{
  const auto code = classCode(formatClass);
  for (auto& slot : s_refusedClasses)
  {
    if (slot.load() == code)
    {
      if (!refused)
      {
        slot = 0;
      }
      return;
    }
  }
  if (refused)
  {
    for (auto& slot : s_refusedClasses)
    {
      auto empty = std::uint32_t{0};
      if (slot.compare_exchange_strong(empty, code))
      {
        return;
      }
    }
  }
}

bool formatClassRefusedForTesting(const FormatClass& formatClass)
{
  const auto code = classCode(formatClass);
  return std::ranges::any_of(
    s_refusedClasses, [&](const auto& slot) { return slot.load() == code; });
}

void setTextureHeapBytesForTesting(const std::uint64_t bytes)
{
  s_heapBytesForTesting = bytes;
}

std::uint64_t textureHeapBytes()
{
  const auto bytes = s_heapBytesForTesting.load();
  return bytes != 0 ? bytes : HeapBytes;
}

void TextureHeaps::setDevice(nvrhi::IDevice& device, const std::uint64_t heapBytes)
{
  m_device = &device;
  m_heapBytes = heapBytes;
}

void TextureHeaps::setAllocationRoom(std::function<bool()> room)
{
  m_allocationRoom = std::move(room);
}

std::uint64_t TextureHeaps::headroomRefusals() const
{
  return m_headroomRefusals;
}

std::uint64_t TextureHeaps::heapBytes() const
{
  return m_heapBytes;
}

void TextureHeaps::setFormatClasses(FormatClassTable table)
{
  m_classes = std::move(table);
}

const std::optional<FormatClassTable>& TextureHeaps::formatClassesForTesting() const
{
  return m_classes;
}

bool TextureHeaps::heapable(const FormatClass& formatClass) const
{
  if (!m_classes || formatClassRefusedForTesting(formatClass))
  {
    return false;
  }
  const auto it =
    std::ranges::find(m_classes->entries, formatClass, &FormatClassEntry::formatClass);
  return it != m_classes->entries.end() && it->heapable;
}

std::optional<HeapPlacement> TextureHeaps::place(
  const nvrhi::ITexture* texture, const nvrhi::MemoryRequirements& requirements)
{
  m_lastRefusal = Refusal::None;
  if (!m_device && !m_cpuHeapsForTesting)
  {
    m_lastRefusal = Refusal::NoHeap;
    return std::nullopt;
  }
  if (requirements.size == 0 || requirements.size > m_heapBytes)
  {
    m_lastRefusal = Refusal::TooLarge;
    return std::nullopt;
  }
  // the texture's own alignment (the director's reading (6)), a power of two
  const auto alignment = std::max(requirements.alignment, std::uint64_t{1});
  for (std::size_t i = 0; i < m_heaps.size(); ++i)
  {
    if (!m_heaps[i].live)
    {
      continue;
    }
    if (const auto range = m_heaps[i].ranges.allocate(requirements.size, alignment))
    {
      return remember(texture, HeapPlacement{i, *range});
    }
  }
  // U2.3: a new heap is a device allocation (the director's reading (1))
  if (m_allocationRoom && !m_allocationRoom())
  {
    m_lastRefusal = Refusal::Room;
    ++m_headroomRefusals;
    return std::nullopt;
  }
  // LP6.4b (the director's reading (7)): a released heap's index is taken again
  const auto released =
    std::ranges::find_if(m_heaps, [](const Heap& h) { return !h.live; });
  const auto index = std::size_t(released - m_heaps.begin());
  auto heap = nvrhi::HeapHandle{};
  if (m_device)
  {
    heap = m_device->createHeap(
      nvrhi::HeapDesc{}
        .setCapacity(m_heapBytes)
        .setType(nvrhi::HeapType::DeviceLocal)
        .setDebugName(fmt::format("Lit preview texture heap {}", index)));
    if (!heap)
    {
      m_lastRefusal = Refusal::NoHeap;
      return std::nullopt;
    }
  }
  auto made = Heap{std::move(heap), RangeAllocator{m_heapBytes, HeapUnit}, true};
  if (index < m_heaps.size())
  {
    m_heaps[index] = std::move(made);
  }
  else
  {
    m_heaps.push_back(std::move(made));
  }
  const auto range = m_heaps[index].ranges.allocate(requirements.size, alignment);
  if (!range)
  {
    m_lastRefusal = Refusal::TooLarge;
    return std::nullopt;
  }
  return remember(texture, HeapPlacement{index, *range});
}

TextureHeaps::Refusal TextureHeaps::lastRefusal() const
{
  return m_lastRefusal;
}

nvrhi::IHeap* TextureHeaps::heap(const std::size_t index) const
{
  return index < m_heaps.size() && m_heaps[index].live ? m_heaps[index].heap.Get()
                                                       : nullptr;
}

bool TextureHeaps::retire(const nvrhi::ITexture* texture, const std::uint64_t frame)
{
  const auto it = m_placed.find(texture);
  if (it == m_placed.end())
  {
    return false;
  }
  m_heaps[it->second.heap].ranges.free(it->second.range, frame);
  m_placed.erase(it);
  return true;
}

void TextureHeaps::reclaim(const std::uint64_t completedFrames)
{
  for (auto& heap : m_heaps)
  {
    heap.ranges.reclaim(completedFrames);
  }
}

bool TextureHeaps::placed(const nvrhi::ITexture* texture) const
{
  return m_placed.contains(texture);
}

std::optional<HeapPlacement> TextureHeaps::placementOf(
  const nvrhi::ITexture* texture) const
{
  const auto it = m_placed.find(texture);
  return it != m_placed.end() ? std::optional{it->second} : std::nullopt;
}

std::optional<HeapPlacement> TextureHeaps::heapOfForTesting(
  const nvrhi::ITexture* texture) const
{
  return placementOf(texture);
}

std::size_t TextureHeaps::releaseEmpty()
{
  auto released = std::size_t{0};
  for (auto& heap : m_heaps)
  {
    if (heap.live && heap.ranges.usedBytes() == 0 && heap.ranges.pendingBytes() == 0)
    {
      heap.heap = nullptr;
      heap.ranges = RangeAllocator{m_heapBytes, HeapUnit};
      heap.live = false;
      ++released;
    }
  }
  m_heapsReleased += released;
  return released;
}

std::uint64_t TextureHeaps::heapsReleased() const
{
  return m_heapsReleased;
}

std::size_t TextureHeaps::heaps() const
{
  return std::size_t(
    std::ranges::count_if(m_heaps, [](const Heap& heap) { return heap.live; }));
}

std::uint64_t TextureHeaps::capacityBytes() const
{
  return std::uint64_t(heaps()) * m_heapBytes;
}

std::uint64_t TextureHeaps::usedBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& heap : m_heaps)
  {
    bytes += heap.ranges.usedBytes() + heap.ranges.pendingBytes();
  }
  return bytes;
}

std::uint64_t TextureHeaps::pendingBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& heap : m_heaps)
  {
    bytes += heap.ranges.pendingBytes();
  }
  return bytes;
}

std::size_t TextureHeaps::freeRanges() const
{
  auto ranges = std::size_t{0};
  for (const auto& heap : m_heaps)
  {
    ranges += heap.live ? heap.ranges.freeRanges() : 0;
  }
  return ranges;
}

std::uint64_t TextureHeaps::largestFree() const
{
  auto largest = std::uint64_t{0};
  for (const auto& heap : m_heaps)
  {
    largest = std::max(largest, heap.live ? heap.ranges.largestFree() : 0);
  }
  return largest;
}

std::size_t TextureHeaps::placedTextures() const
{
  return m_placed.size();
}

std::uint64_t TextureHeaps::largestPlaced() const
{
  return m_largestPlaced;
}

std::uint64_t TextureHeaps::smallestPlaced() const
{
  return m_smallestPlaced;
}

std::uint64_t TextureHeaps::placedTotal() const
{
  return m_placedTotal;
}

void TextureHeaps::setCpuHeapsForTesting(const std::uint64_t heapBytes)
{
  m_cpuHeapsForTesting = true;
  m_heapBytes = heapBytes;
}

void TextureHeaps::releaseAll()
{
  // a heap outlives every texture bound into it (NVRHI's Heap::~Heap frees its memory):
  // the slots let theirs go first
  assert(m_placed.empty() && "a texture is still placed in a heap at teardown");
  m_placed.clear();
  m_heaps.clear();
}

std::optional<HeapPlacement> TextureHeaps::remember(
  const nvrhi::ITexture* texture, const HeapPlacement& placement)
{
  m_placed.insert_or_assign(texture, placement);
  m_largestPlaced = std::max(m_largestPlaced, placement.range.size);
  m_smallestPlaced = m_smallestPlaced == 0
                       ? placement.range.size
                       : std::min(m_smallestPlaced, placement.range.size);
  ++m_placedTotal;
  return placement;
}

} // namespace tb::lit
