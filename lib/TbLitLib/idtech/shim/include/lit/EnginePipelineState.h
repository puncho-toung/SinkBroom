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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/GLState.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2014-2016 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>

// LP2.0 (C24, P2.25): two engine functions copied verbatim into TbLitIdLib, behind a
// facade any thread may call, because they read nothing but their arguments (mirror.json
// M-PIPE-PART-01, M-IMG-PART-02 with their pure-function entries): the GL state bits as
// NVRHI render state, and an image level's row pitch. With GLState.h's bits that the
// preview's passes compose, checked against the copied header where the copies compile.

namespace tb::lit
{

// renderer/GLState.h (copied whole), the bits LP2 composes; static_asserted equal to the
// copy's in shim/src/ShimPipelineState.cpp
namespace engine_gls
{
constexpr std::uint64_t SrcBlendOne = 0ull << 0;
constexpr std::uint64_t SrcBlendZero = 1ull << 0;
constexpr std::uint64_t SrcBlendDstColor = 2ull << 0;
constexpr std::uint64_t DstBlendZero = 0ull << 3;
constexpr std::uint64_t DstBlendOne = 1ull << 3;
constexpr std::uint64_t DepthMask = 1ull << 6;
constexpr std::uint64_t AlphaMask = 1ull << 10;
constexpr std::uint64_t PolygonOffset = 1ull << 12;
constexpr std::uint64_t DepthFuncLess = 0ull << 13;
constexpr std::uint64_t DepthFuncAlways = 1ull << 13;
constexpr std::uint64_t DepthFuncEqual = 3ull << 13;
constexpr std::uint64_t CullFrontSided = 0ull << 15;
constexpr std::uint64_t CullTwoSided = 2ull << 15;
constexpr std::uint64_t StencilFuncAlways = 0ull << 36;
constexpr std::uint64_t MirrorView = 1ull << 62;
constexpr std::uint64_t Default = 0;
} // namespace engine_gls

/**
 * PipelineCache::GetRenderState (renderer/PipelineCache.cpp:99-436, with
 * GetStencilOpState :438-530, copied): renderState specialised by the GL state bits, the
 * PipelineKey's int depth bias and its slope bias, with the engine's bugs (the
 * GLS_CULL_BITS mask, the blend op that is never set, SC209-SC210). The caller sets
 * GetOrCreatePipeline's defaults first (scissor, depth test and write, blend on). Pure;
 * any thread.
 */
void engineRenderState(
  std::uint64_t stateBits,
  int depthBias,
  float slopeBias,
  nvrhi::RenderState& renderState);

/**
 * GetRowPitch (renderer/Image_load.cpp:131-143, with BitsForFormat and BlockSizeForFormat
 * :39-123, copied): the bytes of one row of an image level of this textureFormat_t and
 * width; for a block-compressed format one row of 4×4 blocks. LP2.1 (T0.14): nullopt for
 * a format outside textureFormat_t's FMT_RGBA8 to FMT_RGBA16S (the copy asserts in Debug
 * and returns 0 in Release, and a zero pitch uploads nothing, nvrhi rule 12), for
 * FMT_NONE and for a width below 1: the caller refuses the image. Pure; any thread.
 */
std::optional<int> engineRowPitch(int textureFormat, int width);

} // namespace tb::lit
