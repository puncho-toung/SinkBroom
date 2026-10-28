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

#include "TextureHeaps.h"
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

namespace
{

// the slots the uploader writes: Loading, and Replacing once its new image is here
bool writes(const ImageSlots::Slot& slot)
{
  return slot.pixels
         && (slot.state == ImageSlots::State::Loading || slot.state == ImageSlots::State::Replacing);
}

} // namespace

void ImageSlots::setRequest(ImageRequest request)
{
  m_request = std::move(request);
}

void ImageSlots::setHeaps(TextureHeaps* heaps)
{
  m_heaps = heaps;
}

void ImageSlots::setInPlaceRule(InPlaceRule rule)
{
  m_inPlace = std::move(rule);
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
      adopt(id, lookup, m_seenChanges, false);
    }
  }
  return id;
}

void ImageSlots::refresh(const MaterialTable& table, const std::uint64_t frame)
{
  m_frame = frame;
  // P2.14b: a slot whose files changed since it was answered is asked again; LPF.1 (C40,
  // D98): one holding a complete texture keeps it bound meanwhile (Replacing)
  if (table.imageChanges != m_seenChanges)
  {
    m_seenChanges = table.imageChanges;
    for (ImageSlotId id = 0; id < ImageSlotId(m_slots.size()); ++id)
    {
      const auto& slot = m_slots[id];
      // LP6.4b (the director's reading (3)): an Evicted slot holds no image; the newest
      // is answered when a frame draws it again
      if (slot.state == State::Pending || slot.state == State::Evicted)
      {
        continue;
      }
      if (const auto* change = table.changedImages.find(slot.key);
          change && *change > slot.adoptedChange)
      {
        reRequest(id, frame, true, true);
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
  slot.waitsForRoom = false;
}

void ImageSlots::levelsWritten(const ImageSlotId id, const int firstMip)
{
  auto& slot = m_slots[id];
  if (slot.texture && firstMip < slot.firstWrittenMip)
  {
    // P2.14c: the binding's range covers the written levels only, and grows; LPF.1
    // (D98): a Replacing slot binds its complete texture until the swap, so what it binds
    // has not changed
    slot.firstWrittenMip = std::max(firstMip, 0);
    if (slot.state != State::Replacing)
    {
      bump(id);
    }
  }
}

void ImageSlots::dropPixels(const ImageSlotId id)
{
  auto& slot = m_slots[id];
  slot.pixels.reset();
  if (slot.state == State::Loading)
  {
    slot.state = State::Resident;
    ++m_stateVersion;
  }
  else if (
    slot.state == State::Replacing && slot.texture
    && slot.firstWrittenMip < slot.mipLevels)
  {
    // LPF.1 (C40, D98): the frame that wrote the new texture's last level swaps it in
    endReplacing(id, false);
    slot.state = State::Resident;
  }
}

void ImageSlots::refuse(const ImageSlotId id, std::string reason)
{
  auto& slot = m_slots[id];
  m_pending.erase(id);
  // what was written stays bound until the slot is asked again; nothing more is written.
  // LPF.1: a Replacing slot binds its complete texture again, the new one let go
  slot.pixels.reset();
  if (slot.state == State::Replacing)
  {
    endReplacing(id, true);
  }
  slot.state = State::Default;
  slot.reason = std::move(reason);
  ++m_stateVersion;
}

bool ImageSlots::waitForRoom(const ImageSlotId id)
{
  auto& slot = m_slots[id];
  return slot.state == State::Replacing && !std::exchange(slot.waitsForRoom, true);
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
  m_frame = std::max(m_frame, frame);
  reRequest(id, frame, false);
}

std::uint64_t ImageSlots::evict(const ImageSlotId id, const std::uint64_t frame)
{
  if (id >= m_slots.size())
  {
    return 0;
  }
  auto& slot = m_slots[id];
  const auto complete = slot.state == State::Resident && slot.texture
                        && slot.firstWrittenMip == 0 && slot.mipLevels > 0;
  if (!complete || slot.intrinsic == NoImageSlot)
  {
    return 0;
  }
  m_frame = std::max(m_frame, frame);
  // D35: held until the frames that may bind it have finished; its heap range with it
  const auto placement =
    m_heaps ? m_heaps->placementOf(slot.texture.Get()) : std::nullopt;
  const auto bytes = placement ? placement->range.size : slot.textureBytes;
  retire(slot.texture, slot.textureBytes, frame);
  slot.texture = nullptr;
  slot.mipLevels = 0;
  slot.firstWrittenMip = 0;
  slot.textureBytes = 0;
  slot.pixels.reset();
  slot.writeInPlace = false;
  slot.waitsForRoom = false;
  slot.floored = false;
  slot.state = State::Evicted;
  ++m_stateVersion;
  ++m_evictions;
  bump(id);
  m_evicted.insert(id);
  return bytes;
}

void ImageSlots::want(const ImageSlotId id)
{
  if (id < m_slots.size() && m_slots[id].state == State::Evicted)
  {
    m_wanted.insert(id);
  }
}

bool ImageSlots::wanted(const ImageSlotId id) const
{
  return m_wanted.contains(id);
}

std::vector<ImageSlotId> ImageSlots::evictedSlots() const
{
  auto ids = std::vector<ImageSlotId>{m_evicted.begin(), m_evicted.end()};
  std::ranges::sort(ids);
  return ids;
}

std::size_t ImageSlots::evicted() const
{
  return m_evicted.size();
}

void ImageSlots::clearWanted()
{
  m_wanted.clear();
}

void ImageSlots::reRequestEvicted(const ImageSlotId id, const std::uint64_t frame)
{
  if (id < m_slots.size() && m_slots[id].state == State::Evicted)
  {
    m_frame = std::max(m_frame, frame);
    reRequest(id, frame, false);
  }
}

bool ImageSlots::floor(const ImageSlotId id, const std::uint64_t frame)
{
  if (id >= m_slots.size())
  {
    return false;
  }
  auto& slot = m_slots[id];
  const auto complete = slot.state == State::Resident && slot.texture
                        && slot.firstWrittenMip == 0 && slot.mipLevels > 0;
  if (!complete || slot.floored || slot.intrinsic == NoImageSlot)
  {
    return false;
  }
  m_frame = std::max(m_frame, frame);
  slot.floored = true;
  ++m_floors;
  reRequest(id, frame, true, false);
  return true;
}

std::uint64_t ImageSlots::evictions() const
{
  return m_evictions;
}

std::uint64_t ImageSlots::floors() const
{
  return m_floors;
}

std::size_t ImageSlots::floored() const
{
  return std::size_t(std::ranges::count_if(m_slots, [](const Slot& slot) {
    return slot.floored
           && (slot.state == State::Resident || slot.state == State::Replacing);
  }));
}

std::vector<ImageSlotId> ImageSlots::inPlace() const
{
  auto ids = std::vector<ImageSlotId>{};
  for (ImageSlotId id = 0; id < ImageSlotId(m_slots.size()); ++id)
  {
    const auto& slot = m_slots[id];
    if (slot.writeInPlace && slot.pixels && slot.state == State::Resident)
    {
      ids.push_back(id);
    }
  }
  return ids;
}

void ImageSlots::inPlaceWritten(const ImageSlotId id)
{
  auto& slot = m_slots[id];
  if (!slot.writeInPlace)
  {
    return;
  }
  slot.writeInPlace = false;
  slot.pixels.reset();
  slot.adoptedUsage = slot.replacingUsage;
  slot.adoptedFormat = slot.replacingFormat;
  slot.adoptedOverride = slot.replacingOverride;
  slot.replacingUsage.reset();
  slot.replacingFormat.reset();
  slot.replacingOverride = false;
  ++m_inPlaceWrites;
  // what the slot binds changed at once, whole: its sets and records go (C28, C64)
  bump(id);
}

std::uint64_t ImageSlots::inPlaceWrites() const
{
  return m_inPlaceWrites;
}

std::uint64_t ImageSlots::flooringBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& slot : m_slots)
  {
    if (slot.floored && slot.state == State::Replacing && slot.replaced)
    {
      bytes += slot.replacedBytes;
    }
  }
  return bytes;
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
  if (slot.state == State::Replacing && slot.replaced)
  {
    // LPF.1 (C40, D98): the complete texture, every level, until the new one is written
    result.texture = slot.replaced.Get();
    result.subresources = nvrhi::TextureSubresourceSet{
      0,
      nvrhi::MipLevel(slot.replacedMipLevels),
      0,
      nvrhi::TextureSubresourceSet::AllArraySlices};
    return result;
  }
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
    if (writes(m_slots[id]))
    {
      ids.push_back(id);
    }
  }
  return ids;
}

bool ImageSlots::hasLoading() const
{
  // LP6.4b: an in-place write waiting is an upload left too
  return std::ranges::any_of(m_slots, [](const Slot& slot) {
    return writes(slot) || (slot.writeInPlace && slot.pixels);
  });
}

std::size_t ImageSlots::replacing() const
{
  return std::size_t(std::ranges::count_if(
    m_slots, [](const Slot& slot) { return slot.state == State::Replacing; }));
}

std::size_t ImageSlots::resident() const
{
  return std::size_t(std::ranges::count_if(m_slots, [](const Slot& slot) {
    return (slot.state == State::Replacing && slot.replaced)
           || (slot.state != State::Replacing && slot.texture
               && slot.firstWrittenMip < slot.mipLevels);
  }));
}

std::uint64_t ImageSlots::textureBytes() const
{
  // a Replacing slot holds both textures
  auto bytes = std::uint64_t{0};
  for (const auto& slot : m_slots)
  {
    bytes +=
      (slot.texture ? slot.textureBytes : 0) + (slot.replaced ? slot.replacedBytes : 0);
  }
  return bytes;
}

std::uint64_t ImageSlots::retiredBytes() const
{
  return m_retired.bytes();
}

std::uint64_t ImageSlots::dedicatedTextureBytes() const
{
  const auto dedicated =
    [&](const nvrhi::TextureHandle& texture, const std::uint64_t bytes) {
      return texture && !(m_heaps && m_heaps->placed(texture.Get())) ? bytes
                                                                     : std::uint64_t{0};
    };
  auto total = std::uint64_t{0};
  for (const auto& slot : m_slots)
  {
    total += dedicated(slot.texture, slot.textureBytes)
             + dedicated(slot.replaced, slot.replacedBytes);
  }
  return total;
}

std::size_t ImageSlots::dedicatedTextures(const TextureHeaps* heaps) const
{
  const auto dedicated = [&](const nvrhi::TextureHandle& texture) {
    return texture && !(heaps && heaps->placed(texture.Get())) ? std::size_t{1}
                                                               : std::size_t{0};
  };
  auto count = std::size_t{0};
  for (const auto& slot : m_slots)
  {
    count += dedicated(slot.texture) + dedicated(slot.replaced);
  }
  return count;
}

std::vector<ImageSlotId> ImageSlots::takeChangedSlots()
{
  auto changed = std::exchange(m_changed, {});
  std::ranges::sort(changed);
  const auto [first, last] = std::ranges::unique(changed);
  changed.erase(first, last);
  return changed;
}

std::vector<ImageSlotId> ImageSlots::takeUsageChangedSlots()
{
  auto changed = std::exchange(m_usageChanged, {});
  std::ranges::sort(changed);
  const auto [first, last] = std::ranges::unique(changed);
  changed.erase(first, last);
  return changed;
}

const std::vector<ImageSlotId>& ImageSlots::changedSlots() const
{
  return m_changed;
}

const std::vector<ImageSlotId>& ImageSlots::usageChangedSlots() const
{
  return m_usageChanged;
}

void ImageSlots::collectRetired(const std::uint64_t completedFrames)
{
  m_retired.collect(completedFrames);
}

void ImageSlots::releaseAll()
{
  // LP6.4a: every range a slot's texture holds goes with it (the heaps are released
  // after the slots)
  if (m_heaps)
  {
    for (const auto& slot : m_slots)
    {
      m_heaps->retire(slot.texture.Get(), m_frame);
      m_heaps->retire(slot.replaced.Get(), m_frame);
    }
  }
  m_retired.releaseAll();
  m_slots.clear();
  m_ids.clear();
  m_pending.clear();
  m_changed.clear();
  m_usageChanged.clear();
  m_evicted.clear();
  m_wanted.clear();
}

std::uint64_t ImageSlots::stateVersion() const
{
  return m_stateVersion;
}

void ImageSlots::adopt(
  const ImageSlotId id,
  const ImageLookup& lookup,
  const std::uint64_t change,
  const bool reported)
{
  auto& slot = m_slots[id];
  // LP6.1 (C64, P8.1): the draws' program reads the adopted usage, which an answer sets
  // without a bump
  const auto usageBefore = slot.adoptedUsage;
  const auto reportUsage = [&] {
    if (reported && slot.adoptedUsage != usageBefore)
    {
      m_usageChanged.push_back(id);
    }
  };
  m_pending.erase(id);
  ++m_stateVersion;
  // U2.1: the facts, never the service's slot (it owns the image's bytes)
  const auto usage =
    lookup.slot ? std::optional{lookup.slot->name.usage} : std::optional<int>{};
  const auto format = lookup.slot && lookup.slot->image
                        ? std::optional{lookup.slot->image->format}
                        : std::optional<int>{};
  // LP6.4b (the director's reading (5)): an override's (a fed) image
  const auto fed = lookup.slot && lookup.slot->override;
  slot.adoptedChange = change;
  if (lookup.image)
  {
    // Ready, Source or Generated: the uploader makes the texture; LPF.1 (D98): a
    // Replacing slot's complete texture stays bound meanwhile, with its own facts until
    // the swap
    slot.pixels = lookup.image;
    if (slot.state == State::Replacing)
    {
      slot.replacingUsage = usage;
      slot.replacingFormat = format;
      slot.replacingOverride = fed;
    }
    else
    {
      slot.adoptedUsage = usage;
      slot.adoptedFormat = format;
      slot.adoptedOverride = fed;
      slot.state = State::Loading;
      reportUsage();
    }
    slot.reason.clear();
    return;
  }
  slot.adoptedUsage = usage;
  slot.adoptedFormat = format;
  slot.adoptedOverride = fed;
  reportUsage();
  if (slot.state == State::Replacing)
  {
    // the image has gone (no file, a refusal): the replaced texture goes too, as when the
    // slot is asked again without one (the intrinsic stands in)
    retire(slot.replaced, slot.replacedBytes, m_frame);
    slot.replaced = nullptr;
    slot.replacedMipLevels = 0;
    slot.replacedBytes = 0;
    slot.replacingUsage.reset();
    slot.replacingFormat.reset();
    slot.replacingOverride = false;
    slot.waitsForRoom = false;
    bump(id);
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

void ImageSlots::reRequest(
  const ImageSlotId id,
  const std::uint64_t frame,
  const bool keepComplete,
  const bool inPlace)
{
  auto& slot = m_slots[id];
  // LP6.4b: an Evicted slot asked again is a first request's
  m_evicted.erase(id);
  m_wanted.erase(id);
  const auto complete = slot.state == State::Resident && slot.texture
                        && slot.firstWrittenMip == 0 && slot.mipLevels > 0;
  // LP6.4b (C68 (c), D173, A36, the director's reading (5)): a fed slot's complete
  // texture (made non-permanent) takes a new fed image whose desc equals it and whose
  // levels fit one frame's uploads for an in-place write: Resident, no new texture, no
  // bump until every level is written; else Replacing as before
  if (
    inPlace && keepComplete && complete && slot.adoptedOverride && !slot.floored
    && m_request && m_inPlace)
  {
    if (const auto lookup = m_request(slot.key);
        lookup.image && lookup.slot && lookup.slot->override
        && m_inPlace(*lookup.image, *slot.texture))
    {
      slot.pixels = lookup.image;
      slot.writeInPlace = true;
      slot.replacingUsage = std::optional{lookup.slot->name.usage};
      slot.replacingFormat = lookup.slot->image
                               ? std::optional{lookup.slot->image->format}
                               : std::optional<int>{};
      slot.replacingOverride = true;
      slot.adoptedChange = m_seenChanges;
      slot.reason.clear();
      m_pending.erase(id);
      return;
    }
  }
  slot.writeInPlace = false;
  if (keepComplete && (complete || slot.state == State::Replacing))
  {
    // LPF.1 (C40, D98): the complete texture stays bound, with the slot's version, until
    // the new image is written; a third image before the second landed lets the
    // unfinished new texture go (D35: held for the frames that wrote it)
    if (complete)
    {
      slot.replaced = std::move(slot.texture);
      slot.replacedMipLevels = slot.mipLevels;
      slot.replacedBytes = slot.textureBytes;
      // an in-place image not written yet goes with the restart
      slot.replacingUsage.reset();
      slot.replacingFormat.reset();
      slot.replacingOverride = false;
    }
    retire(slot.texture, slot.textureBytes, frame);
    slot.texture = nullptr;
    slot.mipLevels = 0;
    slot.firstWrittenMip = 0;
    slot.textureBytes = 0;
    slot.pixels.reset();
    slot.waitsForRoom = false;
    slot.reason.clear();
    if (slot.state != State::Replacing)
    {
      slot.state = State::Replacing;
      ++m_stateVersion;
    }
    m_pending.insert(id);
    if (m_request)
    {
      if (const auto lookup = m_request(slot.key);
          lookup.state != ImageLookup::State::Pending)
      {
        adopt(id, lookup, m_seenChanges);
      }
    }
    return;
  }
  // D35: the textures are held until the frames that may bind them have finished
  retire(slot.texture, slot.textureBytes, frame);
  retire(slot.replaced, slot.replacedBytes, frame);
  slot.texture = nullptr;
  slot.mipLevels = 0;
  slot.firstWrittenMip = 0;
  slot.textureBytes = 0;
  slot.replaced = nullptr;
  slot.replacedMipLevels = 0;
  slot.replacedBytes = 0;
  slot.replacingUsage.reset();
  slot.replacingFormat.reset();
  slot.replacingOverride = false;
  slot.waitsForRoom = false;
  slot.pixels.reset();
  slot.adoptedUsage.reset();
  slot.adoptedFormat.reset();
  slot.adoptedOverride = false;
  slot.reason.clear();
  slot.state = State::Pending;
  ++m_stateVersion;
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

void ImageSlots::retire(
  const nvrhi::TextureHandle& texture,
  const std::uint64_t bytes,
  const std::uint64_t frame)
{
  const auto placed = m_heaps && texture && m_heaps->retire(texture.Get(), frame);
  m_retired.retire(texture.Get(), placed ? 0 : bytes, frame);
}

void ImageSlots::endReplacing(const ImageSlotId id, const bool restore)
{
  auto& slot = m_slots[id];
  if (restore)
  {
    // the new texture goes, the complete one is the slot's again (its binding unchanged)
    retire(slot.texture, slot.textureBytes, m_frame);
    slot.texture = std::move(slot.replaced);
    slot.mipLevels = slot.replacedMipLevels;
    slot.firstWrittenMip = 0;
    slot.textureBytes = slot.replacedBytes;
  }
  else
  {
    // D35: the replaced texture is held until the frames that bound it have finished;
    // the bump erases the slot's binding sets (C28); the new image's facts are the draws'
    retire(slot.replaced, slot.replacedBytes, m_frame);
    slot.adoptedUsage = slot.replacingUsage;
    slot.adoptedFormat = slot.replacingFormat;
    slot.adoptedOverride = slot.replacingOverride;
    bump(id);
  }
  slot.replacingUsage.reset();
  slot.replacingFormat.reset();
  slot.replacingOverride = false;
  slot.replaced = nullptr;
  slot.replacedMipLevels = 0;
  slot.replacedBytes = 0;
  slot.waitsForRoom = false;
  ++m_stateVersion;
}

} // namespace tb::lit
