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

#include "lit/EngineImageEnums.h"
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
static_assert(engine_gls::SrcBlendSrcAlpha == GLS_SRCBLEND_SRC_ALPHA);
static_assert(engine_gls::SrcBlendBits == GLS_SRCBLEND_BITS);
static_assert(engine_gls::DstBlendOneMinusSrcAlpha == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
static_assert(engine_gls::DstBlendBits == GLS_DSTBLEND_BITS);
static_assert(engine_gls::DepthFuncBits == GLS_DEPTHFUNC_BITS);
static_assert(engine_gls::CullBackSided == GLS_CULL_BACKSIDED);
static_assert(engine_gls::CullBits == GLS_CULL_BITS);
static_assert(engine_gls::StencilFuncRefShift == GLS_STENCIL_FUNC_REF_SHIFT);
static_assert(engine_gls::StencilFuncRefBits == GLS_STENCIL_FUNC_REF_BITS);
static_assert(engine_gls::StencilFuncMaskShift == GLS_STENCIL_FUNC_MASK_SHIFT);
static_assert(engine_gls::StencilFuncMaskBits == GLS_STENCIL_FUNC_MASK_BITS);
static_assert(engine_gls::StencilFuncEqual == GLS_STENCIL_FUNC_EQUAL);
static_assert(engine_gls::StencilOpFailKeep == GLS_STENCIL_OP_FAIL_KEEP);
static_assert(engine_gls::StencilOpZFailKeep == GLS_STENCIL_OP_ZFAIL_KEEP);
static_assert(engine_gls::StencilOpZFailDecrWrap == GLS_STENCIL_OP_ZFAIL_DECR_WRAP);
static_assert(engine_gls::StencilOpPassKeep == GLS_STENCIL_OP_PASS_KEEP);
static_assert(engine_gls::StencilOpPassReplace == GLS_STENCIL_OP_PASS_REPLACE);
static_assert(engine_gls::StencilOpPassDecrWrap == GLS_STENCIL_OP_PASS_DECR_WRAP);
static_assert(engine_gls::Override == GLS_OVERRIDE);

// U1.5: engine_image's constants (lit/EngineImageEnums.h) are the copied Image.h's and
// Material.h's enum values
static_assert(engine_image::FormatNone == int(FMT_NONE));
static_assert(engine_image::FormatRgba8 == int(FMT_RGBA8));
static_assert(engine_image::FormatXrgb8 == int(FMT_XRGB8));
static_assert(engine_image::FormatAlpha == int(FMT_ALPHA));
static_assert(engine_image::FormatL8a8 == int(FMT_L8A8));
static_assert(engine_image::FormatLum8 == int(FMT_LUM8));
static_assert(engine_image::FormatInt8 == int(FMT_INT8));
static_assert(engine_image::FormatDxt1 == int(FMT_DXT1));
static_assert(engine_image::FormatDxt5 == int(FMT_DXT5));
static_assert(engine_image::FormatDepth == int(FMT_DEPTH));
static_assert(engine_image::FormatX16 == int(FMT_X16));
static_assert(engine_image::FormatY16X16 == int(FMT_Y16_X16));
static_assert(engine_image::FormatRgb565 == int(FMT_RGB565));
static_assert(engine_image::FormatEtc1 == int(FMT_ETC1_RGB8_OES));
static_assert(engine_image::FormatShadowArray == int(FMT_SHADOW_ARRAY));
static_assert(engine_image::FormatRg16f == int(FMT_RG16F));
static_assert(engine_image::FormatRgba16f == int(FMT_RGBA16F));
static_assert(engine_image::FormatRgba32f == int(FMT_RGBA32F));
static_assert(engine_image::FormatR32f == int(FMT_R32F));
static_assert(engine_image::FormatR11g11b10f == int(FMT_R11G11B10F));
static_assert(engine_image::FormatBc6h == int(FMT_BC6H));
static_assert(engine_image::FormatBc7 == int(FMT_BC7));
static_assert(engine_image::FormatR8f == int(FMT_R8F));
static_assert(engine_image::FormatDepthStencil == int(FMT_DEPTH_STENCIL));
static_assert(engine_image::FormatRgba16s == int(FMT_RGBA16S));
static_assert(engine_image::ColorDefault == int(CFM_DEFAULT));
static_assert(engine_image::ColorNormalDxt5 == int(CFM_NORMAL_DXT5));
static_assert(engine_image::ColorYCoCgDxt5 == int(CFM_YCOCG_DXT5));
static_assert(engine_image::ColorGreenAlpha == int(CFM_GREEN_ALPHA));
static_assert(engine_image::Type2D == int(DTT_2D));
static_assert(engine_image::TypeCubic == int(DTT_CUBIC));
static_assert(engine_image::Type2DArray == int(DTT_2D_ARRAY));
static_assert(engine_image::UsageSpecular == int(TD_SPECULAR));
static_assert(engine_image::UsageDiffuse == int(TD_DIFFUSE));
static_assert(engine_image::UsageDefault == int(TD_DEFAULT));
static_assert(engine_image::UsageBump == int(TD_BUMP));
static_assert(engine_image::UsageFont == int(TD_FONT));
static_assert(engine_image::UsageLight == int(TD_LIGHT));
static_assert(engine_image::UsageLookupTableMono == int(TD_LOOKUP_TABLE_MONO));
static_assert(engine_image::UsageLookupTableAlpha == int(TD_LOOKUP_TABLE_ALPHA));
static_assert(engine_image::UsageLookupTableRgb1 == int(TD_LOOKUP_TABLE_RGB1));
static_assert(engine_image::UsageLookupTableRgba == int(TD_LOOKUP_TABLE_RGBA));
static_assert(engine_image::UsageCoverage == int(TD_COVERAGE));
static_assert(engine_image::UsageSpecularPbrRmao == int(TD_SPECULAR_PBR_RMAO));
static_assert(engine_image::UsageHdrLightProbe == int(TD_HDR_LIGHTPROBE));
static_assert(engine_image::UsageHdri == int(TD_HDRI));
static_assert(engine_image::FilterLinear == int(TF_LINEAR));
static_assert(engine_image::FilterNearest == int(TF_NEAREST));
static_assert(engine_image::FilterNearestMipmap == int(TF_NEAREST_MIPMAP));
static_assert(engine_image::FilterDefault == int(TF_DEFAULT));
static_assert(engine_image::RepeatRepeat == int(TR_REPEAT));
static_assert(engine_image::RepeatClamp == int(TR_CLAMP));
static_assert(engine_image::RepeatClampToZero == int(TR_CLAMP_TO_ZERO));
static_assert(engine_image::RepeatClampToZeroAlpha == int(TR_CLAMP_TO_ZERO_ALPHA));
static_assert(engine_image::Cube2D == int(CF_2D));
static_assert(engine_image::CubePackedMipChain == int(CF_2D_PACKED_MIPCHAIN));
static_assert(engine_image::CubeNativeHdr == int(CF_NATIVE_HDR));

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
