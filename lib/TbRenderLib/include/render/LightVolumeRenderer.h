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

#include "base/Macros.h"
#include "gl/GlUtils.h"
#include "gl/VertexArray.h"
#include "mdl/Light.h"
#include "mdl/LightVolume.h"
#include "render/LightVolumeMesh.h"
#include "render/Renderable.h"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace tb
{
namespace mdl
{
class EntityNode;
class Map;
class Node;
} // namespace mdl

namespace render
{
class RenderBatch;
class RenderContext;

/**
 * Which lights show their volumes (the preference "render/Show light volumes").
 */
enum class LightVolumeVisibility
{
  Off,
  Selected,
  All,
};

/**
 * One light's part of the vertex arrays of a LightVolumeRenderer. The node is only
 * compared, never dereferenced.
 */
struct LightRange
{
  const mdl::EntityNode* node = nullptr;
  mdl::LightProperties props;
  mdl::LightFrame frame;
  std::size_t fillFirst = 0;
  std::size_t fillCount = 0;
  std::size_t lineFirst = 0;
  std::size_t lineCount = 0;
  bool selected = false;
};

/**
 * Draws the light volumes of one map view (light editor plan, section 3.4).
 *
 * The volumes are cached in two pairs of vertex arrays: the unselected pair holds the
 * outer shell of every visible light in "All" mode, the selected pair the full volumes
 * of the selected lights. A pair is rebuilt only when it is marked dirty; a rebuilt
 * array is a new VertexArray, so an unchanged frame uploads and allocates nothing.
 *
 * Per frame, the fills are sorted back to front and drawn with one multi-draw call per
 * run of equal array and cull face: the back faces are culled for a light whose volume
 * does not contain the camera, the front faces for a selected light whose volume does;
 * an unselected light whose volume contains the camera draws no fill. The unselected
 * pair skips the lights that are selected.
 *
 * The fills and the lines are enqueued separately (addFills, addLines) so that each view
 * draws them at its own point in the frame.
 */
class LightVolumeRenderer
{
private:
  class FillRenderable : public DirectRenderable
  {
  private:
    LightVolumeRenderer& m_renderer;

  public:
    explicit FillRenderable(LightVolumeRenderer& renderer);

    void prepare(gl::Gl& gl, gl::VboManager& vboManager) override;
    void render(RenderContext& renderContext) override;
  };

  class LineRenderable : public DirectRenderable
  {
  private:
    LightVolumeRenderer& m_renderer;

  public:
    explicit LineRenderable(LightVolumeRenderer& renderer);

    void prepare(gl::Gl& gl, gl::VboManager& vboManager) override;
    void render(RenderContext& renderContext) override;
  };

  struct VolumeArrays
  {
    gl::VertexArray fills;
    gl::VertexArray lines;
    std::vector<LightRange> ranges;
    std::unordered_set<const mdl::EntityNode*> nodes;
    bool dirty = true;
  };

  struct FillDraw
  {
    double distance;
    gl::VertexArray* array;
    GLint first;
    GLsizei count;
    bool cullFront;
  };

  VolumeArrays m_unselected;
  VolumeArrays m_selected;
  // a selected light changed while its outer shell is cached in the unselected pair,
  // which is skipped for it until it is deselected
  bool m_unselectedStale = false;

  LightVolumeVisibility m_visibility = LightVolumeVisibility::Off;
  LightVolumeStyle m_style;

  std::vector<FillDraw> m_fillDraws;
  gl::Indices m_firsts;
  gl::Counts m_counts;

  FillRenderable m_fillRenderable;
  LineRenderable m_lineRenderable;

public:
  LightVolumeRenderer();
  ~LightVolumeRenderer();

  deleteCopyAndMove(LightVolumeRenderer);

  /**
   * Marks both pairs dirty and forgets every node (on load, an editor context, node
   * visibility or definitions change, or when nodes are added or removed).
   */
  void invalidate();

  /**
   * Marks the selected pair dirty (on a selection change).
   */
  void invalidateSelection();

  /**
   * Marks the pairs dirty whose lights are among the given changed nodes.
   */
  void nodesDidChange(const std::vector<mdl::Node*>& nodes);

  /**
   * Rebuilds the dirty pairs from the given map. Draws nothing unless the game config
   * sets entities.lightEditor.
   */
  void collect(
    const mdl::Map& map, LightVolumeVisibility visibility, const LightVolumeStyle& style);

  const std::vector<LightRange>& unselectedRanges() const;
  const std::vector<LightRange>& selectedRanges() const;

  void addFills(RenderBatch& renderBatch);
  void addLines(RenderBatch& renderBatch);

private:
  void rebuildUnselected(const mdl::Map& map);
  void rebuildSelected(const mdl::Map& map);

  void prepareFills(gl::Gl& gl, gl::VboManager& vboManager);
  void prepareLines(gl::Gl& gl, gl::VboManager& vboManager);
  void renderFills(RenderContext& renderContext);
  void renderLines(RenderContext& renderContext);
};

} // namespace render
} // namespace tb
