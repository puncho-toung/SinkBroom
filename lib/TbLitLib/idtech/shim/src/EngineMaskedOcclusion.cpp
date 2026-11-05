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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderSystem_init.cpp), and it calls the
// copied neo/libs/moc (Intel's Masked Software Occlusion Culling, Copyright 2017 Intel
// Corporation, Apache-2.0: idtech/engine/LICENSE_EXCEPTIONS.md and libs/moc/license.txt):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP6.7 (C75, D183, P8.16, P8.22): the facade of lit/EngineMaskedOcclusion.h. TB_LIT_MOC
// is defined where TbLitMoc compiles the library (the engine's USE_INTRINSICS_SSE).

#include "lit/EngineMaskedOcclusion.h"

#if defined(TB_LIT_MOC)
#include "MaskedOcclusionCulling.h"
#endif

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define TB_LIT_MOC_MXCSR 1
#endif

namespace tb::lit
{

std::string_view engineMocImplementationName(const EngineMocImplementation implementation)
{
  switch (implementation)
  {
  case EngineMocImplementation::Sse2:
    return "SSE2";
  case EngineMocImplementation::Sse41:
    return "SSE41";
  case EngineMocImplementation::Avx2:
    return "AVX2";
  case EngineMocImplementation::Avx512:
    return "AVX512";
  }
  return "";
}

#if defined(TB_LIT_MOC)

namespace
{

EngineMocResult resultOf(const MaskedOcclusionCulling::CullingResult result)
{
  switch (result)
  {
  case MaskedOcclusionCulling::VISIBLE:
    return EngineMocResult::Visible;
  case MaskedOcclusionCulling::OCCLUDED:
    return EngineMocResult::Occluded;
  case MaskedOcclusionCulling::VIEW_CULLED:
    return EngineMocResult::ViewCulled;
  }
  return EngineMocResult::Visible;
}

MaskedOcclusionCulling::BackfaceWinding windingOf(const EngineMocBackface backface)
{
  switch (backface)
  {
  case EngineMocBackface::None:
    return MaskedOcclusionCulling::BACKFACE_NONE;
  case EngineMocBackface::Cw:
    return MaskedOcclusionCulling::BACKFACE_CW;
  case EngineMocBackface::Ccw:
    return MaskedOcclusionCulling::BACKFACE_CCW;
  }
  return MaskedOcclusionCulling::BACKFACE_NONE;
}

MaskedOcclusionCulling::VertexLayout layoutOf(const EngineMocVertexLayout& layout)
{
  return MaskedOcclusionCulling::VertexLayout{
    layout.stride, layout.offsetY, layout.offsetZw};
}

} // namespace

std::unique_ptr<EngineMaskedOcclusion> EngineMaskedOcclusion::create()
{
  // RenderSystem_init.cpp:2476: maskedOcclusionCulling =
  // MaskedOcclusionCulling::Create();
  auto* moc = MaskedOcclusionCulling::Create();
  if (!moc)
  {
    return nullptr;
  }
  return std::unique_ptr<EngineMaskedOcclusion>{new EngineMaskedOcclusion{moc}};
}

EngineMaskedOcclusion::EngineMaskedOcclusion(MaskedOcclusionCulling* moc)
  : m_moc{moc}
{
}

EngineMaskedOcclusion::~EngineMaskedOcclusion()
{
  // RenderSystem_init.cpp:1964-1976: destroy occlusion culling object and free
  // hierarchical z-buffer
  if (m_moc)
  {
    MaskedOcclusionCulling::Destroy(m_moc);
    m_moc = nullptr;
  }
}

EngineMocImplementation EngineMaskedOcclusion::implementation() const
{
  switch (m_moc->GetImplementation())
  {
  case MaskedOcclusionCulling::SSE2:
    return EngineMocImplementation::Sse2;
  case MaskedOcclusionCulling::SSE41:
    return EngineMocImplementation::Sse41;
  case MaskedOcclusionCulling::AVX2:
    return EngineMocImplementation::Avx2;
  case MaskedOcclusionCulling::AVX512:
    return EngineMocImplementation::Avx512;
  }
  return EngineMocImplementation::Sse2;
}

void EngineMaskedOcclusion::setResolution(
  const unsigned int width, const unsigned int height)
{
  m_moc->SetResolution(width, height);
}

void EngineMaskedOcclusion::resolution(unsigned int& width, unsigned int& height) const
{
  m_moc->GetResolution(width, height);
}

void EngineMaskedOcclusion::setNearClipPlane(const float nearDist)
{
  m_moc->SetNearClipPlane(nearDist);
}

void EngineMaskedOcclusion::clearBuffer()
{
  m_moc->ClearBuffer();
}

EngineMocResult EngineMaskedOcclusion::renderTriangles(
  const float* vertices,
  const unsigned int* triangles,
  const int triangleCount,
  const float* modelToClipMatrix,
  const EngineMocBackface backface,
  const std::uint32_t clipPlanes,
  const EngineMocVertexLayout& layout)
{
  return resultOf(m_moc->RenderTriangles(
    vertices,
    triangles,
    triangleCount,
    modelToClipMatrix,
    windingOf(backface),
    MaskedOcclusionCulling::ClipPlanes(clipPlanes),
    layoutOf(layout)));
}

EngineMocResult EngineMaskedOcclusion::testTriangles(
  const float* vertices,
  const unsigned int* triangles,
  const int triangleCount,
  const float* modelToClipMatrix,
  const EngineMocBackface backface,
  const std::uint32_t clipPlanes,
  const EngineMocVertexLayout& layout)
{
  return resultOf(m_moc->TestTriangles(
    vertices,
    triangles,
    triangleCount,
    modelToClipMatrix,
    windingOf(backface),
    MaskedOcclusionCulling::ClipPlanes(clipPlanes),
    layoutOf(layout)));
}

EngineMocResult EngineMaskedOcclusion::testRect(
  const float xmin,
  const float ymin,
  const float xmax,
  const float ymax,
  const float wmin) const
{
  return resultOf(m_moc->TestRect(xmin, ymin, xmax, ymax, wmin));
}

void EngineMaskedOcclusion::computePixelDepthBuffer(float* depth, const bool flipY)
{
  m_moc->ComputePixelDepthBuffer(depth, flipY);
}

#else

// no library on this processor: nothing is created, so no member is ever called

std::unique_ptr<EngineMaskedOcclusion> EngineMaskedOcclusion::create()
{
  return nullptr;
}

EngineMaskedOcclusion::EngineMaskedOcclusion(MaskedOcclusionCulling* moc)
  : m_moc{moc}
{
}

EngineMaskedOcclusion::~EngineMaskedOcclusion() = default;

EngineMocImplementation EngineMaskedOcclusion::implementation() const
{
  return EngineMocImplementation::Sse2;
}

void EngineMaskedOcclusion::setResolution(unsigned int, unsigned int) {}

void EngineMaskedOcclusion::resolution(unsigned int& width, unsigned int& height) const
{
  width = 0;
  height = 0;
}

void EngineMaskedOcclusion::setNearClipPlane(float) {}

void EngineMaskedOcclusion::clearBuffer() {}

EngineMocResult EngineMaskedOcclusion::renderTriangles(
  const float*,
  const unsigned int*,
  int,
  const float*,
  EngineMocBackface,
  std::uint32_t,
  const EngineMocVertexLayout&)
{
  return EngineMocResult::Visible;
}

EngineMocResult EngineMaskedOcclusion::testTriangles(
  const float*,
  const unsigned int*,
  int,
  const float*,
  EngineMocBackface,
  std::uint32_t,
  const EngineMocVertexLayout&)
{
  return EngineMocResult::Visible;
}

EngineMocResult EngineMaskedOcclusion::testRect(float, float, float, float, float) const
{
  return EngineMocResult::Visible;
}

void EngineMaskedOcclusion::computePixelDepthBuffer(float*, bool) {}

#endif

EngineMocFloatScope::EngineMocFloatScope()
{
#if defined(TB_LIT_MOC_MXCSR)
  // Flush denorms to zero to avoid performance issues with small values
  m_saved = _mm_getcsr();
  _mm_setcsr(m_saved | EngineMocFlushDenormals);
#endif
}

EngineMocFloatScope::~EngineMocFloatScope()
{
#if defined(TB_LIT_MOC_MXCSR)
  _mm_setcsr(m_saved);
#endif
}

std::uint32_t engineMxcsr()
{
#if defined(TB_LIT_MOC_MXCSR)
  return _mm_getcsr();
#else
  return 0;
#endif
}

namespace
{

// the corners of RenderSystem_init.cpp:2125-2236 in the engine's order: center + (mx or
// px) + (my or py) + (mz or pz), w 1
EngineMocCubeVerts cubeVerts(const float low, const float high)
{
  const auto corner = [&](const bool x, const bool y, const bool z) {
    return std::array<float, 4>{
      0.0f + (x ? high : low), 0.0f + (y ? high : low), 0.0f + (z ? high : low), 1.0f};
  };
  return EngineMocCubeVerts{
    corner(false, false, false), // center + mx + my + mz
    corner(true, false, false),  // center + px + my + mz
    corner(true, true, false),   // center + px + py + mz
    corner(false, true, false),  // center + mx + py + mz
    corner(false, false, true),  // center + mx + my + pz
    corner(true, false, true),   // center + px + my + pz
    corner(true, true, true),    // center + px + py + pz
    corner(false, true, true),   // center + mx + py + pz
  };
}

} // namespace

EngineMocCubeVerts engineMocZeroOneCubeVerts()
{
  return cubeVerts(0.0f, 1.0f);
}

EngineMocCubeVerts engineMocUnitCubeVerts()
{
  return cubeVerts(-1.0f, 1.0f);
}

EngineMocCubeIndexes engineMocCubeIndexes()
{
  // bottom, back, left, right, front, top: two triangles each
  return EngineMocCubeIndexes{2, 3, 0, 1, 2, 0, 5, 1, 0, 4, 5, 0, 7, 4, 0, 3, 7, 0,
                              1, 5, 6, 2, 1, 6, 3, 2, 6, 7, 3, 6, 4, 7, 6, 5, 4, 6};
}

} // namespace tb::lit
