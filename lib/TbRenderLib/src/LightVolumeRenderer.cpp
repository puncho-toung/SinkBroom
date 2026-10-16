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

#include "render/LightVolumeRenderer.h"

#include "gl/ActiveShader.h"
#include "gl/Camera.h"
#include "gl/GlInterface.h"
#include "gl/PrimType.h"
#include "gl/Shaders.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityRotation.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"

#include "kd/overload.h"

#include "vm/mat.h"
#include "vm/vec.h"

#include <algorithm>
#include <utility>

namespace tb::render
{
namespace
{

bool sameStyle(const LightVolumeStyle& lhs, const LightVolumeStyle& rhs)
{
  return lhs.alphaCap == rhs.alphaCap && lhs.shells == rhs.shells && lhs.twoD == rhs.twoD
         && lhs.outerShellOnly == rhs.outerShellOnly;
}

/**
 * Builds the volumes of the given light entities and their ranges. Every light is parsed
 * first, so that the mesh is reserved once for all of them; the frame is
 * lightFrame(entity, policy) without parsing the entity a second time.
 */
std::pair<LightVolumeMesh, std::vector<LightRange>> buildLights(
  const std::vector<const mdl::EntityNode*>& lights,
  const mdl::EntityRotationPolicy policy,
  const LightVolumeStyle& style,
  const bool selected)
{
  auto ranges = std::vector<LightRange>{};
  ranges.reserve(lights.size());

  auto size = LightVolumeMeshSize{};
  for (const auto* entityNode : lights)
  {
    const auto& entity = entityNode->entity();

    auto range = LightRange{};
    range.node = entityNode;
    range.props = mdl::parseLightProperties(entity);
    range.frame = mdl::LightFrame{
      range.props.origin,
      vm::extract_minor(mdl::entityRotation(entity, policy), 3, 3),
    };
    range.selected = selected;

    const auto lightSize = lightVolumeMeshSize(range.props, style, entity.origin());
    size.triangles += lightSize.triangles;
    size.lines += lightSize.lines;

    ranges.push_back(std::move(range));
  }

  auto mesh = LightVolumeMesh{};
  mesh.triangles.reserve(size.triangles);
  mesh.lines.reserve(size.lines);

  for (std::size_t i = 0; i < lights.size(); ++i)
  {
    auto& range = ranges[i];
    range.fillFirst = mesh.triangles.size();
    range.lineFirst = mesh.lines.size();

    appendLightVolumeMesh(
      mesh, range.props, range.frame, style, lights[i]->entity().origin());

    range.fillCount = mesh.triangles.size() - range.fillFirst;
    range.lineCount = mesh.lines.size() - range.lineFirst;
  }

  return {std::move(mesh), std::move(ranges)};
}

} // namespace

LightVolumeRenderer::FillRenderable::FillRenderable(LightVolumeRenderer& renderer)
  : m_renderer{renderer}
{
}

void LightVolumeRenderer::FillRenderable::prepare(gl::Gl& gl, gl::VboManager& vboManager)
{
  m_renderer.prepareFills(gl, vboManager);
}

void LightVolumeRenderer::FillRenderable::render(RenderContext& renderContext)
{
  m_renderer.renderFills(renderContext);
}

LightVolumeRenderer::LineRenderable::LineRenderable(LightVolumeRenderer& renderer)
  : m_renderer{renderer}
{
}

void LightVolumeRenderer::LineRenderable::prepare(gl::Gl& gl, gl::VboManager& vboManager)
{
  m_renderer.prepareLines(gl, vboManager);
}

void LightVolumeRenderer::LineRenderable::render(RenderContext& renderContext)
{
  m_renderer.renderLines(renderContext);
}

LightVolumeRenderer::LightVolumeRenderer()
  : m_fillRenderable{*this}
  , m_lineRenderable{*this}
{
}

LightVolumeRenderer::~LightVolumeRenderer() = default;

void LightVolumeRenderer::invalidate()
{
  // the nodes may be gone (a new map), so nothing of them is kept
  for (auto* arrays : {&m_unselected, &m_selected})
  {
    arrays->ranges.clear();
    arrays->nodes.clear();
    arrays->dirty = true;
  }
}

void LightVolumeRenderer::invalidateSelection()
{
  m_selected.dirty = true;
  if (m_unselectedStale)
  {
    m_unselected.dirty = true;
  }
}

void LightVolumeRenderer::nodesDidChange(const std::vector<mdl::Node*>& nodes)
{
  for (const auto* node : nodes)
  {
    const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(node);
    if (!entityNode)
    {
      continue;
    }

    const auto inSelected = m_selected.nodes.contains(entityNode);
    const auto inUnselected = m_unselected.nodes.contains(entityNode);
    if (!inSelected && !inUnselected && !mdl::isLightEntity(entityNode->entity()))
    {
      continue;
    }

    if (inSelected || entityNode->transitivelySelected())
    {
      // a drag updates the small selected pair only
      m_selected.dirty = true;
      m_unselectedStale = true;
    }
    else
    {
      m_unselected.dirty = true;
    }
  }
}

void LightVolumeRenderer::collect(
  const mdl::Map& map, LightVolumeVisibility visibility, const LightVolumeStyle& style)
{
  if (!mdl::hasLightEditor(map.gameInfo().gameConfig))
  {
    visibility = LightVolumeVisibility::Off;
  }

  if (visibility != m_visibility || !sameStyle(style, m_style))
  {
    m_visibility = visibility;
    m_style = style;
    invalidate();
  }

  if (m_unselected.dirty)
  {
    rebuildUnselected(map);
  }
  if (m_selected.dirty)
  {
    rebuildSelected(map);
  }
}

const std::vector<LightRange>& LightVolumeRenderer::unselectedRanges() const
{
  return m_unselected.ranges;
}

const std::vector<LightRange>& LightVolumeRenderer::selectedRanges() const
{
  return m_selected.ranges;
}

void LightVolumeRenderer::addFills(RenderBatch& renderBatch)
{
  renderBatch.add(&m_fillRenderable);
}

void LightVolumeRenderer::addLines(RenderBatch& renderBatch)
{
  renderBatch.add(&m_lineRenderable);
}

void LightVolumeRenderer::rebuildUnselected(const mdl::Map& map)
{
  m_unselected.ranges.clear();
  m_unselected.nodes.clear();

  auto lights = std::vector<const mdl::EntityNode*>{};
  if (m_visibility == LightVolumeVisibility::All)
  {
    const auto& editorContext = map.editorContext();
    map.worldNode().accept(kdl::overload(
      [](auto&& thisLambda, const mdl::WorldNode& worldNode) {
        worldNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const mdl::LayerNode& layerNode) {
        layerNode.visitChildren(thisLambda);
      },
      [](auto&& thisLambda, const mdl::GroupNode& groupNode) {
        groupNode.visitChildren(thisLambda);
      },
      [&](const mdl::EntityNode& entityNode) {
        if (editorContext.visible(entityNode) && mdl::isLightEntity(entityNode.entity()))
        {
          lights.push_back(&entityNode);
        }
      },
      [](const mdl::BrushNode&) {},
      [](const mdl::PatchNode&) {}));
  }

  auto style = m_style;
  style.outerShellOnly = true;

  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

  auto [mesh, ranges] = buildLights(lights, policy, style, false);
  m_unselected.ranges = std::move(ranges);
  m_unselected.nodes.insert(lights.begin(), lights.end());

  m_unselected.fills = gl::VertexArray::move(std::move(mesh.triangles));
  m_unselected.lines = gl::VertexArray::move(std::move(mesh.lines));
  m_unselected.dirty = false;
  m_unselectedStale = false;
}

void LightVolumeRenderer::rebuildSelected(const mdl::Map& map)
{
  m_selected.ranges.clear();
  m_selected.nodes.clear();

  auto style = m_style;
  style.outerShellOnly = false;

  const auto policy = map.gameInfo().gameConfig.entityConfig.rotationPolicy;

  auto lights = std::vector<const mdl::EntityNode*>{};
  if (m_visibility != LightVolumeVisibility::Off)
  {
    for (const auto* entityNodeBase : map.selection().allEntities())
    {
      const auto* entityNode = dynamic_cast<const mdl::EntityNode*>(entityNodeBase);
      if (entityNode && mdl::isLightEntity(entityNode->entity()))
      {
        lights.push_back(entityNode);
      }
    }
  }

  auto [mesh, ranges] = buildLights(lights, policy, style, true);
  m_selected.ranges = std::move(ranges);
  m_selected.nodes.insert(lights.begin(), lights.end());

  m_selected.fills = gl::VertexArray::move(std::move(mesh.triangles));
  m_selected.lines = gl::VertexArray::move(std::move(mesh.lines));
  m_selected.dirty = false;
}

void LightVolumeRenderer::prepareFills(gl::Gl& gl, gl::VboManager& vboManager)
{
  m_unselected.fills.prepare(gl, vboManager);
  m_selected.fills.prepare(gl, vboManager);
}

void LightVolumeRenderer::prepareLines(gl::Gl& gl, gl::VboManager& vboManager)
{
  m_unselected.lines.prepare(gl, vboManager);
  m_selected.lines.prepare(gl, vboManager);
}

void LightVolumeRenderer::renderFills(RenderContext& renderContext)
{
  const auto cameraPosition = vm::vec3d{renderContext.camera().position()};
  // the 2D camera is always outside
  const auto containsCamera = [&](const LightRange& range) {
    return renderContext.render3D()
           && mdl::containsPoint(range.props, range.frame, cameraPosition);
  };
  const auto distance = [&](const LightRange& range) {
    return vm::squared_distance(range.frame.origin, cameraPosition);
  };

  m_fillDraws.clear();
  for (const auto& range : m_unselected.ranges)
  {
    if (
      range.fillCount > 0 && !m_selected.nodes.contains(range.node)
      && !containsCamera(range))
    {
      m_fillDraws.push_back(FillDraw{
        distance(range),
        &m_unselected.fills,
        GLint(range.fillFirst),
        GLsizei(range.fillCount),
        false});
    }
  }
  if (!renderContext.hideSelection())
  {
    for (const auto& range : m_selected.ranges)
    {
      if (range.fillCount > 0)
      {
        m_fillDraws.push_back(FillDraw{
          distance(range),
          &m_selected.fills,
          GLint(range.fillFirst),
          GLsizei(range.fillCount),
          containsCamera(range)});
      }
    }
  }

  if (m_fillDraws.empty())
  {
    return;
  }

  // back to front
  std::ranges::stable_sort(m_fillDraws, [](const auto& lhs, const auto& rhs) {
    return lhs.distance > rhs.distance;
  });

  auto& gl = renderContext.gl();

  gl.pushAttrib(GL_POLYGON_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);
  if (renderContext.render2D())
  {
    // brush edges draw over the fills, which are enqueued before the map
    gl.disable(GL_DEPTH_TEST);
  }
  else
  {
    gl.enable(GL_DEPTH_TEST);
  }
  gl.depthMask(GL_FALSE);
  gl.frontFace(GL_CW);
  gl.enable(GL_CULL_FACE);
  gl.polygonMode(GL_FRONT_AND_BACK, GL_FILL);

  {
    auto shader =
      gl::ActiveShader{gl, renderContext.shaderManager(), gl::Shaders::VaryingPCShader};

    auto runStart = m_fillDraws.begin();
    while (runStart != m_fillDraws.end())
    {
      const auto runEnd =
        std::find_if(runStart, m_fillDraws.end(), [&](const auto& fillDraw) {
          return fillDraw.array != runStart->array
                 || fillDraw.cullFront != runStart->cullFront;
        });

      m_firsts.clear();
      m_counts.clear();
      for (auto it = runStart; it != runEnd; ++it)
      {
        m_firsts.push_back(it->first);
        m_counts.push_back(it->count);
      }

      auto& array = *runStart->array;
      gl.cullFace(runStart->cullFront ? GL_FRONT : GL_BACK);
      if (array.setup(gl, shader.program()))
      {
        array.render(
          gl, gl::PrimType::Triangles, m_firsts, m_counts, GLint(m_firsts.size()));
        array.cleanup(gl, shader.program());
      }

      runStart = runEnd;
    }
  }

  gl.popAttrib();
}

void LightVolumeRenderer::renderLines(RenderContext& renderContext)
{
  m_firsts.clear();
  m_counts.clear();
  for (const auto& range : m_unselected.ranges)
  {
    if (range.lineCount > 0 && !m_selected.nodes.contains(range.node))
    {
      m_firsts.push_back(GLint(range.lineFirst));
      m_counts.push_back(GLsizei(range.lineCount));
    }
  }

  const auto drawSelected = !renderContext.hideSelection() && !m_selected.lines.empty();
  if (m_firsts.empty() && !drawSelected)
  {
    return;
  }

  auto& gl = renderContext.gl();

  gl.pushAttrib(GL_POLYGON_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);
  gl.enable(GL_DEPTH_TEST);
  gl.depthMask(GL_FALSE);

  {
    auto shader =
      gl::ActiveShader{gl, renderContext.shaderManager(), gl::Shaders::VaryingPCShader};

    if (!m_firsts.empty() && m_unselected.lines.setup(gl, shader.program()))
    {
      m_unselected.lines.render(
        gl, gl::PrimType::Lines, m_firsts, m_counts, GLint(m_firsts.size()));
      m_unselected.lines.cleanup(gl, shader.program());
    }

    if (drawSelected && m_selected.lines.setup(gl, shader.program()))
    {
      m_selected.lines.render(gl, gl::PrimType::Lines);
      m_selected.lines.cleanup(gl, shader.program());
    }
  }

  gl.popAttrib();
}

} // namespace tb::render
