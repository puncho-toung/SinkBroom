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

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// a texture the heaps place by its address only (never bound, never drawn)
class FakeTexture : public nvrhi::RefCounter<nvrhi::ITexture>
{
private:
  nvrhi::TextureDesc m_desc;

public:
  const nvrhi::TextureDesc& getDesc() const override { return m_desc; }
  nvrhi::Object getNativeView(
    nvrhi::ObjectType,
    nvrhi::Format,
    nvrhi::TextureSubresourceSet,
    nvrhi::TextureDimension,
    bool) override
  {
    return nullptr;
  }
};

std::vector<nvrhi::TextureHandle> fakeTextures(const std::size_t count)
{
  auto textures = std::vector<nvrhi::TextureHandle>{};
  for (std::size_t i = 0; i < count; ++i)
  {
    textures.push_back(nvrhi::TextureHandle::Create(new FakeTexture{}));
  }
  return textures;
}

nvrhi::MemoryRequirements requirements(
  const std::uint64_t size, const std::uint64_t alignment)
{
  auto result = nvrhi::MemoryRequirements{};
  result.size = size;
  result.alignment = alignment;
  return result;
}

constexpr auto KiB = std::uint64_t{1024};

} // namespace

TEST_CASE("TextureHeaps")
{
  SECTION(
    "engineFormatClasses: the engine table's formats, once each, times 3 dimensions")
  {
    for (const auto d24s8 : {true, false})
    {
      CAPTURE(d24s8);
      const auto classes = engineFormatClasses(d24s8);
      // 17 formats (engineTextureFormat over the engine formats 1-24: ETC1 and BC7
      // refused, R8 and D32 shared by four and two engine formats, RGBA8_UINT by two)
      REQUIRE(classes.size() == 3u * 17u);
      CHECK(
        classes.front()
        == FormatClass{nvrhi::Format::RGBA8_UNORM, nvrhi::TextureDimension::Texture2D});
      auto seen = std::set<std::pair<int, int>>{};
      for (const auto& formatClass : classes)
      {
        CHECK(seen.emplace(int(formatClass.format), int(formatClass.dimension)).second);
      }
      for (const auto dimension :
           {nvrhi::TextureDimension::Texture2D,
            nvrhi::TextureDimension::TextureCube,
            nvrhi::TextureDimension::Texture2DArray})
      {
        CHECK(std::ranges::count(classes, dimension, &FormatClass::dimension) == 17);
        for (const auto format :
             {nvrhi::Format::RGBA8_UNORM,
              nvrhi::Format::BC1_UNORM,
              nvrhi::Format::BC3_UNORM,
              nvrhi::Format::BC6H_UFLOAT,
              nvrhi::Format::R11G11B10_FLOAT,
              nvrhi::Format::D32})
        {
          CHECK(
            std::ranges::find(classes, FormatClass{format, dimension}) != classes.end());
        }
      }
      const auto depthStencil = d24s8 ? nvrhi::Format::D24S8 : nvrhi::Format::D32S8;
      const auto other = d24s8 ? nvrhi::Format::D32S8 : nvrhi::Format::D24S8;
      CHECK(std::ranges::count(classes, depthStencil, &FormatClass::format) == 3);
      CHECK(std::ranges::count(classes, other, &FormatClass::format) == 0);
    }
    CHECK(
      formatClassName(
        FormatClass{nvrhi::Format::BC1_UNORM, nvrhi::TextureDimension::TextureCube})
      == "BC1_UNORM cube");
  }

  SECTION("heapable: probed, heap-able and not refused by a test")
  {
    auto heaps = TextureHeaps{};
    const auto rgba =
      FormatClass{nvrhi::Format::RGBA8_UNORM, nvrhi::TextureDimension::Texture2D};
    const auto bc1 =
      FormatClass{nvrhi::Format::BC1_UNORM, nvrhi::TextureDimension::Texture2D};
    // no probe yet: nothing goes into a heap
    CHECK_FALSE(heaps.heapable(rgba));
    auto table = FormatClassTable{};
    table.heapMemoryType = 1;
    auto entry = FormatClassEntry{};
    entry.formatClass = rgba;
    entry.supported = true;
    entry.probed = true;
    entry.memoryTypeBits = 0x2;
    entry.alignment = 1024;
    entry.heapable = true;
    table.entries.push_back(entry);
    entry.formatClass = bc1;
    entry.memoryTypeBits = 0x4;
    entry.heapable = false;
    entry.reason = "the heaps' memory type 1 is not in its memoryTypeBits 0x4";
    table.entries.push_back(entry);
    heaps.setFormatClasses(table);
    CHECK(heaps.heapable(rgba));
    CHECK_FALSE(heaps.heapable(bc1));
    CHECK_FALSE(heaps.heapable(
      FormatClass{nvrhi::Format::RGBA8_UNORM, nvrhi::TextureDimension::TextureCube}));
    REQUIRE(heaps.formatClassesForTesting());
    CHECK(heaps.formatClassesForTesting()->entries.size() == 2u);

    // the test hook refuses a class until it is cleared, and the probe's line says so
    setFormatClassRefusedForTesting(rgba, true);
    CHECK(formatClassRefusedForTesting(rgba));
    CHECK_FALSE(heaps.heapable(rgba));
    const auto line = formatClassLine(table);
    CHECK(line.find("RGBA8_UNORM 2D (refused by a test)") != std::string::npos);
    CHECK(line.find("BC1_UNORM 2D (the heaps' memory type 1") != std::string::npos);
    CHECK(line.find("alignment 1024: RGBA8_UNORM 2D, BC1_UNORM 2D") != std::string::npos);
    setFormatClassRefusedForTesting(rgba, false);
    CHECK_FALSE(formatClassRefusedForTesting(rgba));
    CHECK(heaps.heapable(rgba));
    CHECK(formatClassLine(table).find("refused by a test") == std::string::npos);
  }

  SECTION("place: one heap, then a second when the first is full, nothing over a heap")
  {
    auto heaps = TextureHeaps{};
    heaps.setCpuHeapsForTesting(256 * KiB);
    const auto textures = fakeTextures(6);
    CHECK(heaps.heaps() == 0u);
    CHECK(heaps.capacityBytes() == 0u);

    // each texture's own alignment aligns its offset; the first one makes the heap
    const auto a = heaps.place(textures[0].Get(), requirements(10 * KiB, 4 * KiB));
    REQUIRE(a);
    CHECK(*a == HeapPlacement{0, ArenaRange{0, 10 * KiB}});
    const auto b = heaps.place(textures[1].Get(), requirements(64 * KiB, 64 * KiB));
    REQUIRE(b);
    CHECK(*b == HeapPlacement{0, ArenaRange{64 * KiB, 64 * KiB}});
    // the padding between them stays free and takes what fits
    const auto c = heaps.place(textures[2].Get(), requirements(32 * KiB, 1 * KiB));
    REQUIRE(c);
    CHECK(*c == HeapPlacement{0, ArenaRange{10 * KiB, 32 * KiB}});
    CHECK(heaps.heaps() == 1u);
    CHECK(heaps.capacityBytes() == 256 * KiB);
    CHECK(heaps.usedBytes() == 106 * KiB);
    CHECK(heaps.placedTextures() == 3u);
    CHECK(heaps.placed(textures[1].Get()));
    CHECK(heaps.heapOfForTesting(textures[1].Get()) == b);

    // 192 KiB do not fit the first heap's 150 KiB left: a second heap
    const auto d = heaps.place(textures[3].Get(), requirements(192 * KiB, 4 * KiB));
    REQUIRE(d);
    CHECK(*d == HeapPlacement{1, ArenaRange{0, 192 * KiB}});
    CHECK(heaps.heaps() == 2u);
    CHECK(heaps.capacityBytes() == 512 * KiB);

    // larger than a heap: placed nowhere, no heap made
    CHECK_FALSE(heaps.place(textures[4].Get(), requirements(257 * KiB, 4 * KiB)));
    CHECK(heaps.lastRefusal() == TextureHeaps::Refusal::TooLarge);
    CHECK(heaps.heaps() == 2u);
    CHECK_FALSE(heaps.placed(textures[4].Get()));
    CHECK(heaps.largestPlaced() == 192 * KiB);
    CHECK(heaps.smallestPlaced() == 10 * KiB);
    CHECK(heaps.placedTotal() == 4u);

    for (std::size_t i = 0; i < 4; ++i)
    {
      CHECK(heaps.retire(textures[i].Get(), 1));
    }
    CHECK_FALSE(heaps.retire(textures[4].Get(), 1));
    heaps.reclaim(2);
    CHECK(heaps.usedBytes() == 0u);
    heaps.releaseAll();
    CHECK(heaps.heaps() == 0u);
    CHECK(heaps.capacityBytes() == 0u);
  }

  SECTION("retire: a range is handed out again only once its frame has finished")
  {
    auto heaps = TextureHeaps{};
    heaps.setCpuHeapsForTesting(128 * KiB);
    const auto textures = fakeTextures(3);
    const auto a = heaps.place(textures[0].Get(), requirements(128 * KiB, 4 * KiB));
    REQUIRE(a);
    // freed in frame 5: pending (in the used bytes) until frame 5 has finished
    CHECK(heaps.retire(textures[0].Get(), 5));
    CHECK_FALSE(heaps.placed(textures[0].Get()));
    CHECK(heaps.usedBytes() == 128 * KiB);
    CHECK(heaps.pendingBytes() == 128 * KiB);
    // no room in the one heap: without the room (U2.3) no second heap either
    auto room = true;
    heaps.setAllocationRoom([&] { return room; });
    room = false;
    CHECK_FALSE(heaps.place(textures[1].Get(), requirements(64 * KiB, 4 * KiB)));
    CHECK(heaps.lastRefusal() == TextureHeaps::Refusal::Room);
    CHECK(heaps.headroomRefusals() == 1u);
    CHECK(heaps.heaps() == 1u);
    heaps.reclaim(5);
    CHECK(heaps.pendingBytes() == 128 * KiB);
    heaps.reclaim(6);
    CHECK(heaps.usedBytes() == 0u);
    // the range again, at the same offset; a texture that fits a heap is never refused
    // for the room (the director's reading (1))
    const auto b = heaps.place(textures[1].Get(), requirements(64 * KiB, 4 * KiB));
    REQUIRE(b);
    CHECK(*b == HeapPlacement{0, ArenaRange{0, 64 * KiB}});
    CHECK(heaps.headroomRefusals() == 1u);
    CHECK(heaps.lastRefusal() == TextureHeaps::Refusal::None);
    room = true;
    const auto c = heaps.place(textures[2].Get(), requirements(96 * KiB, 4 * KiB));
    REQUIRE(c);
    CHECK(c->heap == 1u);
    CHECK(heaps.retire(textures[1].Get(), 7));
    CHECK(heaps.retire(textures[2].Get(), 7));
    heaps.releaseAll();
  }

  SECTION("without a device or the test mode no heap is made")
  {
    auto heaps = TextureHeaps{};
    const auto textures = fakeTextures(1);
    CHECK_FALSE(heaps.place(textures[0].Get(), requirements(4 * KiB, 4 * KiB)));
    CHECK(heaps.lastRefusal() == TextureHeaps::Refusal::NoHeap);
    CHECK(heaps.heaps() == 0u);
  }
}

} // namespace tb::lit
