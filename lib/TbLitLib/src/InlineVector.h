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

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>

namespace tb::lit
{

/**
 * LP6.1 (C64): at most N items held inline with their count, for the keys and bindings
 * the draws build (BindingSetKey's slots and resources, ImageSlots::Binding's versions):
 * no heap allocation. Equality and iteration see the used prefix only. N is the largest
 * use, so an item past it is a programming error (asserted; dropped in Release). Pure.
 */
template <typename T, std::size_t N>
class InlineVector
{
  static_assert(N > 0);

private:
  std::array<T, N> m_items = {};
  std::size_t m_size = 0;

public:
  using value_type = T;
  using iterator = T*;
  using const_iterator = const T*;

  static constexpr std::size_t Capacity = N;

  constexpr InlineVector() = default;

  constexpr InlineVector(const std::initializer_list<T> items)
  {
    append(items.begin(), items.end());
  }

  // a shorter one's items (ImageSlots::Binding's versions into a key's slots)
  template <std::size_t M>
    requires(M < N)
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr InlineVector(const InlineVector<T, M>& other)
  {
    append(other.begin(), other.end());
  }

  constexpr InlineVector& operator=(const std::initializer_list<T> items)
  {
    clear();
    append(items.begin(), items.end());
    return *this;
  }

  constexpr void push_back(const T& item)
  {
    assert(m_size < N);
    if (m_size < N)
    {
      m_items[m_size++] = item;
    }
  }

  template <typename Iterator>
  constexpr void append(Iterator first, const Iterator last)
  {
    for (; first != last; ++first)
    {
      push_back(*first);
    }
  }

  constexpr void clear() { m_size = 0; }

  constexpr std::size_t size() const { return m_size; }
  constexpr bool empty() const { return m_size == 0; }

  constexpr T* begin() { return m_items.data(); }
  constexpr T* end() { return m_items.data() + m_size; }
  constexpr const T* begin() const { return m_items.data(); }
  constexpr const T* end() const { return m_items.data() + m_size; }

  constexpr T& operator[](const std::size_t i) { return m_items[i]; }
  constexpr const T& operator[](const std::size_t i) const { return m_items[i]; }
  constexpr const T& front() const { return m_items[0]; }
  constexpr const T& back() const { return m_items[m_size - 1]; }

  friend constexpr bool operator==(const InlineVector& lhs, const InlineVector& rhs)
  {
    return std::ranges::equal(lhs, rhs);
  }
};

} // namespace tb::lit
