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
  // LP6.4b: the asks per image name
  std::map<std::string, int> askedFor;

  void publish(
    const MaterialImageKey& key,
    const bool withImage,
    const std::string& why = {},
    const bool fed = false)
  {
    auto slot = std::make_shared<ImageCacheSlot>();
    // LP6.4b: an override's image (fed)
    slot->override = fed;
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
      ++askedFor[key.name];
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
      == InlineVector<SlotVersion, 2>{SlotVersion{bump, 1}, SlotVersion{flat, 1}});

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
      == InlineVector<SlotVersion, 2>{SlotVersion{bump, 1}, SlotVersion{flat, 2}});

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
    CHECK(binding.versions == InlineVector<SlotVersion, 2>{SlotVersion{bump, 2}});
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

    // its file changes (a re-probe): asked again; LPF.1 (C40, D98): the complete texture
    // stays bound, with the slot's version, while the service has no answer (Replacing)
    const auto* old = slots.slot(id)->texture.Get();
    service.change(key);
    slots.refresh(service.table, 6);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    // asked for, as its intrinsic (_default, never published here) is
    CHECK(slots.pending() == 2u);
    CHECK(slots.binding(id).texture == old);
    CHECK_FALSE(slots.binding(id).intrinsic);
    CHECK(slots.slot(id)->version == version);
    CHECK(slots.takeChangedSlots().empty());
    CHECK(slots.retiredBytes() == 0u);

    // the service reads it again: written, then swapped in, the old texture held until
    // frame 7 has finished
    service.publish(key, true);
    slots.refresh(service.table, 7);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    CHECK(slots.loading() == std::vector<ImageSlotId>{id});
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.binding(id).texture != old);
    CHECK(slots.retiredBytes() == 84u);
    slots.collectRetired(7);
    CHECK(slots.retiredBytes() == 84u);
    slots.collectRetired(8);
    CHECK(slots.retiredBytes() == 0u);
    // an old change is not seen again
    ++service.table.version;
    slots.refresh(service.table, 8);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(slots.slot(id)->texture);

    // a file that is gone: the replaced texture goes too, the intrinsic stands in
    service.change(key);
    service.publish(key, false, "gone");
    slots.refresh(service.table, 9);
    CHECK(slots.slot(id)->state == ImageSlots::State::Default);
    CHECK(slots.binding(id).intrinsic);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.retiredBytes() == 84u);
  }

  SECTION("LPF.1 (C40, D98, D102): Replacing keeps the complete texture bound")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/override", UsageDiffuse);
    service.publish(intrinsicImageKey("_default"), true);
    service.publish(key, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    const auto intrinsic = slots.slot(id)->intrinsic;
    for (const auto slot : {intrinsic, id})
    {
      REQUIRE(slots.slot(slot)->state == ImageSlots::State::Loading);
      slots.attachTexture(slot, fakeTexture(), 3, 84);
      slots.levelsWritten(slot, 0);
      slots.dropPixels(slot);
    }
    // Loading to Resident
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Resident);
    static_cast<void>(slots.takeChangedSlots());
    const auto first = slots.slot(id)->texture;
    const auto version = slots.slot(id)->version;
    const auto bound = [&](const nvrhi::ITexture* texture) {
      const auto binding = slots.binding(id);
      return binding.texture == texture && !binding.intrinsic
             && binding.versions == InlineVector<SlotVersion, 2>{SlotVersion{id, version}}
             && binding.subresources.baseMipLevel == 0u
             && binding.subresources.numMipLevels == 3u;
    };

    // Resident to Replacing: a second image in the table that drops the first (one
    // publication): the first stays bound while the second is made and written
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 4);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    CHECK(slots.replacing() == 1u);
    CHECK(slots.pending() == 0u);
    CHECK(slots.hasLoading());
    CHECK(slots.loading() == std::vector<ImageSlotId>{id});
    CHECK(bound(first.Get()));
    CHECK(slots.takeChangedSlots().empty());
    const auto second = fakeTexture();
    slots.attachTexture(id, second, 3, 84);
    slots.levelsWritten(id, 2);
    slots.levelsWritten(id, 1);
    CHECK(bound(first.Get()));
    CHECK(slots.takeChangedSlots().empty());
    // both textures are held, and the slot counts once
    CHECK(slots.textureBytes() == 3u * 84u);
    CHECK(slots.resident() == 2u);

    // Replacing to Replacing: a third image before the second landed: the unfinished
    // second goes (held for its frames), the first still bound
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 5);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    CHECK(bound(first.Get()));
    CHECK_FALSE(slots.slot(id)->texture);
    CHECK(slots.slot(id)->pixels);
    CHECK(slots.retiredBytes() == 84u);
    CHECK(slots.takeChangedSlots().empty());

    // D102: the headroom refuses the third image's texture: the first stays bound, the
    // image kept, said once until a texture is made
    CHECK(slots.waitForRoom(id));
    CHECK_FALSE(slots.waitForRoom(id));
    CHECK(slots.slot(id)->pixels);
    CHECK(bound(first.Get()));

    // the frame that writes its last level swaps it in: Replacing to Resident
    const auto third = fakeTexture();
    slots.attachTexture(id, third, 3, 84);
    CHECK_FALSE(slots.slot(id)->waitsForRoom);
    slots.levelsWritten(id, 0);
    CHECK(bound(first.Get()));
    const auto states = slots.stateVersion();
    slots.dropPixels(id);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(slots.stateVersion() > states);
    CHECK(slots.replacing() == 0u);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.binding(id).texture == third.Get());
    CHECK_FALSE(slots.slot(id)->replaced);
    // the first is held until frame 5 has finished, as the second
    CHECK(slots.retiredBytes() == 2u * 84u);
    slots.collectRetired(6);
    CHECK(slots.retiredBytes() == 0u);
    CHECK(slots.textureBytes() == 2u * 84u);

    // a refusal while Replacing binds the complete texture again
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 7);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Replacing);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 2);
    slots.refuse(id, "a format the engine refuses");
    CHECK(slots.slot(id)->state == ImageSlots::State::Default);
    CHECK(slots.binding(id).texture == third.Get());
    CHECK_FALSE(slots.binding(id).intrinsic);
    CHECK(slots.retiredBytes() == 84u);
    CHECK_FALSE(slots.slot(id)->replaced);

    // a Default slot is not complete: asked again as before (Pending)
    service.change(key);
    slots.refresh(service.table, 8);
    CHECK(slots.slot(id)->state == ImageSlots::State::Pending);
    service.publish(key, true);
    slots.refresh(service.table, 9);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);

    // the holder is released on a drop: both textures let go, the intrinsic stands in
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 10);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Replacing);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.collectRetired(20);
    slots.dropTexture(id, 10);
    CHECK_FALSE(slots.slot(id)->replaced);
    CHECK_FALSE(slots.slot(id)->texture);
    CHECK(slots.binding(id).intrinsic);
    CHECK(slots.retiredBytes() == 2u * 84u);

    // and on teardown
    slots.refresh(service.table, 11);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 12);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Replacing);
    slots.releaseAll();
    CHECK(slots.size() == 0u);
    CHECK(slots.retiredBytes() == 0u);
    CHECK(slots.textureBytes() == 0u);
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

  SECTION("LP6.4b (C68 (b), D172): Evicted binds its intrinsic, asked again when drawn")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/e", UsageDiffuse);
    service.publish(key, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    REQUIRE(slots.slot(id)->state == ImageSlots::State::Resident);
    static_cast<void>(slots.takeChangedSlots());
    const auto version = slots.slot(id)->version;
    const auto asked = service.askedFor[key.name];
    const auto pending = slots.pending();
    const auto states = slots.stateVersion();

    // an intrinsic, and a slot without a complete texture, are never evicted
    CHECK(slots.evict(slots.slot(id)->intrinsic, 5) == 0u);
    const auto loadingKey = keyOf("textures/e_loading", UsageDiffuse);
    service.publish(loadingKey, true);
    const auto loading = slots.request(loadingKey);
    CHECK(slots.evict(loading, 5) == 0u);
    static_cast<void>(slots.takeChangedSlots());

    // evicted in frame 5: its texture retired with the frame, its intrinsic bound, its
    // version bumped, not pending, the image's facts kept
    CHECK(slots.evict(id, 5) == 84u);
    CHECK(slots.slot(id)->state == ImageSlots::State::Evicted);
    CHECK_FALSE(slots.slot(id)->texture);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.binding(id).intrinsic);
    CHECK(slots.pending() == pending);
    CHECK(slots.resident() == 0u);
    CHECK(slots.retiredBytes() == 84u);
    CHECK(slots.stateVersion() > states);
    CHECK(slots.evicted() == 1u);
    CHECK(slots.evictedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.evictions() == 1u);
    CHECK(slots.slot(id)->adoptedUsage);
    CHECK(slots.evict(id, 6) == 0u);

    // a new table, a changed file: never asked (the director's reading (3))
    ++service.table.version;
    slots.refresh(service.table, 6);
    service.change(key);
    service.publish(key, true);
    slots.refresh(service.table, 7);
    CHECK(service.askedFor[key.name] == asked);
    CHECK(slots.slot(id)->state == ImageSlots::State::Evicted);

    // a draw asked for it by its state: asked again at the frame's end, as a first
    // request (Loading: the service has its image)
    slots.want(loading);
    CHECK_FALSE(slots.wanted(loading));
    slots.want(id);
    CHECK(slots.wanted(id));
    slots.reRequestEvicted(id, 8);
    CHECK(service.askedFor[key.name] == asked + 1);
    CHECK(slots.slot(id)->state == ImageSlots::State::Loading);
    CHECK(slots.slot(id)->pixels);
    CHECK(slots.evicted() == 0u);
    CHECK_FALSE(slots.wanted(id));
    slots.reRequestEvicted(id, 8);
    CHECK(service.askedFor[key.name] == asked + 1);
    slots.collectRetired(6);
    CHECK(slots.retiredBytes() == 0u);
  }

  SECTION("LP6.4b (C68 (c), D173): a fed slot's equal image is written in place")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/fed", UsageDiffuse);
    service.publish(key, true, {}, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    auto fits = true;
    slots.setInPlaceRule([&](const CachedImage&, const nvrhi::ITexture&) { return fits; });
    const auto id = slots.request(key);
    CHECK(slots.slot(id)->adoptedOverride);
    const auto texture = fakeTexture();
    slots.attachTexture(id, texture, 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    static_cast<void>(slots.takeChangedSlots());
    const auto version = slots.slot(id)->version;

    // fed again, the rule passes: Resident kept, its texture bound, the image held for
    // the write, no bump until it is written
    service.change(key);
    service.publish(key, true, {}, true);
    slots.refresh(service.table, 4);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(slots.slot(id)->writeInPlace);
    CHECK(slots.slot(id)->pixels);
    CHECK(slots.slot(id)->texture == texture);
    CHECK(slots.slot(id)->version == version);
    CHECK(slots.inPlace() == std::vector<ImageSlotId>{id});
    CHECK(slots.loading().empty());
    CHECK(slots.hasLoading());
    CHECK(slots.takeChangedSlots().empty());
    slots.inPlaceWritten(id);
    CHECK_FALSE(slots.slot(id)->writeInPlace);
    CHECK_FALSE(slots.slot(id)->pixels);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.takeChangedSlots() == std::vector<ImageSlotId>{id});
    CHECK(slots.inPlaceWrites() == 1u);
    CHECK_FALSE(slots.hasLoading());

    // the rule fails (another desc, or over the budget): Replacing as before
    fits = false;
    service.change(key);
    service.publish(key, true, {}, true);
    slots.refresh(service.table, 6);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    CHECK_FALSE(slots.slot(id)->writeInPlace);
    CHECK(slots.slot(id)->replaced == texture);

    // a file's image (no override) is never written in place
    const auto fileKey = keyOf("textures/file", UsageDiffuse);
    service.publish(fileKey, true);
    const auto file = slots.request(fileKey);
    slots.attachTexture(file, fakeTexture(), 3, 84);
    slots.levelsWritten(file, 0);
    slots.dropPixels(file);
    fits = true;
    service.change(fileKey);
    service.publish(fileKey, true);
    slots.refresh(service.table, 8);
    CHECK(slots.slot(file)->state == ImageSlots::State::Replacing);
  }

  SECTION("LP6.4b (C68 (b)): the mip floor through Replacing, the complete one bound")
  {
    auto service = FakeService{};
    const auto key = keyOf("textures/f", UsageDiffuse);
    service.publish(key, true);
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    const auto id = slots.request(key);
    slots.attachTexture(id, fakeTexture(), 3, 84);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    static_cast<void>(slots.takeChangedSlots());
    const auto version = slots.slot(id)->version;

    CHECK(slots.floor(id, 4));
    CHECK(slots.slot(id)->floored);
    CHECK(slots.slot(id)->state == ImageSlots::State::Replacing);
    CHECK(slots.slot(id)->version == version);
    CHECK_FALSE(slots.binding(id).intrinsic);
    CHECK(slots.binding(id).texture);
    CHECK(slots.floored() == 1u);
    CHECK(slots.floors() == 1u);
    CHECK(slots.flooringBytes() == 84u);
    CHECK_FALSE(slots.floor(id, 4));

    // the floor texture written: the swap, the floor mark kept
    slots.attachTexture(id, fakeTexture(), 1, 16);
    slots.levelsWritten(id, 0);
    slots.dropPixels(id);
    CHECK(slots.slot(id)->state == ImageSlots::State::Resident);
    CHECK(slots.slot(id)->floored);
    CHECK(slots.slot(id)->version == version + 1);
    CHECK(slots.flooringBytes() == 0u);
    CHECK(slots.floored() == 1u);
    CHECK_FALSE(slots.floor(id, 5));

    // the eviction clears it: a later request makes the whole texture again
    CHECK(slots.evict(id, 9) == 16u);
    CHECK_FALSE(slots.slot(id)->floored);
    CHECK(slots.floored() == 0u);
  }

  SECTION("LP6.1 (C64, P8.1): an answer's adopted usage is reported, a request's not")
  {
    auto service = FakeService{};
    auto slots = ImageSlots{};
    slots.setRequest(service.request());
    // answered at its request: not a change (nothing has drawn the slot yet)
    const auto readyKey = keyOf("textures/u_ready", UsageSpecular);
    service.publish(readyKey, true);
    const auto ready = slots.request(readyKey);
    CHECK(slots.slot(ready)->adoptedUsage);
    // pending at its request, answered by a later table: a change without a version bump,
    // which the draw records read (the program follows the usage) and the cache does not
    const auto laterKey = keyOf("textures/u_later", UsageSpecular);
    const auto later = slots.request(laterKey);
    CHECK_FALSE(slots.slot(later)->adoptedUsage);
    static_cast<void>(slots.takeChangedSlots());
    CHECK(slots.takeUsageChangedSlots().empty());
    const auto version = slots.slot(later)->version;
    service.publish(laterKey, true);
    slots.refresh(service.table, 1);
    CHECK(slots.slot(later)->adoptedUsage);
    CHECK(slots.slot(later)->version == version);
    CHECK(slots.usageChangedSlots() == std::vector<ImageSlotId>{later});
    CHECK(slots.takeUsageChangedSlots() == std::vector<ImageSlotId>{later});
    CHECK(slots.takeUsageChangedSlots().empty());
    CHECK(slots.takeChangedSlots().empty());
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
      key.slots.append(slots.begin(), slots.end());
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

  SECTION("LP6.4b (D172, A54): a slot's last-bound frame is the max lastUsed of its sets")
  {
    auto cache = BindingSetCache{16};
    CHECK_FALSE(cache.lastBound(0));
    cache.get(keyOf({{0, 1}, {1, 1}}), 3, make);
    cache.get(keyOf({{0, 1}}), 5, make);
    CHECK(cache.lastBound(0) == 5u);
    CHECK(cache.lastBound(1) == 3u);
    // a record's per-frame stamp moves it through the entry it holds
    auto* entry = cache.hold(keyOf({{1, 1}}), 4, make);
    REQUIRE(entry);
    CHECK(cache.lastBound(1) == 4u);
    entry->lastUsed = 9;
    CHECK(cache.lastBound(1) == 9u);
    BindingSetCache::release(*entry);
    // the slot's sets go with its bump: none holds it any more
    cache.slotChanged(0);
    CHECK_FALSE(cache.lastBound(0));
    CHECK(cache.lastBound(1) == 9u);
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
      // LP6.1 (C64): and the adopted usages' changes, which the render thread takes too
      static_cast<void>(slots.takeUsageChangedSlots());
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
