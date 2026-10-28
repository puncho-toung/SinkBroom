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

#include "ImageSlots.h"
#include "InlineVector.h"
#include "base/Macros.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::lit
{

// LP6.1 (C64): the most slot versions a binding set's key holds, the ambient pass's set 2
// (AMBIENT_LIGHTING_IBL's nine units, each with the intrinsic standing in), and the most
// other resources, DRAW_INTERACTION_SM's sampler set (four samplers)
inline constexpr std::size_t BindingSetKeySlots = 18;
inline constexpr std::size_t BindingSetKeyResources = 4;

/**
 * C28, D37: what a binding set is made of, beyond its layout's order: the binding layout,
 * the image slots it binds with their versions (a slot bound through its intrinsic lists
 * both, ImageSlots::Binding), the sampler set, the render-target generation only when it
 * binds a render target (0 otherwise), and the other resources it binds, which must live
 * as long as the session (constant buffers, the tonemap buffers). LP6.1 (C64): the slots
 * and the resources are held inline (no heap vector per key); the hash and the equality
 * read the used prefix.
 */
struct BindingSetKey
{
  nvrhi::IBindingLayout* layout = nullptr;
  InlineVector<SlotVersion, BindingSetKeySlots> slots;
  std::uint64_t samplers = 0;
  std::uint64_t renderTargetGeneration = 0;
  InlineVector<const void*, BindingSetKeyResources> resources;

  bool operator==(const BindingSetKey&) const = default;
};

struct BindingSetKeyHash
{
  std::size_t operator()(const BindingSetKey& key) const;
};

/**
 * C28, D37 (LP2.1): the render thread's binding sets, made against the compile thread's
 * layouts (C20: binding layouts are the compile thread's, binding sets the render
 * thread's). A reverse index maps each slot to the sets that hold it, so that a version
 * bump (slotChanged) erases exactly the sets that depend on it; a new render-target
 * generation erases exactly the sets that bind a render target; an LRU over the last
 * frame each set was used bounds the cache (BindingSetCacheSize). A set erased here may
 * still be in a frame in flight: its command list holds it until NVRHI collects it.
 *
 * LP6.1 (C64, P8.1, D172): a draw record holds the entries of the sets it draws with
 * (hold): it stamps each one's lastUsed once per frame through the entry (no hash; the
 * map's nodes do not move), and trim never erases a held entry, so a record's set stays
 * in the map and in the reverse index, where slotChanged finds it. The records let go of
 * their holds before the cache erases a set they hold (the slot's change, the render
 * targets' generation, teardown); an erased held entry is counted (heldErased, 0).
 * Render thread only; clear in teardown step 2.
 */
class BindingSetCache
{
public:
  // a set and the last frame it was used in; the records holding it (LP6.1)
  struct Entry
  {
    nvrhi::BindingSetHandle set;
    std::uint64_t lastUsed = 0;
    std::uint32_t holds = 0;
  };

private:
  std::size_t m_capacity;
  std::unordered_map<BindingSetKey, Entry, BindingSetKeyHash> m_sets;
  // the keys (owned by m_sets, whose nodes do not move) that hold each slot
  std::unordered_map<ImageSlotId, std::unordered_set<const BindingSetKey*>> m_bySlot;
  std::uint64_t m_made = 0;
  std::uint64_t m_heldErased = 0;

public:
  explicit BindingSetCache(std::size_t capacity);

  /**
   * The set of key, made by make on a miss; null when make returns null (nothing is
   * kept then). frame is the frame it is used in, for the LRU.
   */
  nvrhi::BindingSetHandle get(
    const BindingSetKey& key,
    std::uint64_t frame,
    const std::function<nvrhi::BindingSetHandle()>& make);
  /**
   * LP6.1 (C64): get's entry, held by the caller (a draw record) until it lets go
   * (release); null when make returns null. The entry lives while it is held.
   */
  Entry* hold(
    const BindingSetKey& key,
    std::uint64_t frame,
    const std::function<nvrhi::BindingSetHandle()>& make);
  // LP6.1 (C64): a hold let go
  static void release(Entry& entry);

  // a slot's version changed: the sets that hold it go; how many
  std::size_t slotChanged(ImageSlotId slot);
  // C28: the render targets were made again (generation): the sets that bind one of an
  // older generation go, and no other; how many
  std::size_t renderTargetsChanged(std::uint64_t generation);
  // D37: beyond the capacity the least recently used sets go, never one used in frame
  // nor (LP6.1) one a record holds; how many
  std::size_t trim(std::uint64_t frame);

  std::size_t size() const;
  std::size_t capacity() const;
  // the sets that hold a slot (the reverse index)
  std::size_t setsOf(ImageSlotId slot) const;
  // the sets made so far
  std::uint64_t made() const;
  // LP6.1 (C64): the held entries erased so far (a record outliving its set: 0)
  std::uint64_t heldErased() const;
  // teardown step 2
  void clear();

private:
  // get's and hold's: the entry of key, made by make on a miss
  Entry* entryOf(
    const BindingSetKey& key,
    std::uint64_t frame,
    const std::function<nvrhi::BindingSetHandle()>& make);
  void erase(std::unordered_map<BindingSetKey, Entry, BindingSetKeyHash>::iterator it);

  deleteCopyAndMove(BindingSetCache);
};

} // namespace tb::lit
