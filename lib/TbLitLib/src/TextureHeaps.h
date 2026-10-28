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

#pragma once

#include "RangeAllocator.h"
#include "base/Macros.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{

/**
 * LP6.4a (C68 (a), D171, P8.3): a slot texture's class for the heaps: its format and its
 * dimension, as engineTextureDesc makes them (Texture2D, TextureCube of 6 slices,
 * Texture2DArray of 6 slices).
 */
struct FormatClass
{
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Texture2D;

  bool operator==(const FormatClass&) const = default;
};

// a class's name for the log ("RGBA8_UNORM 2D", "BC1_UNORM cube", "D32 array")
std::string formatClassName(const FormatClass& formatClass);

/**
 * LP6.4a (D171, P8.3): the classes the slots' textures can be made in: each format
 * engineTextureFormat maps an engine image format to (on a device with or without D24S8),
 * once, in the engine formats' order, times the three dimensions engineTextureDesc makes.
 * Pure.
 */
std::vector<FormatClass> engineFormatClasses(bool d24s8Supported);

/**
 * LP6.4a (P8.3): what the Start probe found for one class: whether the device makes it
 * at all (vkGetPhysicalDeviceImageFormatProperties with NVRHI's usage and flags for a
 * slot texture: optimal tiling, TRANSFER_SRC | TRANSFER_DST | SAMPLED, CUBE_COMPATIBLE
 * for a cube; a class it does not make is never created), the probe image's
 * memoryTypeBits, alignment and dedicated requirements
 * (vkGetImageMemoryRequirements2 with VkMemoryDedicatedRequirements), and so whether a
 * texture of the class may go into a heap: the memory type NVRHI's createHeap takes
 * (heapMemoryType) is among its memoryTypeBits and it does not require a dedicated
 * allocation. prefersDedicated is a note (the log's), never a refusal.
 */
struct FormatClassEntry
{
  FormatClass formatClass;
  bool supported = false;
  bool probed = false;
  std::uint32_t memoryTypeBits = 0;
  std::uint64_t alignment = 0;
  bool requiresDedicated = false;
  bool prefersDedicated = false;
  bool heapable = false;
  // why the class stays dedicated (empty when heapable)
  std::string reason;
};

// the probe's table: every class, the memory type a DeviceLocal heap takes, the time
struct FormatClassTable
{
  std::vector<FormatClassEntry> entries;
  // the first memory type whose flags hold DEVICE_LOCAL (NVRHI
  // vulkan-allocator.cpp:107-113 with memoryTypeBits ~0u, as createHeap asks); nullopt
  // when the device has none
  std::optional<std::uint32_t> heapMemoryType;
  double milliseconds = 0.0;
};

/**
 * LP6.4a (P8.3, the director's reading (3)), render thread, at the session's first frame
 * before any upload: for each class of engineFormatClasses(d24s8Supported) the device
 * makes, one single-level virtual texture of one block (4 × 4 for the block-compressed
 * formats, 1 × 1 else; a cube or an array of 6 slices), its native image's
 * vkGetImageMemoryRequirements2 through the session's dispatcher
 * (VULKAN_HPP_DEFAULT_DISPATCHER), the texture released at once. NVRHI checks none of it
 * (createHeap takes the first DEVICE_LOCAL type with memoryTypeBits ~0u,
 * bindTextureMemory binds without a type, alignment or dedicated check).
 */
FormatClassTable probeFormatClasses(
  nvrhi::IDevice& device,
  vk::Device vkDevice,
  vk::PhysicalDevice physicalDevice,
  bool d24s8Supported);

// the probe's one Info line: the classes probed with their alignments, the classes kept
// dedicated and why, the time
std::string formatClassLine(const FormatClassTable& table);

// LP6.4a, any thread (tests): while set, the class answers "refused" (a forced
// dedicated class: the probe's line says so, and the heaps place none of it from the
// next texture on); process-wide, like the session's other static test hooks
void setFormatClassRefusedForTesting(const FormatClass& formatClass, bool refused);
bool formatClassRefusedForTesting(const FormatClass& formatClass);

// LP6.4a, any thread (tests): the heaps' size the next sessions make (0: HeapBytes, Q47);
// process-wide like the hook above; and the size a session takes at its phase B
void setTextureHeapBytesForTesting(std::uint64_t bytes);
std::uint64_t textureHeapBytes();

// the heaps' allocators' own unit: none of their own, each texture's own alignment (a
// power of two) aligns its offset (RangeAllocator::allocate(size, alignment), the
// director's reading (6)); only optimal-tiling images share a heap, so Vulkan's
// bufferImageGranularity never applies between neighbours
constexpr auto HeapUnit = std::uint64_t{1};

// where a texture is: a heap and its range
struct HeapPlacement
{
  std::size_t heap = 0;
  ArenaRange range;

  bool operator==(const HeapPlacement&) const = default;
};

/**
 * LP6.4a (C68 (a), D171, R5.14, D50, §3.10 "Textures are placed in heaps"): the slots'
 * textures in a few DeviceLocal heaps of HeapBytes each, each with a RangeAllocator, as
 * the geometry arenas hold the chunks: a texture of a heap-able class (the Start probe)
 * is made virtual and bound into a range of its own size and alignment (the allocator's
 * aligned form, the texture's own requirement: the director's reading (6)); another
 * heap is made when none holds it, under U2.3's room (the director's reading (1): only a
 * new heap is an allocation; a texture that fits a heap makes none and is never refused
 * for the headroom). A texture let go frees its range stamped with the frame, handed out
 * again only once that frame has finished (reclaim; D35: its handle is held by
 * RetiredResources until the same frame). Heaps are not released before teardown (the
 * director's reading (5): LP6.4b compacts and releases); a heap outlives every texture
 * bound into it (NVRHI's Heap::~Heap frees the memory; a bound texture holds its heap).
 *
 * Threads: the render thread; releaseAll in teardown step 2 after the slots' textures.
 */
class TextureHeaps
{
public:
  // why place found no range
  enum class Refusal
  {
    None,
    // larger than a heap
    TooLarge,
    // a new heap would pass U2.3's room
    Room,
    // the device made no heap
    NoHeap,
  };

private:
  struct Heap
  {
    nvrhi::HeapHandle heap;
    RangeAllocator ranges;
  };

  nvrhi::IDevice* m_device = nullptr;
  std::uint64_t m_heapBytes = 0;
  std::vector<Heap> m_heaps;
  std::unordered_map<const nvrhi::ITexture*, HeapPlacement> m_placed;
  bool m_cpuHeapsForTesting = false;
  std::function<bool()> m_allocationRoom;
  std::uint64_t m_headroomRefusals = 0;
  Refusal m_lastRefusal = Refusal::None;
  std::optional<FormatClassTable> m_classes;
  // the largest and smallest range placed so far, and the textures placed so far
  std::uint64_t m_largestPlaced = 0;
  std::uint64_t m_smallestPlaced = 0;
  std::uint64_t m_placedTotal = 0;

public:
  TextureHeaps() = default;

  // the device the heaps are made on, and their size (HeapBytes; tests make smaller ones)
  void setDevice(nvrhi::IDevice& device, std::uint64_t heapBytes);
  // U2.3: asked before a new heap is made; when it says no, place finds nothing
  // (Refusal::Room) and headroomRefusals counts it
  void setAllocationRoom(std::function<bool()> room);
  std::uint64_t headroomRefusals() const;
  std::uint64_t heapBytes() const;

  // the Start probe's table (render thread), and whether a texture of the class may be
  // placed: probed, heap-able and not refused by a test (setFormatClassRefusedForTesting)
  void setFormatClasses(FormatClassTable table);
  const std::optional<FormatClassTable>& formatClassesForTesting() const;
  bool heapable(const FormatClass& formatClass) const;

  /**
   * A range for texture (virtual, not bound yet) of requirements' size and alignment:
   * the first heap whose allocator holds it (best fit within the heap), else a new heap
   * under the room; nullopt with lastRefusal: larger than a heap, the room refused, no
   * heap made. The caller binds it (bindTextureMemory) and retires it when it lets the
   * texture go.
   */
  std::optional<HeapPlacement> place(
    const nvrhi::ITexture* texture, const nvrhi::MemoryRequirements& requirements);
  Refusal lastRefusal() const;
  // the heap of a placement, for bindTextureMemory
  nvrhi::IHeap* heap(std::size_t index) const;
  // texture's range is no longer used from frame on: free once that frame has finished;
  // whether texture was placed
  bool retire(const nvrhi::ITexture* texture, std::uint64_t frame);
  // the ranges freed in frames that have finished (frames 0 to completedFrames - 1)
  void reclaim(std::uint64_t completedFrames);

  // whether texture is in a heap, and where (tests)
  bool placed(const nvrhi::ITexture* texture) const;
  std::optional<HeapPlacement> heapOfForTesting(const nvrhi::ITexture* texture) const;

  // C11, C16: the heaps made, their bytes (heaps × HeapBytes), the bytes handed out
  // (the ranges placed and those freed and waiting for their frames), the textures placed
  std::size_t heaps() const;
  std::uint64_t capacityBytes() const;
  std::uint64_t usedBytes() const;
  std::uint64_t pendingBytes() const;
  std::size_t placedTextures() const;
  // LP6.4b's figures: the largest and the smallest range placed so far, and how many
  std::uint64_t largestPlaced() const;
  std::uint64_t smallestPlaced() const;
  std::uint64_t placedTotal() const;

  // tests (CPU): heaps of heapBytes made without a device, ranges only
  void setCpuHeapsForTesting(std::uint64_t heapBytes);

  // teardown step 2, after the slots' textures are let go: every heap (no range may be
  // handed out still: asserted)
  void releaseAll();

private:
  std::optional<HeapPlacement> remember(
    const nvrhi::ITexture* texture, const HeapPlacement& placement);

  deleteCopyAndMove(TextureHeaps);
};

} // namespace tb::lit
