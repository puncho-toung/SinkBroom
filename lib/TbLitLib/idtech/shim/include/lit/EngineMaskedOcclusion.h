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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderSystem_init.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

// LP6.7 (C75, D183, Q44, A59, P8.16): the facade over Intel's Masked Software Occlusion
// Culling as the engine vendors it (neo/libs/moc, Copyright 2017 Intel Corporation,
// Apache-2.0: idtech/engine/libs/moc, copied whole and never edited), which TbLitMoc
// builds with the engine CMake's per-file flags. Standard types only; C++17 and C++20.
// Threading: an EngineMaskedOcclusion is its caller's alone (the render thread's: one
// serial buffer, MOC_MULTITHREADED 0 as the engine has it, RenderCommon.h:71); the free
// functions are pure. Where the library is not built (a processor without the engine's
// USE_INTRINSICS_SSE) create() returns null.

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

class MaskedOcclusionCulling;

namespace tb::lit
{

// MaskedOcclusionCulling::Implementation: the instruction set Create() chose
enum class EngineMocImplementation
{
  Sse2,
  Sse41,
  Avx2,
  Avx512,
};

// "SSE2", "SSE41", "AVX2" or "AVX512"
std::string_view engineMocImplementationName(EngineMocImplementation implementation);

// MaskedOcclusionCulling::CullingResult
enum class EngineMocResult
{
  Visible,
  Occluded,
  ViewCulled,
};

// MaskedOcclusionCulling::BackfaceWinding
enum class EngineMocBackface
{
  None,
  Cw,
  Ccw,
};

// MaskedOcclusionCulling::ClipPlanes' CLIP_PLANE_ALL (near, left, right, bottom, top)
inline constexpr auto EngineMocClipAll = std::uint32_t{0x1f};

/**
 * MaskedOcclusionCulling::VertexLayout: the byte stride between vertices and the byte
 * offsets of y and of z (with a matrix) or w (without one) from x. The library's default
 * is (16, 4, 12): (x, y, z, w) clip-space vertices read as (x, y, w).
 */
struct EngineMocVertexLayout
{
  int stride = 16;
  int offsetY = 4;
  int offsetZw = 12;
};

/**
 * One masked occlusion buffer (tr.maskedOcclusionCulling): create is
 * MaskedOcclusionCulling::Create() (RenderSystem_init.cpp:2476: the best SIMD the CPU
 * offers, the library's CPUID dispatch, MaskedOcclusionCulling.cpp:79-93, 482-509), the
 * destructor MaskedOcclusionCulling::Destroy (:1964-1976). The calls are the library's,
 * the matrix a column-major model-to-clip matrix (the engine passes the transposed
 * idRenderMatrix) or null. The caller sets FTZ and DAZ around them (EngineMocFloatScope).
 */
class EngineMaskedOcclusion
{
public:
  static std::unique_ptr<EngineMaskedOcclusion> create();
  ~EngineMaskedOcclusion();

  EngineMaskedOcclusion(const EngineMaskedOcclusion&) = delete;
  EngineMaskedOcclusion& operator=(const EngineMaskedOcclusion&) = delete;

  EngineMocImplementation implementation() const;

  void setResolution(unsigned int width, unsigned int height);
  void resolution(unsigned int& width, unsigned int& height) const;
  void setNearClipPlane(float nearDist);
  void clearBuffer();

  EngineMocResult renderTriangles(
    const float* vertices,
    const unsigned int* triangles,
    int triangleCount,
    const float* modelToClipMatrix,
    EngineMocBackface backface,
    std::uint32_t clipPlanes,
    const EngineMocVertexLayout& layout);
  EngineMocResult testTriangles(
    const float* vertices,
    const unsigned int* triangles,
    int triangleCount,
    const float* modelToClipMatrix,
    EngineMocBackface backface,
    std::uint32_t clipPlanes,
    const EngineMocVertexLayout& layout);
  EngineMocResult testRect(
    float xmin, float ymin, float xmax, float ymax, float wmin) const;
  // the buffer's depth per pixel (width × height floats, the library's 1 / w)
  void computePixelDepthBuffer(float* depth, bool flipY);

private:
  explicit EngineMaskedOcclusion(MaskedOcclusionCulling* moc);

  MaskedOcclusionCulling* m_moc = nullptr;
};

/**
 * RenderSystem_init.cpp:2473-2474: "Flush denorms to zero" - _mm_setcsr( _mm_getcsr() |
 * 0x8040 ), FTZ (bit 15) and DAZ (bit 6). The engine sets them once on its main thread;
 * the preview sets them for the masked occlusion calls alone: the scope sets them and
 * gives back the register it found (P8.22). Nothing where there is no MXCSR.
 */
class EngineMocFloatScope
{
public:
  EngineMocFloatScope();
  ~EngineMocFloatScope();

  EngineMocFloatScope(const EngineMocFloatScope&) = delete;
  EngineMocFloatScope& operator=(const EngineMocFloatScope&) = delete;

private:
  std::uint32_t m_saved = 0;
};

inline constexpr auto EngineMocFlushDenormals = std::uint32_t{0x8040};

// _mm_getcsr(): the calling thread's MXCSR (0 where there is none); for the checks
std::uint32_t engineMxcsr();

// a cube's eight (x, y, z, 1) corners and its twelve triangles (36 indexes)
using EngineMocCubeVerts = std::array<std::array<float, 4>, 8>;
using EngineMocCubeIndexes = std::array<unsigned int, 36>;

// R_MakeZeroOneCubeTrisForMaskedOcclusionCulling (RenderSystem_init.cpp:2125-2202): the
// zero-to-one cube [0, 1]³ the light test transforms (tr.maskedZeroOneCubeVerts), and
// its triangles, which the unit cube's test takes too (tr.maskedZeroOneCubeIndexes)
EngineMocCubeVerts engineMocZeroOneCubeVerts();
EngineMocCubeIndexes engineMocCubeIndexes();
// R_MakeUnitCubeTrisForMaskedOcclusionCulling (:2204-2236): the unit cube [-1, 1]³ the
// surface test transforms (tr.maskedUnitCubeVerts)
EngineMocCubeVerts engineMocUnitCubeVerts();

} // namespace tb::lit
