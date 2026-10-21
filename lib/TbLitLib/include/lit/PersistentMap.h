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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace tb::lit
{

/**
 * An immutable hash map whose set and erase return a new map that shares every node off
 * the key's path (R5.16): a two-level trie of 64 × 64 buckets on the key's hash. A change
 * copies the root's 64 pointers, one inner node's 64 pointers and one small bucket, never
 * the values or the rest of the map, so publishing one changed material costs the same
 * whatever the table's size. Maps are values: copying one copies a pointer, and a map
 * may be read from any thread while another thread builds a newer one from it.
 */
template <
  typename Key,
  typename Value,
  typename Hash = std::hash<Key>,
  typename Equal = std::equal_to<Key>>
class PersistentMap
{
public:
  static constexpr std::size_t Bits = 6;
  static constexpr std::size_t Fanout = std::size_t{1} << Bits;

private:
  struct Bucket
  {
    std::vector<std::pair<Key, Value>> items;
  };

  struct Inner
  {
    std::array<std::shared_ptr<const Bucket>, Fanout> buckets;
  };

  struct Root
  {
    std::array<std::shared_ptr<const Inner>, Fanout> inners;
    std::size_t size = 0;
  };

  std::shared_ptr<const Root> m_root;

  static std::pair<std::size_t, std::size_t> positionsOf(const Key& key)
  {
    // a multiplicative mix, so that both levels use well-mixed bits
    auto x = std::uint64_t(Hash{}(key)) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return {
      std::size_t(x >> (64 - Bits)),
      std::size_t((x >> (64 - 2 * Bits)) & (Fanout - 1)),
    };
  }

  explicit PersistentMap(std::shared_ptr<const Root> root)
    : m_root{std::move(root)}
  {
  }

public:
  PersistentMap() = default;

  std::size_t size() const { return m_root ? m_root->size : 0; }
  bool empty() const { return size() == 0; }

  // the value of key, or null; valid while this map lives
  const Value* find(const Key& key) const
  {
    if (!m_root)
    {
      return nullptr;
    }
    const auto [outer, inner] = positionsOf(key);
    const auto& innerNode = m_root->inners[outer];
    if (!innerNode)
    {
      return nullptr;
    }
    const auto& bucket = innerNode->buckets[inner];
    if (!bucket)
    {
      return nullptr;
    }
    for (const auto& item : bucket->items)
    {
      if (Equal{}(item.first, key))
      {
        return &item.second;
      }
    }
    return nullptr;
  }

  // a map with key set to value
  PersistentMap set(Key key, Value value) const
  {
    const auto [outer, inner] = positionsOf(key);

    auto root = m_root ? std::make_shared<Root>(*m_root) : std::make_shared<Root>();
    auto innerNode = root->inners[outer] ? std::make_shared<Inner>(*root->inners[outer])
                                         : std::make_shared<Inner>();
    auto bucket = innerNode->buckets[inner]
                    ? std::make_shared<Bucket>(*innerNode->buckets[inner])
                    : std::make_shared<Bucket>();

    auto replaced = false;
    for (auto& item : bucket->items)
    {
      if (Equal{}(item.first, key))
      {
        item.second = std::move(value);
        replaced = true;
        break;
      }
    }
    if (!replaced)
    {
      bucket->items.emplace_back(std::move(key), std::move(value));
      ++root->size;
    }

    innerNode->buckets[inner] = std::move(bucket);
    root->inners[outer] = std::move(innerNode);
    return PersistentMap{std::move(root)};
  }

  // a map without key; this map itself when it has no key
  PersistentMap erase(const Key& key) const
  {
    if (!find(key))
    {
      return *this;
    }
    const auto [outer, inner] = positionsOf(key);

    auto root = std::make_shared<Root>(*m_root);
    auto innerNode = std::make_shared<Inner>(*root->inners[outer]);
    auto bucket = std::make_shared<Bucket>(*innerNode->buckets[inner]);
    for (auto it = bucket->items.begin(); it != bucket->items.end(); ++it)
    {
      if (Equal{}(it->first, key))
      {
        bucket->items.erase(it);
        break;
      }
    }
    --root->size;

    innerNode->buckets[inner] =
      bucket->items.empty() ? nullptr : std::shared_ptr<const Bucket>{std::move(bucket)};
    root->inners[outer] = std::move(innerNode);
    return PersistentMap{std::move(root)};
  }

  // calls f(key, value) for every item, in no defined order
  template <typename F>
  void forEach(F&& f) const
  {
    if (!m_root)
    {
      return;
    }
    for (const auto& innerNode : m_root->inners)
    {
      if (!innerNode)
      {
        continue;
      }
      for (const auto& bucket : innerNode->buckets)
      {
        if (!bucket)
        {
          continue;
        }
        for (const auto& item : bucket->items)
        {
          f(item.first, item.second);
        }
      }
    }
  }

  // whether both maps share their root: nothing changed between them
  bool sharesRootWith(const PersistentMap& other) const { return m_root == other.m_root; }
};

} // namespace tb::lit
