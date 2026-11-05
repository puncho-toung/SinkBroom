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
// renderer/tr_frontend_addlights.cpp, renderer/tr_frontend_addmodels.cpp and
// idlib/geometry/RenderMatrix.h):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "MaskedOcclusion.h"

#include "LitFrontend.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineTables.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>

namespace tb::lit
{
namespace
{

using MocClock = std::chrono::steady_clock;

double millisecondsSince(const MocClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(MocClock::now() - start).count();
}

// a chunk's idDrawVerts as RenderTriangles reads them: x at 0, y at 4, z at 8 (with a
// matrix the library reads z, VertexLayout's offsetZ), the vertex's 32 bytes apart
static_assert(sizeof(EngineDrawVert) == 32);
static_assert(offsetof(EngineDrawVert, xyz) == 0);
constexpr auto DrawVertLayout = EngineMocVertexLayout{int(sizeof(EngineDrawVert)), 4, 8};

// idRenderMatrix::TransformPoint( const idVec4& in, idVec4& out ) (RenderMatrix.h:
// 358-366): the rows times (x, y, z, w), in the engine's order of the sums
std::array<float, 4> transformPoint4(
  const EngineMat4& matrix, const std::array<float, 4>& in)
{
  auto out = std::array<float, 4>{};
  for (std::size_t r = 0; r < 4; ++r)
  {
    out[r] = in[0] * matrix[r * 4 + 0] + in[1] * matrix[r * 4 + 1]
             + in[2] * matrix[r * 4 + 2] + in[3] * matrix[r * 4 + 3];
  }
  return out;
}

// idBounds::ContainsPoint: inside or on the bounds
bool boundsContainPoint(const EngineBounds& bounds, const EngineVec3& point)
{
  return !(
    point[0] < bounds[0][0] || point[1] < bounds[0][1] || point[2] < bounds[0][2]
    || point[0] > bounds[1][0] || point[1] > bounds[1][1] || point[2] > bounds[1][2]);
}

} // namespace

MaskedOcclusion::MaskedOcclusion()
  : m_zeroOneCube{engineMocZeroOneCubeVerts()}
  , m_unitCube{engineMocUnitCubeVerts()}
  , m_cubeIndexes{engineMocCubeIndexes()}
{
}

MaskedOcclusion::~MaskedOcclusion()
{
  release();
}

const std::vector<unsigned int>& MaskedOcclusion::indexesOf(const Chunk& chunk)
{
  // R_CreateMaskedOcclusionCullingTris: the uint16 indexes as unsigned int, once per
  // chunk; after a new chunks version an address met again keeps its copy only when its
  // indexes are the same (another chunk may have been made there)
  const auto [it, made] = m_indexes.try_emplace(&chunk);
  auto& entry = it->second;
  if (
    made
    || (entry.chunksVersion != m_chunksVersion
        && !std::equal(
          chunk.indexes.begin(),
          chunk.indexes.end(),
          entry.indexes.begin(),
          entry.indexes.end())))
  {
    entry.indexes.assign(chunk.indexes.begin(), chunk.indexes.end());
  }
  entry.chunksVersion = m_chunksVersion;
  return entry.indexes;
}

std::optional<MaskedOcclusion::SurfaceCull> MaskedOcclusion::cullSurfaces(
  const LitView& view,
  const std::uint64_t chunksVersion,
  const std::vector<LitSurface>& surfaces,
  const std::vector<LitSpace>& spaces,
  const std::vector<char>& occluder,
  std::vector<char>& culled)
{
  if (m_unavailable)
  {
    return std::nullopt;
  }
  const auto start = MocClock::now();
  m_thread = std::this_thread::get_id();
  // RenderSystem_init.cpp:2473-2476: denormals flushed, then the buffer; here around
  // every call and given back after them
  const auto floatScope = EngineMocFloatScope{};
  if (!m_moc)
  {
    m_moc = EngineMaskedOcclusion::create();
    if (!m_moc)
    {
      m_unavailable = true;
      return std::nullopt;
    }
  }
  // the copies of chunks no fill of this version met are dropped once the version moved
  // (their chunks may be gone: the map is never read through a key)
  const auto newVersion = chunksVersion != m_chunksVersion;
  m_chunksVersion = chunksVersion;

  // TMOC:548-567: reduce to half res, the width a multiple of 8, the height of 4; the
  // near plane r_znear (cramZNear is never set in the preview)
  auto viewWidth = int(std::floor(float(view.width) * 0.5f));
  auto viewHeight = int(std::floor(float(view.height) * 0.5f));
  if (viewWidth & 7)
  {
    // must be multiple of 8
    viewWidth = (viewWidth + 7) & ~7;
  }
  if (viewHeight & 3)
  {
    // must be multiple of 4
    viewHeight = (viewHeight + 3) & ~3;
  }
  const auto zNear = EngineFrame.znear;
  m_moc->setResolution(unsigned(viewWidth), unsigned(viewHeight));
  m_moc->setNearClipPlane(zNear);
  m_moc->clearBuffer();

  auto result = SurfaceCull{};
  // TMOC:356-381: RenderTriangles( mocVerts, mocIndexes, numIndexes / 3, transposed
  // unjitteredMVP, BACKFACE_CCW, CLIP_PLANE_ALL, VertexLayout( 16, 4, 8 ) ), here the
  // chunk's own idDrawVerts. The recorded order (P8.15): the occluders' sub-cell runs
  // (Chunk::ranges, the whole chunk without them) front to back by the squared distance
  // from the view origin to their bounds, ties in the surfaces' order then the runs'. The
  // library's QUICK_MASK merge keeps a working layer's farthest depth, so a far wall
  // drawn before a near one leaks through it (libs/moc/README.md, "render order": front
  // to back is the best order); the engine's area models are the flow's, the preview's
  // chunks without a tree every world chunk in the frustum
  struct Run
  {
    float distance = 0.0f;
    std::size_t surface = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
  };
  auto runs = std::vector<Run>{};
  auto mvps = std::vector<EngineMat4>(surfaces.size());
  const auto distanceTo = [](const EngineBounds& bounds, const EngineVec3& point) {
    auto squared = 0.0f;
    for (std::size_t k = 0; k < 3; ++k)
    {
      const auto d = std::max({bounds[0][k] - point[k], 0.0f, point[k] - bounds[1][k]});
      squared += d * d;
    }
    return squared;
  };
  for (std::size_t i = 0; i < surfaces.size(); ++i)
  {
    if (!occluder[i])
    {
      continue;
    }
    const auto& chunk = *surfaces[i].chunk;
    if (chunk.verts.empty() || chunk.indexes.size() < 3)
    {
      continue;
    }
    const auto& space = spaces[surfaces[i].space];
    mvps[i] = engineMatrixTranspose(space.mvp);
    const auto origin = engineGlobalPointToLocal(space.modelMatrix, view.viewOrigin);
    ++result.occluders;
    result.occluderTriangles += chunk.indexes.size() / 3;
    if (chunk.ranges.empty())
    {
      runs.push_back(
        Run{distanceTo(chunk.bounds, origin), i, 0, std::uint32_t(chunk.indexes.size())});
      continue;
    }
    for (const auto& range : chunk.ranges)
    {
      runs.push_back(
        Run{distanceTo(range.bounds, origin), i, range.firstIndex, range.indexCount});
    }
  }
  std::ranges::stable_sort(
    runs, [](const Run& lhs, const Run& rhs) { return lhs.distance < rhs.distance; });
  for (const auto& run : runs)
  {
    const auto& chunk = *surfaces[run.surface].chunk;
    const auto& indexes = indexesOf(chunk);
    const auto triangles = int(run.indexCount / 3);
    if (triangles == 0 || std::size_t(run.firstIndex) + run.indexCount > indexes.size())
    {
      continue;
    }
    const auto rendered = m_moc->renderTriangles(
      chunk.verts.front().xyz.data(),
      indexes.data() + run.firstIndex,
      triangles,
      mvps[run.surface].data(),
      EngineMocBackface::Ccw,
      EngineMocClipAll,
      DrawVertLayout);
    result.runsViewCulled += rendered == EngineMocResult::ViewCulled ? 1 : 0;
    ++result.runs;
  }

  // FAM:660-727: every surface; the unit cube over its bounds through its model matrix
  // and the unjittered MVP, BACKFACE_NONE, skipped with the view inside its bounds (no
  // weapon or model depth hack, mirror or subview in the preview)
  culled.assign(surfaces.size(), 0);
  for (std::size_t i = 0; i < surfaces.size(); ++i)
  {
    const auto& surface = surfaces[i];
    const auto& space = spaces[surface.space];
    const auto& bounds = surface.chunk->bounds;
    const auto localViewOrigin =
      engineGlobalPointToLocal(space.modelMatrix, view.viewOrigin);
    if (boundsContainPoint(bounds, localViewOrigin))
    {
      continue;
    }
    const auto inverseBaseModelProject =
      engineOffsetScaleForBounds(space.modelRenderMatrix, bounds);
    const auto invProjectMVPMatrix =
      engineMatrixMultiply(view.worldMvp, inverseBaseModelProject);
    auto triVerts = std::array<std::array<float, 4>, 8>{};
    for (std::size_t v = 0; v < 8; ++v)
    {
      // transform to clip space
      triVerts[v] = transformPoint4(invProjectMVPMatrix, m_unitCube[v]);
    }
    ++result.tests;
    const auto visible = m_moc->testTriangles(
                           triVerts.front().data(),
                           m_cubeIndexes.data(),
                           12,
                           nullptr,
                           EngineMocBackface::None,
                           EngineMocClipAll,
                           EngineMocVertexLayout{})
                         == EngineMocResult::Visible;
    if (!visible)
    {
      culled[i] = 1;
      ++result.culled;
    }
  }

  if (newVersion)
  {
    std::erase_if(m_indexes, [&](const auto& entry) {
      return entry.second.chunksVersion != m_chunksVersion;
    });
  }
  result.ms = millisecondsSince(start);
  return result;
}

bool MaskedOcclusion::lightVisible(
  const LitView& view,
  const EngineMat4& inverseBaseLightProject,
  const bool pointOrParallel,
  const ScreenRect& scissor,
  double& ms)
{
  if (!m_moc)
  {
    return true;
  }
  const auto start = MocClock::now();
  const auto floatScope = EngineMocFloatScope{};

  // draw light volume 1 percentage bigger to avoid flickering right before entering the
  // volume with the camera (FAL:301-315: the 3 x 3 part alone, so the box is 1 %
  // smaller, toward the image of the cube's origin: E-LIT-16, mirrored)
  constexpr auto mocLightScale = 0.99f;
  auto scaledInverseBaseLightProject = inverseBaseLightProject;
  for (std::size_t r = 0; r < 3; ++r)
  {
    for (std::size_t c = 0; c < 3; ++c)
    {
      scaledInverseBaseLightProject[r * 4 + c] *= mocLightScale;
    }
  }
  const auto invProjectMVPMatrix =
    engineMatrixMultiply(view.worldMvp, scaledInverseBaseLightProject);

  auto wmin = std::numeric_limits<float>::infinity();
  // NOTE: zeroToOne cube is only for lights and models need the unit cube
  auto triVerts = std::array<std::array<float, 4>, 8>{};
  for (std::size_t i = 0; i < 8; ++i)
  {
    // transform to clip space
    triVerts[i] = transformPoint4(invProjectMVPMatrix, m_zeroOneCube[i]);
    const auto w = triVerts[i][3];
    if (i == 0)
    {
      wmin = w;
    }
    else if (w < wmin)
    {
      wmin = w;
    }
  }

  auto result = EngineMocResult::Visible;
  if (pointOrParallel)
  {
    // backface none so objects are still visible where we run into
    result = m_moc->testTriangles(
      triVerts.front().data(),
      m_cubeIndexes.data(),
      12,
      nullptr,
      EngineMocBackface::None,
      EngineMocClipAll,
      EngineMocVertexLayout{});
  }
  else
  {
    // scissor test alternative: the scissor rectangle has GL convention and starts in
    // the lower left corner, converted to NDC values (FAL:362-386; the viewport is (0,
    // 0, width - 1, height - 1))
    const auto screenWidth = float(view.width - 1) - 0.0f;
    const auto screenHeight = float(view.height - 1) - 0.0f;
    const auto x1 = -1.0f + (float(scissor.x1) / screenWidth) * 2.0f;
    const auto x2 = -1.0f + (float(scissor.x2) / screenWidth) * 2.0f;
    const auto y1 = -1.0f + (float(scissor.y1) / screenHeight) * 2.0f;
    const auto y2 = -1.0f + (float(scissor.y2) / screenHeight) * 2.0f;

    auto zmin = scissor.zmin;
    zmin = float(1.0 - double(zmin)); // reverse depth
    auto wmin2 = float(1.0 / double(zmin));
    wmin2 *= wmin;
    wmin2 = std::max(wmin2, 0.0f);

    result = m_moc->testRect(x1, y1, x2, y2, wmin2);
  }
  ms += millisecondsSince(start);
  return result == EngineMocResult::Visible;
}

std::vector<float> MaskedOcclusion::depthForTesting(
  unsigned int& width, unsigned int& height)
{
  width = 0;
  height = 0;
  if (!m_moc)
  {
    return {};
  }
  const auto floatScope = EngineMocFloatScope{};
  m_moc->resolution(width, height);
  auto depth = std::vector<float>(std::size_t(width) * height);
  m_moc->computePixelDepthBuffer(depth.data(), false);
  return depth;
}

std::string_view MaskedOcclusion::implementationName() const
{
  return m_moc ? engineMocImplementationName(m_moc->implementation())
               : std::string_view{};
}

std::thread::id MaskedOcclusion::lastThread() const
{
  return m_thread;
}

std::size_t MaskedOcclusion::indexCopies() const
{
  return m_indexes.size();
}

std::uint64_t MaskedOcclusion::indexCopyBytes() const
{
  auto bytes = std::uint64_t{0};
  for (const auto& [chunk, entry] : m_indexes)
  {
    bytes += entry.indexes.size() * sizeof(unsigned int);
  }
  return bytes;
}

void MaskedOcclusion::release()
{
  if (m_moc)
  {
    const auto floatScope = EngineMocFloatScope{};
    m_moc.reset();
  }
  m_indexes.clear();
  m_chunksVersion = 0;
}

} // namespace tb::lit
