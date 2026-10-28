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

#include "BindingSetCache.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace tb::lit
{
namespace
{

void combine(std::size_t& seed, const std::size_t value)
{
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

} // namespace

std::size_t BindingSetKeyHash::operator()(const BindingSetKey& key) const
{
  auto seed = std::hash<const void*>{}(key.layout);
  for (const auto& slot : key.slots)
  {
    combine(seed, std::hash<std::uint32_t>{}(slot.slot));
    combine(seed, std::hash<std::uint64_t>{}(slot.version));
  }
  combine(seed, std::hash<std::uint64_t>{}(key.samplers));
  combine(seed, std::hash<std::uint64_t>{}(key.renderTargetGeneration));
  for (const auto* resource : key.resources)
  {
    combine(seed, std::hash<const void*>{}(resource));
  }
  return seed;
}

BindingSetCache::BindingSetCache(const std::size_t capacity)
  : m_capacity{std::max(capacity, std::size_t{1})}
{
}

nvrhi::BindingSetHandle BindingSetCache::get(
  const BindingSetKey& key,
  const std::uint64_t frame,
  const std::function<nvrhi::BindingSetHandle()>& make)
{
  const auto* entry = entryOf(key, frame, make);
  return entry ? entry->set : nvrhi::BindingSetHandle{};
}

BindingSetCache::Entry* BindingSetCache::hold(
  const BindingSetKey& key,
  const std::uint64_t frame,
  const std::function<nvrhi::BindingSetHandle()>& make)
{
  auto* entry = entryOf(key, frame, make);
  if (entry)
  {
    ++entry->holds;
  }
  return entry;
}

BindingSetCache::Entry* BindingSetCache::entryOf(
  const BindingSetKey& key,
  const std::uint64_t frame,
  const std::function<nvrhi::BindingSetHandle()>& make)
{
  if (const auto it = m_sets.find(key); it != m_sets.end())
  {
    it->second.lastUsed = frame;
    return &it->second;
  }
  auto set = make();
  if (!set)
  {
    return nullptr;
  }
  ++m_made;
  const auto [it, inserted] = m_sets.emplace(key, Entry{std::move(set), frame, 0});
  for (const auto& slot : it->first.slots)
  {
    m_bySlot[slot.slot].emplace(&it->first, &it->second);
  }
  return &it->second;
}

void BindingSetCache::release(Entry& entry)
{
  entry.holds -= std::min(entry.holds, 1u);
}

std::size_t BindingSetCache::slotChanged(const ImageSlotId slot)
{
  const auto dependent = m_bySlot.find(slot);
  if (dependent == m_bySlot.end())
  {
    return 0;
  }
  // copied: erase edits the reverse index
  auto keys = std::vector<const BindingSetKey*>{};
  keys.reserve(dependent->second.size());
  for (const auto& [key, entry] : dependent->second)
  {
    keys.push_back(key);
  }
  for (const auto* key : keys)
  {
    erase(m_sets.find(*key));
  }
  return keys.size();
}

std::size_t BindingSetCache::renderTargetsChanged(const std::uint64_t generation)
{
  auto erased = std::size_t{0};
  for (auto it = m_sets.begin(); it != m_sets.end();)
  {
    const auto stale = it->first.renderTargetGeneration != 0
                       && it->first.renderTargetGeneration != generation;
    const auto next = std::next(it);
    if (stale)
    {
      erase(it);
      ++erased;
    }
    it = next;
  }
  return erased;
}

std::size_t BindingSetCache::trim(const std::uint64_t frame)
{
  if (m_sets.size() <= m_capacity)
  {
    return 0;
  }
  // the oldest first; a set of this frame is in its command list and stays, and so does
  // (LP6.1) a set a record holds
  auto byAge = std::vector<std::pair<std::uint64_t, const BindingSetKey*>>{};
  byAge.reserve(m_sets.size());
  for (const auto& [key, entry] : m_sets)
  {
    if (entry.lastUsed < frame && entry.holds == 0)
    {
      byAge.emplace_back(entry.lastUsed, &key);
    }
  }
  const auto excess = std::min(m_sets.size() - m_capacity, byAge.size());
  std::ranges::nth_element(byAge, byAge.begin() + std::ptrdiff_t(excess));
  for (std::size_t i = 0; i < excess; ++i)
  {
    erase(m_sets.find(*byAge[i].second));
  }
  return excess;
}

std::size_t BindingSetCache::size() const
{
  return m_sets.size();
}

std::size_t BindingSetCache::capacity() const
{
  return m_capacity;
}

std::size_t BindingSetCache::setsOf(const ImageSlotId slot) const
{
  const auto it = m_bySlot.find(slot);
  return it != m_bySlot.end() ? it->second.size() : 0;
}

std::optional<std::uint64_t> BindingSetCache::lastBound(const ImageSlotId slot) const
{
  const auto it = m_bySlot.find(slot);
  if (it == m_bySlot.end() || it->second.empty())
  {
    return std::nullopt;
  }
  auto last = std::uint64_t{0};
  for (const auto& [key, entry] : it->second)
  {
    last = std::max(last, entry->lastUsed);
  }
  return last;
}

std::uint64_t BindingSetCache::made() const
{
  return m_made;
}

std::uint64_t BindingSetCache::heldErased() const
{
  return m_heldErased;
}

void BindingSetCache::clear()
{
  m_bySlot.clear();
  m_sets.clear();
}

void BindingSetCache::erase(
  const std::unordered_map<BindingSetKey, Entry, BindingSetKeyHash>::iterator it)
{
  if (it == m_sets.end())
  {
    return;
  }
  // LP6.1 (C64): a record still holding it would draw an erased set
  if (it->second.holds > 0)
  {
    ++m_heldErased;
  }
  for (const auto& slot : it->first.slots)
  {
    if (const auto dependent = m_bySlot.find(slot.slot); dependent != m_bySlot.end())
    {
      dependent->second.erase(&it->first);
      if (dependent->second.empty())
      {
        m_bySlot.erase(dependent);
      }
    }
  }
  m_sets.erase(it);
}

} // namespace tb::lit
