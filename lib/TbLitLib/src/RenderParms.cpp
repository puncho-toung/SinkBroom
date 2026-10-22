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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/NVRHI/RenderProgs_NVRHI.cpp and
// renderer/RenderProgs.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2022 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023 Stephen Saunders
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "RenderParms.h"

#include <algorithm>
#include <cstring>

namespace tb::lit
{
namespace
{

constexpr auto LayoutCount = std::size_t(EngineBindingLayout::Num);

static_assert(LayoutCount <= 64);

// renderParmLayoutTypes (RenderProgs.cpp:224-391) for the mirrored sets, as bit masks
std::array<std::uint64_t, std::size_t(RenderParm::Total)> layoutsOfParms()
{
  auto masks = std::array<std::uint64_t, std::size_t(RenderParm::Total)>{};
  for (std::size_t layout = 0; layout < LayoutCount; ++layout)
  {
    const auto set = engineParmSetOfLayout(EngineBindingLayout(layout));
    for (const auto& member : engineParmSetMembers(set))
    {
      masks[std::size_t(member.parm)] |= std::uint64_t{1} << layout;
    }
  }
  return masks;
}

const std::array<std::uint64_t, std::size_t(RenderParm::Total)>& parmLayouts()
{
  static const auto masks = layoutsOfParms();
  return masks;
}

} // namespace

std::uint64_t engineLayoutsOfParm(const RenderParm parm)
{
  return parm < RenderParm::Total ? parmLayouts()[std::size_t(parm)] : 0;
}

RenderParms::RenderParms()
{
  zero();
}

void RenderParms::zero()
{
  // ZeroUniforms: the values cleared and every layout's constants due
  for (auto& value : m_values)
  {
    value = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
  }
  m_changed.fill(true);
}

void RenderParms::set(const RenderParm parm, const Vec4& value)
{
  if (parm >= RenderParm::Total)
  {
    return;
  }
  // SetUniformValue: a component that differs marks the parm changed
  auto& current = m_values[std::size_t(parm)];
  auto parmChanged = false;
  for (std::size_t i = 0; i < 4; ++i)
  {
    if (current[i] != value[i])
    {
      current[i] = value[i];
      parmChanged = true;
    }
  }
  if (parmChanged)
  {
    const auto mask = engineLayoutsOfParm(parm);
    for (std::size_t layout = 0; layout < LayoutCount; ++layout)
    {
      if (mask & (std::uint64_t{1} << layout))
      {
        m_changed[layout] = true;
      }
    }
  }
}

void RenderParms::set(const RenderParm first, const std::span<const float> values)
{
  // SetVertexParms( rp, values, num ): float4 i into rp + i
  for (std::size_t i = 0; i + 4 <= values.size(); i += 4)
  {
    set(
      RenderParm(int(first) + int(i / 4)),
      Vec4{values[i], values[i + 1], values[i + 2], values[i + 3]});
  }
}

const RenderParms::Vec4& RenderParms::value(const RenderParm parm) const
{
  return m_values[std::min(std::size_t(parm), m_values.size() - 1)];
}

bool RenderParms::changed(const EngineBindingLayout layout) const
{
  return std::size_t(layout) < LayoutCount && m_changed[std::size_t(layout)];
}

void RenderParms::clearChanged(const EngineBindingLayout layout)
{
  if (std::size_t(layout) < LayoutCount)
  {
    m_changed[std::size_t(layout)] = false;
  }
}

std::size_t RenderParms::select(
  const EngineBindingLayout layout, const std::span<float> out) const
{
  // SelectUniforms: memcpy of each member's float4, in the set's order
  const auto members = engineParmSetMembers(engineParmSetOfLayout(layout));
  if (members.empty() || out.size() < members.size() * 4)
  {
    return 0;
  }
  for (std::size_t i = 0; i < members.size(); ++i)
  {
    const auto& value = m_values[std::size_t(members[i].parm)];
    std::memcpy(out.data() + i * 4, value.data(), sizeof(float) * 4);
  }
  return members.size() * 4;
}

} // namespace tb::lit
