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

#pragma once

#include "lit/EngineTables.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tb::lit
{

// the most float4s of any renderparm set (rpMaximalSet11's 68), the size of a commit
inline constexpr std::size_t MaxParmSetFloat4s = 68;

/**
 * LP2.2a (C27, SC241): renderProgManager's uniforms as the backend keeps them, one float4
 * per renderParm_t, and the per-binding-layout "uniforms changed" flags.
 *
 * - zero is ZeroUniforms (RenderProgs_NVRHI.cpp:371-379): every parm 0 and every layout
 *   changed, at the start of every view (SC201);
 * - set is SetUniformValue (:343-364): a parm whose value differs marks every layout
 *   whose renderparm set holds it (renderParmLayoutTypes, RenderProgs.cpp:224-391) as
 *   changed;
 * - select is SelectUniforms (:386-533): the layout's set, member by member in the set's
 *   order, 16 bytes each, as CommitConstantBuffer writes it and CommitPushConstants
 *   pushes it.
 *
 * Only the sets the preview mirrors (0, 4 and 10, lit/EngineTables.h) are selectable;
 * their layouts are the ones LP2 draws with. Plain data; the render thread.
 */
class RenderParms
{
public:
  using Vec4 = std::array<float, 4>;

private:
  std::array<Vec4, std::size_t(RenderParm::Total)> m_values = {};
  std::array<bool, std::size_t(EngineBindingLayout::Num)> m_changed = {};

public:
  RenderParms();

  void zero();
  void set(RenderParm parm, const Vec4& value);
  // SetVertexParms: count float4s from values, into parm and the ones after it
  void set(RenderParm first, std::span<const float> values);

  const Vec4& value(RenderParm parm) const;
  bool changed(EngineBindingLayout layout) const;
  void clearChanged(EngineBindingLayout layout);

  /**
   * SelectUniforms: the layout's renderparm set into out, member by member (4 floats
   * each); the number of floats written, 0 for a layout whose set is not mirrored or
   * that out cannot hold.
   */
  std::size_t select(EngineBindingLayout layout, std::span<float> out) const;
};

// the layouts whose renderparm set holds parm (renderParmLayoutTypes), as bits of
// EngineBindingLayout; only the mirrored sets 0, 4 and 10 are known. Pure.
std::uint64_t engineLayoutsOfParm(RenderParm parm);

} // namespace tb::lit
