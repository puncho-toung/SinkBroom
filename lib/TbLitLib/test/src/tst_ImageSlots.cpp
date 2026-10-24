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

// LP2.1 (C28, D37, P2.14): the image-slot table and the binding-set cache on fake NVRHI
// handles, without a device.

#include "BindingSetCache.h"
#include "ImageSlots.h"
#include "LitTestUtils.h"
#include "lit/ImageCache.h"

#include <nvrhi/nvrhi.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace engine_image;

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

class FakeBindingSet : public nvrhi::RefCounter<nvrhi::IBindingSet>
{
public:
  const nvrhi::BindingSetDesc* getDesc() const override { return nullptr; }
  nvrhi::IBindingLayout* getLayout() const override { return nullptr; }
};

nvrhi::TextureHandle fakeTexture()
{
  return nvrhi::TextureHandle::Create(new FakeTexture{});
}

nvrhi::BindingSetHandle fakeSet()
{
  return nvrhi::BindingSetHandle::Create(new FakeBindingSet{});
}

MaterialImageKey keyOf(const std::string& name, const int usage)
{
  auto key = MaterialImageKey{};
  key.name = name;
  key.usage = usage;
  key.filter = FilterDefault;
  return key;
}

// a material table's images as the fake service answers them
struct FakeService
{
  MaterialTable table;
  std::map<std::string, std::shared_ptr<const ImageCacheSlot>> slots;
  int asked = 0;

  void publish(
    const MaterialImageKey& key, const bool withImage, const std::string& why = {})
  {
    auto slot = std::make_shared<ImageCacheSlot>();
    if (withImage)
    {
      auto image = std::make_shared<CachedImage>();
      image->width = 4;
      image->height = 4;
      image->numLevels = 3;
      slot->state = ImageCacheSlot::State::Ready;
      slot->image = image;
    }
    slot->sourceReason = why;
    table.images = table.images.set(key, slot);
    ++table.version;
  }

  void change(const MaterialImageKey& key)
  {
    table.images = table.images.erase(key);
    ++table.imageChanges;
    table.changedImages = table.changedImages.set(key, table.imageChanges);
    ++table.version;
  }

  ImageRequest request()
  {
    return [this](const MaterialImageKey& key) {
      ++asked;
      const auto slot = table.findImage(key);
      if (!slot)
      {
        return ImageLookup{};
      }
      return ImageLookup{
        slot->image ? ImageLookup::State::Ready : ImageLookup::State::Default,
        slot->image,
        slot};
    };
  }
};

} // namespace

TEST_CASE("ImageSlots")
{
  SECTION("the intrinsic a pending slot binds, by usage (P2.14d)")
  {
    CHECK(pendingIntrinsicName(UsageBump) == "_flat");
    CHECK(pendingIntrinsicName(UsageLight) == "_white");
    CHECK(pendingIntrinsicName(UsageDiffuse) == "_default");
    CHECK(pendingIntrinsicName(UsageSpecular) == "_default");
    CHECK(pendingIntrinsicName(UsageDefault) == "_default");
    CHECK(pendingIntrinsicName(UsageCoverage) == "_default");

    // the shim's record of an intrinsic: TD_DEFAULT, TF_DEFAULT, TR_REPEAT
    const auto flat = intrinsicImageKey("_flat");
    CHECK(flat.intrinsic);
    CHECK_FALSE(flat.isDefault);
    CHECK(flat.usage == UsageDefault);
    CHECK(flat.filter == FilterDefault);
    CHECK(flat.repeat == RepeatRepeat);
    CHECK(intrinsicImageKey("_default").isDefault);
    CHECK(lp2IntrinsicKeys().size() == 7u);
  }

  SECTION("a slot is Pending, binds its intrinsic, and its arrival changes its binding")
  {
    auto service = FakeService{};
    auto slots = ImageSlots{};
    slots.setRequest(service.request());

    const auto bumpKey = keyOf("textures/a_local", UsageBump);
    const auto bump = slots.request(bumpKey);
    // the request made its intrinsic's slot first, and asked for both
    REQUIRE(slots.size() == 2u);
    CHECK(slots.pending() == 2u);
    CHECK(service.asked == 2);
    const auto flat = *slots.find(intrinsicImageKey("_flat"));
    CHECK(slots.slot(bump)->intrinsic == flat);
    CHECK(slots.slot(flat)->intrinsic == NoImageSlot);
    CHECK(slots.request(bumpKey) == bump);

    // nothing written: the binding names both, and has no texture
    auto binding = slots.binding(bump);
    CHECK_FALSE(binding.texture);
    CHECK(binding.intrinsic);
    CHECK(
      binding.versions
      == std::vector<SlotVersion>{SlotVersion{bump, 1}, SlotVersion{flat, 1}});

    // the intrinsic arrives and is written: the pending slot binds it
    service.publish(intrinsicImageKey("_flat"), true);
    slots.refresh(service.table, 1);
    CHECK(slots.slot(flat)->state == ImageSlots::State::Loading);
    CHECK(slots.slot(bump)->state == ImageSlots::State::Pending);
    const auto flatTexture = fakeTexture();
    slots.attachTexture(flat, flatTexture, 3, 96);
    slots.levelsWritten(flat, 0);
    slots.dropPixels(flat);
    CHECK(slots.slot(flat)->state == ImageSlots::State::Resident);
    CHECK_FALSE(slots.slot(flat)->pixels);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{flat});
    binding = slots.binding(bump);
    CHECK(binding.texture == flatTexture.Get());
    CHECK(binding.subresources.baseMipLevel == 0u);
    CHECK(binding.subresources.numMipLevels == 3u);
    CHECK(
      binding.versions
      == std::vector<SlotVersion>{SlotVersion{bump, 1}, SlotVersion{flat, 2}});

    // the slot's own image: low mips first, each extension of the written range bumps it
    // and the binding never reaches an unwritten level (P2.14c)
    service.publish(bumpKey, true);
    const auto asked = service.asked;
    slots.refresh(service.table, 2);
    // a new table asks only the pending slot
    CHECK(service.asked == asked + 1);
    REQUIRE(slots.slot(bump)->state == ImageSlots::State::Loading);
    const auto texture = fakeTexture();
    slots.attachTexture(bump, texture, 3, 84);
    CHECK(slots.binding(bump).texture == flatTexture.Get());
    slots.levelsWritten(bump, 2);
    binding = slots.binding(bump);
    CHECK(binding.texture == texture.Get());
    CHECK(binding.subresources.baseMipLevel == 2u);
    CHECK(binding.subresources.numMipLevels == 1u);
    CHECK(binding.versions == std::vector<SlotVersion>{SlotVersion{bump, 2}});
    slots.levelsWritten(bump, 1);
    slots.levelsWritten(bump, 1);
    binding = slots.binding(bump);
    CHECK(binding.subresources.baseMipLevel == 1u);
    CHECK(binding.subresources.numMipLevels == 2u);
    slots.levelsWritten(bump, 0);
    CHECK(slots.binding(bump).subresources.numMipLevels == 3u);
    CHECK(slots.slot(bump)->version == 4u);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{bump});
    CHECK(slots.resident() == 2u);
    CHECK(slots.textureBytes() == 180u);
  }

  SECTION("a Default answer binds the intrinsic for good, with the reason")
  {
    auto service = FakeService{};
    const auto key = keyOf("lights/missing", UsageLight);
    service.publish(key, false, "Couldn't load image: lights/missing");
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    CHECK(slots.slot(id)->state == ImageSlots::State::Default);
    CHECK(slots.slot(id)->reason == "Couldn't load image: lights/missing");
    CHECK(slots.slot(slots.slot(id)->intrinsic)->key.name == "_white");
    CHECK(slots.pending() == 1u);
  }

  SECTION("the service handshake: a changed file asks again, the budget's drop does not")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/b", UsageDiffuse);
    service.publish(key, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Loading);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    static_cast<void>(slots.takeChangedSlots());
    const auto version = slots.slot(id)->version;

    // the budget drops the image's bytes from the table (S2.4): no change, the texture
    // stays
    service.table.images = service.table.images.erase(key);
    ++service.table.version;
    slots.refresh(service.table, 5);
    CHECK(slots.slot(id)->texture);
    CHECK(slots.slot(id)->version == version);

    // its file changes (a re-probe): asked again, the texture let go, the version bumped,
    // the intrinsic bound until the new one is written
    service.change(key);
    slots.refresh(service.table, 6);
    CHECK(slots.slot(id)->state == ImageSlots::State::Pending);
    CHECK_FALSE(slots.slot(id)->texture);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.binding(id).intrinsic);
    CHECK(slots.retiredBytes() == 84u);
    // the texture is held until frame 6 has finished
    slots.collectRetired(6);
    CHECK(slots.retiredBytes() == 84u);
    slots.collectRetired(7);
    CHECK(slots.retiredBytes() == 0u);

    // the service reads it again
    service.publish(key, true);
    slots.refresh(service.table, 7);
    CHECK(slots.slot(id)->state == ImageSlots::State::Loading);
    // an old change is not seen again
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    ++service.table.version;
    slots.refresh(service.table, 8);
    CHECK(slots.slot(id)->texture);
  }

  SECTION("U2.1: an adopted image's bytes go once written and the service let them go")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/d", UsageSpecular);
    service.publish(key, true);
    // the record's usage after the PBR rename, and the image's format, as the service
    // answers them
    {
      auto slot = std::make_shared<ImageCacheSlot>(*service.table.findImage(key));
      slot->name.usage = UsageSpecularPbrRmao;
      auto image = std::make_shared<CachedImage>(*slot->image);
      image->format = FormatDxt5;
      slot->image = image;
      service.table.images = service.table.images.set(key, slot);
      ++service.table.version;
    }
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Loading);
    CHECK(slots.slot(id)->adoptedUsage == UsageSpecularPbrRmao);
    CHECK(slots.slot(id)->adoptedFormat == FormatDxt5);
    const auto weak = std::weak_ptr<const CachedImage>{slots.slot(id)->pixels};

    // the service's budget drops the taken image (S2.4): the slot still holds the bytes
    // it has to write
    service.table.images = service.table.images.erase(key);
    ++service.table.version;
    slots.refresh(service.table, 1);
    CHECK_FALSE(weak.expired());

    // written: nothing in the process holds the bytes any more; the facts stay
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(weak.expired());
    CHECK(slots.slot(id)->adoptedUsage == UsageSpecularPbrRmao);
    CHECK(slots.slot(id)->adoptedFormat == FormatDxt5);
  }

  SECTION("U2.2: a collect that keeps some retired textures subtracts the others' bytes")
  {
    auto slots = ImageSlots{};
    const auto a = slots.provideForTesting(keyOf("textures/e", UsageDiffuse), {});
    const auto b = slots.provideForTesting(keyOf("textures/f", UsageDiffuse), {});
    slots.attachTexture(a, fakeTexture(), 1, 100);
    slots.attachTexture(b, fakeTexture(), 1, 7);
    // a retired in frame 4, b in frame 5
    slots.dropTexture(a, 4);
    slots.dropTexture(b, 5);
    CHECK(slots.retiredBytes() == 107u);
    // frame 4 has finished, frame 5 not: a's bytes go, b's stay
    slots.collectRetired(5);
    CHECK(slots.retiredBytes() == 7u);
    slots.collectRetired(6);
    CHECK(slots.retiredBytes() == 0u);
  }

  SECTION("a dropped texture is asked for again and binds the intrinsic meanwhile")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/c", UsageDiffuse);
    service.publish(key, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    static_cast<void>(slots.takeChangedSlots());

    slots.dropTexture(id, 3);
    // the service still has it: the bytes are back at once, to be written again
    CHECK(slots.slot(id)->state == ImageSlots::State::Loading);
    CHECK(slots.slot(id)->pixels);
    CHECK(slots.binding(id).intrinsic);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
  }

  SECTION("LP4.3 (D79, P4.6): the state version moves on every state change, only then")
  {
    auto service = FakeService{};
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto key = keyOf("env/maps/m/area0_lightgrid_amb", UsageDiffuse);
    const auto start = slots.stateVersion();
    // a new slot (and its intrinsic's) is Pending: no change
    const auto id = slots.request(key);
    CHECK(slots.stateVersion() == start);
    // an answer: Pending to Loading
    service.publish(key, true);
    slots.refresh(service.table, 1);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Loading);
    const auto loading = slots.stateVersion();
    CHECK(loading > start);
    // a written level bumps the slot's version, not its state's
    slots.attachTexture(id, fakeTexture(), 1, 16);
    slots.levelsWritten(id, 0);
    CHECK(slots.stateVersion() == loading);
    // every level written: Resident
    slots.dropPixels(id);
    const auto resident = slots.stateVersion();
    CHECK(resident > loading);
    // a re-request: Pending again, then the service's answer at once
    slots.dropTexture(id, 2);
    CHECK(slots.stateVersion() > resident);
    // a refusal: Default
    const auto beforeRefusal = slots.stateVersion();
    slots.refuse(id, "a format the engine refuses");
    CHECK(slots.slot(id)->state == ImageSlots::State::Default);
    CHECK(slots.stateVersion() > beforeRefusal);
    // a Default answer of a new slot
    const auto missing = keyOf("env/maps/m/area1_lightgrid_amb", UsageDiffuse);
    service.publish(missing, false, "no image");
    const auto beforeMissing = slots.stateVersion();
    static_cast<void>(slots.request(missing));
    CHECK(slots.stateVersion() > beforeMissing);
  }
}

TEST_CASE("BindingSetCache")
{
  auto layout = int{0};
  auto otherLayout = int{0};
  const auto keyOf =
    [&](std::vector<SlotVersion> slots, const std::uint64_t generation = 0) {
      auto key = BindingSetKey{};
      key.layout = reinterpret_cast<nvrhi::IBindingLayout*>(&layout);
      key.slots = std::move(slots);
      key.samplers = 1;
      key.renderTargetGeneration = generation;
      return key;
    };
  auto made = 0;
  const auto make = [&]() {
    ++made;
    return fakeSet();
  };

  SECTION("keys: a set is made once per key, and every field tells keys apart")
  {
    auto cache = BindingSetCache{16};
    const auto a = cache.get(keyOf({{0, 1}, {1, 1}}), 1, make);
    CHECK(cache.get(keyOf({{0, 1}, {1, 1}}), 1, make) == a);
    CHECK(made == 1);
    CHECK(cache.get(keyOf({{0, 2}, {1, 1}}), 1, make) != a);
    CHECK(cache.get(keyOf({{1, 1}, {0, 1}}), 1, make) != a);
    auto key = keyOf({{0, 1}, {1, 1}});
    key.layout = reinterpret_cast<nvrhi::IBindingLayout*>(&otherLayout);
    CHECK(cache.get(key, 1, make) != a);
    key = keyOf({{0, 1}, {1, 1}});
    key.samplers = 2;
    CHECK(cache.get(key, 1, make) != a);
    key = keyOf({{0, 1}, {1, 1}});
    key.resources.push_back(&layout);
    CHECK(cache.get(key, 1, make) != a);
    CHECK(made == 6);
    CHECK(cache.size() == 6u);
    // a make that fails keeps nothing
    CHECK_FALSE(cache.get(keyOf({{9, 1}}), 1, [] { return nvrhi::BindingSetHandle{}; }));
    CHECK(cache.size() == 6u);
  }

  SECTION("the reverse index: a slot's bump erases exactly the sets that hold it")
  {
    auto cache = BindingSetCache{16};
    cache.get(keyOf({{0, 1}, {1, 1}}), 1, make);
    cache.get(keyOf({{0, 1}, {2, 1}}), 1, make);
    cache.get(keyOf({{2, 1}}), 1, make);
    cache.get(keyOf({{3, 1}}), 1, make);
    CHECK(cache.setsOf(0) == 2u);
    CHECK(cache.setsOf(2) == 2u);

    CHECK(cache.slotChanged(0) == 2u);
    CHECK(cache.size() == 2u);
    CHECK(cache.setsOf(0) == 0u);
    CHECK(cache.setsOf(1) == 0u);
    CHECK(cache.setsOf(2) == 1u);
    CHECK(cache.setsOf(3) == 1u);
    CHECK(cache.slotChanged(7) == 0u);
    CHECK(cache.slotChanged(2) == 1u);
    CHECK(cache.size() == 1u);

    // the next frame makes the set again, with the new version
    made = 0;
    cache.get(keyOf({{0, 2}, {1, 1}}), 2, make);
    CHECK(made == 1);
  }

  SECTION("the render-target generation erases only the sets that bind a target")
  {
    auto cache = BindingSetCache{16};
    cache.get(keyOf({{0, 1}}), 1, make);
    cache.get(keyOf({{1, 1}}, 1), 1, make);
    cache.get(keyOf({{2, 1}}, 1), 1, make);
    CHECK(cache.renderTargetsChanged(1) == 0u);
    CHECK(cache.renderTargetsChanged(2) == 2u);
    CHECK(cache.size() == 1u);
    CHECK(cache.setsOf(1) == 0u);
    CHECK(cache.setsOf(0) == 1u);
  }

  SECTION("the LRU bound: the least recently used go, never one of this frame")
  {
    auto cache = BindingSetCache{4};
    for (std::uint32_t i = 0; i < 8; ++i)
    {
      cache.get(keyOf({{i, 1}}), i, make);
    }
    // slot 1's set used again in frame 9
    cache.get(keyOf({{1, 1}}), 9, make);
    CHECK(cache.size() == 8u);
    CHECK(cache.trim(9) == 4u);
    CHECK(cache.size() == 4u);
    // the oldest went: 0, 2, 3, 4; 1 (frame 9), 5, 6, 7 stay
    for (const auto [slot, kept] : std::vector<std::pair<std::uint32_t, bool>>{
           {0, false},
           {1, true},
           {2, false},
           {3, false},
           {4, false},
           {5, true},
           {6, true},
           {7, true}})
    {
      CAPTURE(slot);
      CHECK((cache.setsOf(slot) == 1u) == kept);
    }

    // sets of the frame being trimmed are never released, even beyond the bound
    auto busy = BindingSetCache{2};
    for (std::uint32_t i = 0; i < 5; ++i)
    {
      busy.get(keyOf({{i, 1}}), 3, make);
    }
    CHECK(busy.trim(3) == 0u);
    CHECK(busy.trim(4) == 3u);
    CHECK(busy.size() == 2u);
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
// U2.4: 100 cycles of what the render thread does to these tables per frame and per
// change leave the CRT's debug heap as two warm-up cycles left it
TEST_CASE("ImageSlots and BindingSetCache: 100 cycles on the debug heap (U2.4)")
{
  SECTION("the slot table: request, adopt, levels, re-request, collect")
  {
    auto service = FakeService{};
    auto keys = std::vector<MaterialImageKey>{};
    for (auto i = 0; i < 8; ++i)
    {
      keys.push_back(
        keyOf("textures/heap" + std::to_string(i), i % 2 ? UsageBump : UsageDiffuse));
      service.publish(keys.back(), true);
    }
    service.publish(intrinsicImageKey("_flat"), true);
    service.publish(intrinsicImageKey("_default"), true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    auto frame = std::uint64_t{0};
    const auto cycle = [&] {
      for (const auto& key : keys)
      {
        const auto id = slots.request(key);
        if (slots.slot(id)->state == ImageSlots::State::Loading)
        {
          slots.attachTexture(id, fakeTexture(), 3, 84);
          slots.levelsWritten(id, 2);
          slots.levelsWritten(id, 0);
          slots.dropPixels(id);
        }
      }
      ++frame;
      for (const auto& key : keys)
      {
        slots.dropTexture(*slots.find(key), frame);
      }
      static_cast<void>(slots.takeChangedSlots());
      ++service.table.version;
      slots.refresh(service.table, frame);
      slots.collectRetired(frame + 1);
    };
    cycle();
    cycle();
    CHECK_FALSE(heapGrows([&] {
      for (auto i = 0; i < 100; ++i)
      {
        cycle();
      }
    }));
    CHECK(slots.size() == 10u);
    CHECK(slots.retiredBytes() == 0u);
  }

  SECTION("the set cache and its reverse index: get, slotChanged, the generation, trim")
  {
    auto layout = int{0};
    auto cache = BindingSetCache{8};
    const auto make = [] { return fakeSet(); };
    auto frame = std::uint64_t{0};
    const auto cycle = [&] {
      ++frame;
      for (std::uint32_t slot = 0; slot < 12; ++slot)
      {
        auto key = BindingSetKey{};
        key.layout = reinterpret_cast<nvrhi::IBindingLayout*>(&layout);
        key.slots = {SlotVersion{slot, frame}, SlotVersion{slot + 12, 1}};
        cache.get(key, frame, make);
      }
      auto target = BindingSetKey{};
      target.layout = reinterpret_cast<nvrhi::IBindingLayout*>(&layout);
      target.renderTargetGeneration = frame;
      cache.get(target, frame, make);
      for (std::uint32_t slot = 0; slot < 6; ++slot)
      {
        cache.slotChanged(slot);
      }
      cache.renderTargetsChanged(frame + 1);
      cache.trim(frame + 1);
    };
    cycle();
    cycle();
    CHECK_FALSE(heapGrows([&] {
      for (auto i = 0; i < 100; ++i)
      {
        cycle();
      }
    }));
    CHECK(cache.size() <= cache.capacity());
  }
}
#endif

} // namespace tb::lit
