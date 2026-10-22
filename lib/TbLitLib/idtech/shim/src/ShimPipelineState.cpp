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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/PipelineCache.h):
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP2.0 (C24, P2.25): the copied ranges of renderer/PipelineCache.cpp (GetRenderState,
// GetStencilOpState) and renderer/Image_load.cpp (BitsForFormat, BlockSizeForFormat,
// GetRowPitch), compiled here behind the facade lit/EnginePipelineState.h. Both are pure:
// they read their arguments and the copied GLState.h and Image.h constants only, call no
// common, FatalError, SIMD or allocator, and have no statics (C24's five conditions,
// mirror.json M-PIPE-PURE-01, M-IMG-PURE-02), so any thread may run them.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
// clang-format on

#include "lit/EnginePipelineState.h"

#include <nvrhi/nvrhi.h>

// PipelineCache.h:31-39's PipelineKey names the engine's Framebuffer class, which only
// its pointer member needs
class Framebuffer;

#include "renderer/PipelineCache.h"

// PipelineCache.h:65-86 declares the cache with these two members private; the copies
// define them, and the shim calls them on an empty object (the members they define never
// read the cache's state)
class PipelineCache
{
public:
  void GetRenderState(uint64 stateBits, PipelineKey key, nvrhi::RenderState& renderState);
  nvrhi::DepthStencilState::StencilOpDesc GetStencilOpState(uint64 stateBits);
};

#include "renderer/Image_load.cpp"
#include "renderer/PipelineCache.cpp"

namespace tb::lit
{

static_assert(engine_gls::SrcBlendOne == GLS_SRCBLEND_ONE);
static_assert(engine_gls::SrcBlendZero == GLS_SRCBLEND_ZERO);
static_assert(engine_gls::SrcBlendDstColor == GLS_SRCBLEND_DST_COLOR);
static_assert(engine_gls::DstBlendZero == GLS_DSTBLEND_ZERO);
static_assert(engine_gls::DstBlendOne == GLS_DSTBLEND_ONE);
static_assert(engine_gls::DepthMask == GLS_DEPTHMASK);
static_assert(engine_gls::AlphaMask == GLS_ALPHAMASK);
static_assert(engine_gls::PolygonOffset == GLS_POLYGON_OFFSET);
static_assert(engine_gls::DepthFuncLess == GLS_DEPTHFUNC_LESS);
static_assert(engine_gls::DepthFuncAlways == GLS_DEPTHFUNC_ALWAYS);
static_assert(engine_gls::DepthFuncEqual == GLS_DEPTHFUNC_EQUAL);
static_assert(engine_gls::CullFrontSided == GLS_CULL_FRONTSIDED);
static_assert(engine_gls::CullTwoSided == GLS_CULL_TWOSIDED);
static_assert(engine_gls::StencilFuncAlways == GLS_STENCIL_FUNC_ALWAYS);
static_assert(engine_gls::MirrorView == GLS_MIRROR_VIEW);
static_assert(engine_gls::Default == GLS_DEFAULT);

void engineRenderState(
  const std::uint64_t stateBits,
  const int depthBias,
  const float slopeBias,
  nvrhi::RenderState& renderState)
{
  // GetOrCreatePipeline's key: GetRenderState reads its biases only
  auto key = PipelineKey{};
  key.state = stateBits;
  key.program = 0;
  key.depthBias = depthBias;
  key.slopeBias = slopeBias;
  key.framebuffer = nullptr;
  auto cache = PipelineCache{};
  cache.GetRenderState(stateBits, key, renderState);
}

std::optional<int> engineRowPitch(const int textureFormat, const int width)
{
  // T0.14: BitsForFormat knows FMT_NONE (0 bits) to FMT_RGBA16S; outside them it asserts
  if (textureFormat <= int(FMT_NONE) || textureFormat > int(FMT_RGBA16S) || width < 1)
  {
    return std::nullopt;
  }
  const auto pitch = GetRowPitch(textureFormat_t(textureFormat), width);
  return pitch > 0 ? std::optional{pitch} : std::nullopt;
}

} // namespace tb::lit
