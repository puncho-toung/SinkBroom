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
// GPL Source Code and RBDOOM-3-BFG
// (neo/renderer/tr_frontend_masked_occlusion_culling.cpp,
// renderer/tr_frontend_addlights.cpp and renderer/tr_frontend_addmodels.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "LitView.h"
#include "lit/EngineLight.h"
#include "lit/EngineMaskedOcclusion.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tb::lit
{
struct Chunk;
struct LitSurface;

/**
 * FAL:367-371: a spot light's TestRect depth, wmin / ( 1 - zmin ) in the engine's double
 * steps, then Max( wmin2, 0.0f ) (F3.11: +0 for a -0 or a NaN, as the engine's macro
 * gives). Pure.
 */
float engineMocRectWmin(float scissorZmin, float wmin);

/**
 * LP6.7 (C75, D183, P8.15, P8.22): the lists' masked occlusion culling, the engine's
 * r_useMaskedOcclusionCulling 1 path on one buffer of Intel's library (the copy in
 * idtech/engine/libs/moc, through lit/EngineMaskedOcclusion.h), serial as the engine's
 * MOC_MULTITHREADED 0. The frontend calls it once per lists build: cullSurfaces fills the
 * buffer with the static world's occluders it is given in their order
 * (R_FillMaskedOcclusionBufferWithModels, tr_frontend_masked_occlusion_culling.cpp
 * :534-621, the occluders' call :356-381) and tests every listed surface
 * (R_AddSingleModel, tr_frontend_addmodels.cpp:660-727); lightVisible tests a light whose
 * volume does not contain the view (R_AddSingleLight, tr_frontend_addlights.cpp:286-388).
 * Each sets FTZ | DAZ in the MXCSR for its calls and gives the register back (the engine
 * sets them once, RenderSystem_init.cpp:2473-2474). The occluders' 32-bit index copies
 * (RenderTriangles takes unsigned int, the chunks hold uint16, ChunkBuilder.h:188) are
 * made at a chunk's first fill and kept here beside the chunk, by its address, until a
 * fill of a new chunks version no longer meets it (the engine's tri->mocIndexes,
 * R_CreateMaskedOcclusionCullingTris, kept with its triangles). The buffer is made at the
 * first build (Create under FTZ | DAZ, RenderSystem_init.cpp:2473-2476) and freed by
 * release, which a session's Stop calls (LitPasses::releaseAll). Render thread only.
 */
class MaskedOcclusion
{
public:
  // what one build's fill and surface tests did
  struct SurfaceCull
  {
    std::size_t occluders = 0;
    std::size_t occluderTriangles = 0;
    // the occluders' sub-cell runs rasterized, and those whose every triangle was outside
    // the view or back-facing (RenderTriangles' VIEW_CULLED)
    std::size_t runs = 0;
    std::size_t runsViewCulled = 0;
    std::size_t tests = 0;
    std::size_t culled = 0;
    // F2.11 (2026-11-05): the fill's time (the engine's pc.mocMicroSec) and the surface
    // tests', in milliseconds
    double fillMs = 0.0;
    double testMs = 0.0;
  };

  MaskedOcclusion();
  ~MaskedOcclusion();

  MaskedOcclusion(const MaskedOcclusion&) = delete;
  MaskedOcclusion& operator=(const MaskedOcclusion&) = delete;

  /**
   * TMOC:546-577 then the fill and FAM's tests: the buffer at ⌊width / 2⌋ rounded up
   * to a multiple of 8 by ⌊height / 2⌋ rounded up to a multiple of 4, the near plane
   * r_znear (cramZNear never), cleared; then the surfaces whose occluder flag is set
   * rasterized
   * through RenderTriangles( the chunk's idDrawVerts, its 32-bit indexes, the transposed
   * unjittered mvp of its space, BACKFACE_CCW, CLIP_PLANE_ALL, VertexLayout( sizeof(
   * idDrawVert ), 4, 8 ) ), run by sub-cell run, the runs front to back (the recorded
   * order, P8.15: by the squared distance from the view origin to the run's bounds, ties
   * in the surfaces' order then the runs'); then every surface tested unless the
   * view is inside its bounds: culled[i] true where the buffer hides it. chunksVersion
   * keeps the index copies. Nothing (nullopt, culled untouched) where the library is not
   * built, or (F3.10) where a buffer side is 0 (a view side of 1).
   */
  std::optional<SurfaceCull> cullSurfaces(
    const LitView& view,
    std::uint64_t chunksVersion,
    const std::vector<LitSurface>& surfaces,
    const std::vector<LitSpace>& spaces,
    const std::vector<char>& occluder,
    std::vector<char>& culled);

  /**
   * FAL:291-388 for a light whose volume does not contain the view (the caller's
   * viewInsideLight, :286): the zero-to-one cube through scaledInverseBaseLightProject ·
   * the unjittered MVP, its 3 × 3 part scaled by 0.99 and its translation not (E-LIT-16:
   * the tested box 1 % smaller, shrunk toward the image of the cube's origin, as the
   * engine has it); a point or parallel light's twelve triangles through TestTriangles(
   * BACKFACE_NONE ), a spot light's scissor through TestRect in NDC with wmin / ( 1 -
   * zmin ). True when VISIBLE; ms gains the call's time. Call it only after cullSurfaces
   * filled the buffer this build.
   */
  bool lightVisible(
    const LitView& view,
    const EngineMat4& inverseBaseLightProject,
    bool pointOrParallel,
    const ScreenRect& scissor,
    double& ms);

  // tests, after a build: the buffer's resolution and its depth per pixel (1 / w, -1
  // where nothing was rasterized), rows bottom up; empty before the buffer is made
  std::vector<float> depthForTesting(unsigned int& width, unsigned int& height);
  // the buffer's instruction set ("SSE2", "SSE41", "AVX2", "AVX512"), "" before it is
  // made
  std::string_view implementationName() const;
  // the thread of the last fill (tests: the render thread's)
  std::thread::id lastThread() const;
  // the occluders' index copies kept, and their bytes
  std::size_t indexCopies() const;
  std::uint64_t indexCopyBytes() const;
  // F3.20 (2026-11-05, tests): the spot lights' TestRect calls since construction
  std::uint64_t rectTests() const;
  // Stop: the buffer and the copies freed (the next build makes them again)
  void release();

private:
  struct OccluderIndexes
  {
    std::uint64_t chunksVersion = 0;
    std::vector<unsigned int> indexes;
  };

  const std::vector<unsigned int>& indexesOf(const Chunk& chunk);

  std::unique_ptr<EngineMaskedOcclusion> m_moc;
  bool m_unavailable = false;
  std::unordered_map<const Chunk*, OccluderIndexes> m_indexes;
  std::uint64_t m_chunksVersion = 0;
  std::thread::id m_thread;
  std::uint64_t m_rectTests = 0;
  EngineMocCubeVerts m_zeroOneCube;
  EngineMocCubeVerts m_unitCube;
  EngineMocCubeIndexes m_cubeIndexes;
};

} // namespace tb::lit
