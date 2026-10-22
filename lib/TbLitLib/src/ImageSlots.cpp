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

#include "ImageSlots.h"

#include "lit/ImageCache.h"

#include <algorithm>
#include <utility>

namespace tb::lit
{

std::string_view pendingIntrinsicName(const int usage)
{
  switch (usage)
  {
  case engine_image::UsageBump:
    return "_flat";
  case engine_image::UsageLight:
    return "_white";
  default:
    return "_default";
  }
}

MaterialImageKey intrinsicImageKey(const std::string_view name)
{
  auto key = MaterialImageKey{};
  key.name = std::string{name};
  key.usage = engine_image::UsageDefault;
  key.filter = engine_image::FilterDefault;
  key.repeat = engine_image::RepeatRepeat;
  key.cube = engine_image::Cube2D;
  key.cubeMapSize = 0;
  key.intrinsic = true;
  key.isDefault = name == "_default";
  return key;
}

std::vector<MaterialImageKey> lp2IntrinsicKeys()
{
  // D69: what LP2's passes bind, and the stand-ins of pending slots
  auto keys = std::vector<MaterialImageKey>{};
  for (const auto* name :
       {"_default",
        "_white",
        "_black",
        "_blackDiffuse",
        "_flat",
        "_noFalloff",
        "_quadratic"})
  {
    keys.push_back(intrinsicImageKey(name));
  }
  return keys;
}

void ImageSlots::setRequest(ImageRequest request)
{
  m_request = std::move(request);
}

ImageSlotId ImageSlots::request(const MaterialImageKey& key)
{
  if (const auto it = m_ids.find(key); it != m_ids.end())
  {
    return it->second;
  }
  // the stand-in first: an intrinsic, and the default image, have none
  const auto intrinsic = key.intrinsic || key.isDefault
                           ? NoImageSlot
                           : request(intrinsicImageKey(pendingIntrinsicName(key.usage)));
  const auto id = ImageSlotId(m_slots.size());
  auto slot = Slot{};
  slot.key = key;
  slot.intrinsic = intrinsic;
  m_slots.push_back(std::move(slot));
  m_ids.emplace(key, id);
  m_pending.insert(id);
  if (m_request)
  {
    if (const auto lookup = m_request(key); lookup.state != ImageLookup::State::Pending)
    {
      adopt(id, lookup, m_seenChanges);
    }
  }
  return id;
}

void ImageSlots::refresh(const MaterialTable& table, const std::uint64_t frame)
{
  // P2.14b: a slot whose files changed since it was answered is asked again
  if (table.imageChanges != m_seenChanges)
  {
    m_seenChanges = table.imageChanges;
    for (ImageSlotId id = 0; id < ImageSlotId(m_slots.size()); ++id)
    {
      const auto& slot = m_slots[id];
      if (slot.state == State::Pending)
      {
        continue;
      }
      if (const auto* change = table.changedImages.find(slot.key);
          change && *change > slot.adoptedChange)
      {
        reRequest(id, frame);
      }
    }
  }

  // only the pending slots are asked, and only for a new table
  if (table.version != m_tableVersion && m_request)
  {
    m_tableVersion = table.version;
    const auto pending = std::vector<ImageSlotId>{m_pending.begin(), m_pending.end()};
    for (const auto id : pending)
    {
      if (const auto lookup = m_request(m_slots[id].key);
          lookup.state != ImageLookup::State::Pending)
      {
        adopt(id, lookup, m_seenChanges);
      }
    }
  }
}

bool ImageSlots::anyArrivals(const MaterialTable& table) const
{
  return std::ranges::any_of(m_pending, [&](const ImageSlotId id) {
    return table.findImage(m_slots[id].key) != nullptr;
  });
}

void ImageSlots::attachTexture(
  const ImageSlotId id,
  nvrhi::TextureHandle texture,
  const int mipLevels,
  const std::uint64_t bytes)
{
  auto& slot = m_slots[id];
  slot.texture = std::move(texture);
  slot.mipLevels = mipLevels;
  slot.firstWrittenMip = mipLevels;
  slot.textureBytes = bytes;
}

void ImageSlots::levelsWritten(const ImageSlotId id, const int firstMip)
{
  auto& slot = m_slots[id];
  if (slot.texture && firstMip < slot.firstWrittenMip)
  {
    // P2.14c: the binding's range covers the written levels only, and grows
    slot.firstWrittenMip = std::max(firstMip, 0);
    bump(id);
  }
}

void ImageSlots::dropPixels(const ImageSlotId id)
{
  auto& slot = m_slots[id];
  slot.pixels.reset();
  if (slot.state == State::Loading)
  {
    slot.state = State::Resident;
  }
}

void ImageSlots::refuse(const ImageSlotId id, std::string reason)
{
  auto& slot = m_slots[id];
  m_pending.erase(id);
  // what was written stays bound until the slot is asked again; nothing more is written
  slot.pixels.reset();
  slot.state = State::Default;
  slot.reason = std::move(reason);
}

ImageSlotId ImageSlots::provideForTesting(
  const MaterialImageKey& key, std::shared_ptr<const CachedImage> image)
{
  const auto id = request(key);
  auto lookup = ImageLookup{};
  lookup.state = ImageLookup::State::Ready;
  lookup.image = std::move(image);
  adopt(id, lookup, m_seenChanges);
  return id;
}

void ImageSlots::dropTexture(const ImageSlotId id, const std::uint64_t frame)
{
  reRequest(id, frame);
}

ImageSlots::Binding ImageSlots::binding(const ImageSlotId id) const
{
  const auto written = [](const Slot& slot) {
    return slot.texture && slot.firstWrittenMip < slot.mipLevels;
  };
  const auto range = [](const Slot& slot) {
    return nvrhi::TextureSubresourceSet{
      nvrhi::MipLevel(slot.firstWrittenMip),
      nvrhi::MipLevel(slot.mipLevels - slot.firstWrittenMip),
      0,
      nvrhi::TextureSubresourceSet::AllArraySlices};
  };

  const auto& slot = m_slots[id];
  auto result = Binding{};
  result.versions.push_back(SlotVersion{id, slot.version});
  if (written(slot))
  {
    result.texture = slot.texture.Get();
    result.subresources = range(slot);
    return result;
  }
  if (slot.intrinsic == NoImageSlot)
  {
    return result;
  }
  // C28, P2.14d: the usage's intrinsic stands in; a change of either changes the set
  const auto& intrinsic = m_slots[slot.intrinsic];
  result.versions.push_back(SlotVersion{slot.intrinsic, intrinsic.version});
  result.intrinsic = true;
  if (written(intrinsic))
  {
    result.texture = intrinsic.texture.Get();
    result.subresources = range(intrinsic);
  }
  return result;
}

const ImageSlots::Slot* ImageSlots::slot(const ImageSlotId id) const
{
  return id < m_slots.size() ? &m_slots[id] : nullptr;
}

std::optional<ImageSlotId> ImageSlots::find(const MaterialImageKey& key) const
{
  const auto it = m_ids.find(key);
  return it != m_ids.end() ? std::optional{it->second} : std::nullopt;
}

std::size_t ImageSlots::size() const
{
  return m_slots.size();
}

std::size_t ImageSlots::pending() const
{
  return m_pending.size();
}

std::vector<ImageSlotId> ImageSlots::loading() const
{
  auto ids = std::vector<ImageSlotId>{};
  for (ImageSlotId id = 0; id < ImageSlotId(m_slots.size()); ++id)
  {
    if (m_slots[id].state == State::Loading)
    {
      ids.push_back(id);
    }
  }
  return ids;
}

bool ImageSlots::hasLoading() const
{
  return std::ranges::any_of(
    m_slots, [](const Slot& slot) { return slot.state == State::Loading; });
}

std::size_t ImageSlots::resident() const
{
  return std::size_t(std::ranges::count_if(m_slots, [](const Slot& slot) {
    return slot.texture && slot.firstWrittenMip < slot.mipLevels;
  }));
}

std::uint64_t ImageSlots::textureBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& slot : m_slots)
  {
    bytes += slot.texture ? slot.textureBytes : 0;
  }
  return bytes;
}

std::uint64_t ImageSlots::retiredBytes() const
{
  return m_retired.bytes();
}

std::vector<ImageSlotId> ImageSlots::takeChangedSlots()
{
  auto changed = std::exchange(m_changed, {});
  std::ranges::sort(changed);
  const auto [first, last] = std::ranges::unique(changed);
  changed.erase(first, last);
  return changed;
}

void ImageSlots::collectRetired(const std::uint64_t completedFrames)
{
  m_retired.collect(completedFrames);
}

void ImageSlots::releaseAll()
{
  m_retired.releaseAll();
  m_slots.clear();
  m_ids.clear();
  m_pending.clear();
  m_changed.clear();
}

void ImageSlots::adopt(
  const ImageSlotId id, const ImageLookup& lookup, const std::uint64_t change)
{
  auto& slot = m_slots[id];
  m_pending.erase(id);
  slot.adopted = lookup.slot;
  slot.adoptedChange = change;
  if (lookup.image)
  {
    // Ready, Source or Generated: the uploader makes the texture
    slot.pixels = lookup.image;
    slot.state = State::Loading;
    slot.reason.clear();
    return;
  }
  // Default (and an intrinsic the service does not make): the intrinsic stands in
  slot.state = State::Default;
  slot.reason = !lookup.slot                         ? std::string{"no image"}
                : !lookup.slot->sourceReason.empty() ? lookup.slot->sourceReason
                                                     : lookup.slot->reason;
}

void ImageSlots::bump(const ImageSlotId id)
{
  ++m_slots[id].version;
  m_changed.push_back(id);
}

void ImageSlots::reRequest(const ImageSlotId id, const std::uint64_t frame)
{
  auto& slot = m_slots[id];
  // D35: the texture is held until the frames that may bind it have finished
  m_retired.retire(slot.texture.Get(), slot.textureBytes, frame);
  slot.texture = nullptr;
  slot.mipLevels = 0;
  slot.firstWrittenMip = 0;
  slot.textureBytes = 0;
  slot.pixels.reset();
  slot.adopted.reset();
  slot.reason.clear();
  slot.state = State::Pending;
  bump(id);
  m_pending.insert(id);
  if (m_request)
  {
    if (const auto lookup = m_request(slot.key);
        lookup.state != ImageLookup::State::Pending)
    {
      adopt(id, lookup, m_seenChanges);
    }
  }
}

} // namespace tb::lit
