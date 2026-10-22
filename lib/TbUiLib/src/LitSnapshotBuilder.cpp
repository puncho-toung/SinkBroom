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

#include "ui/LitSnapshotBuilder.h"

#include "lit/NumberText.h"
#include "mdl/BezierPatch.h"
#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Light.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/UvAttributes.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace tb::ui
{
namespace
{

bool anyChildVisibleInPreview(const mdl::EditorContext& context, const mdl::Node& node)
{
  return std::ranges::any_of(node.children(), [&](const auto* child) {
    return visibleInPreview(context, *child);
  });
}

// R3.10: the serialiser's text of the value (fmt's "{}", formatted into a buffer on the
// stack, R2.13) as the engine's idLexer reads it: NumberValue's double, then its float
template <typename T>
float readBack(const T value)
{
  auto buffer = std::array<char, 64>{};
  const auto written = fmt::format_to_n(buffer.data(), buffer.size(), "{}", value);
  const auto text =
    std::string_view{buffer.data(), std::min(written.size, buffer.size())};
  return lit::lexerFloat(text).value_or(float(value));
}

std::uint32_t count(const std::size_t size)
{
  return std::uint32_t(size);
}

} // namespace

bool visibleInPreview(const mdl::EditorContext& context, const mdl::Node& node)
{
  // EditorContext::visible, case by case, without `if (node.selected()) return true;`
  return node.accept(kdl::overload(
    [](const mdl::WorldNode& worldNode) { return worldNode.visible(); },
    [](const mdl::LayerNode& layerNode) { return layerNode.visible(); },
    [&](const mdl::GroupNode& groupNode) {
      return anyChildVisibleInPreview(context, groupNode) && groupNode.visible();
    },
    [&](const mdl::EntityNode& entityNode) {
      if (!entityNode.entity().pointEntity())
      {
        return anyChildVisibleInPreview(context, entityNode);
      }
      return entityNode.visible() && context.showPointEntities()
             && !context.entityDefinitionHidden(entityNode);
    },
    [&](const mdl::BrushNode& brushNode) {
      if (
        !context.showBrushes() || brushNode.hasTag(context.hiddenTags())
        || brushNode.allFacesHaveAnyTagInMask(context.hiddenTags()))
      {
        return false;
      }
      if (const auto* entityNode = brushNode.entity();
          entityNode && context.entityDefinitionHidden(*entityNode))
      {
        return false;
      }
      return brushNode.visible();
    },
    [&](const mdl::PatchNode& patchNode) {
      return context.showPatches() && !patchNode.hasTag(context.hiddenTags())
             && patchNode.visible();
    }));
}

lit::SnapshotNodeKind pointEntityKind(const mdl::Entity& entity)
{
  return mdl::isLightEntity(entity) ? lit::SnapshotNodeKind::Light
                                    : lit::SnapshotNodeKind::PointEntity;
}

float serialisedFloat(const double value)
{
  return readBack(value);
}

float serialisedFloat(const float value)
{
  return readBack(value);
}

vm::vec3d serialisedPoint(const vm::vec3d& point)
{
  return vm::vec3d{
    double(serialisedFloat(point.x())),
    double(serialisedFloat(point.y())),
    double(serialisedFloat(point.z()))};
}

SerialisedValveTexture serialisedValveTexture(const mdl::BrushFace& face)
{
  const auto uAxis = face.uAxis();
  const auto vAxis = face.vAxis();
  const auto uvAttributes = face.uvAttributes();
  return {
    vm::vec3f{
      serialisedFloat(uAxis.x()), serialisedFloat(uAxis.y()), serialisedFloat(uAxis.z())},
    vm::vec3f{
      serialisedFloat(vAxis.x()), serialisedFloat(vAxis.y()), serialisedFloat(vAxis.z())},
    vm::vec2f{
      serialisedFloat(uvAttributes.offset.x()), serialisedFloat(uvAttributes.offset.y())},
    vm::vec2f{
      serialisedFloat(uvAttributes.scale.x()), serialisedFloat(uvAttributes.scale.y())},
  };
}

LitSnapshotBuilder::LitSnapshotBuilder(
  const std::uint64_t generation, const std::uint64_t sequence)
{
  m_snapshot.generation = generation;
  m_snapshot.sequence = sequence;
}

void LitSnapshotBuilder::addBrush(
  const lit::PreviewId id,
  const lit::PreviewId entity,
  const bool visible,
  const bool active,
  const mdl::BrushNode& brushNode)
{
  const auto firstFace = count(m_snapshot.faces.size());
  for (const auto& face : brushNode.brush().faces())
  {
    const auto polygon = face.polygon();
    const auto firstVertex = count(m_snapshot.vertices.size());
    m_snapshot.vertices.insert(
      m_snapshot.vertices.end(), polygon.vertices().begin(), polygon.vertices().end());

    // as MapFileSerializer writes the name
    const auto& materialName =
      face.materialName().empty() ? mdl::BrushFace::NoMaterialName : face.materialName();
    const auto texture = serialisedValveTexture(face);
    auto points = face.points();
    for (auto& point : points)
    {
      point = serialisedPoint(point);
    }
    m_snapshot.faces.push_back(lit::SnapshotFace{
      lit::SnapshotRange{firstVertex, count(polygon.vertices().size())},
      points,
      face.boundary(),
      addString(materialName),
      texture.uAxis,
      texture.vAxis,
      texture.offset,
      texture.scale,
    });
  }

  m_snapshot.nodes.push_back(lit::SnapshotNode{
    id,
    entity,
    lit::SnapshotNodeKind::Brush,
    visible,
    lit::SnapshotRange{firstFace, count(m_snapshot.faces.size()) - firstFace},
    active,
  });
}

void LitSnapshotBuilder::addPatch(
  const lit::PreviewId id,
  const lit::PreviewId entity,
  const bool visible,
  const bool active,
  const mdl::PatchNode& patchNode)
{
  const auto& patch = patchNode.patch();
  const auto& controlPoints = patch.controlPoints();
  const auto firstPoint = count(m_snapshot.controlPoints.size());
  for (const auto& point : controlPoints)
  {
    // R3.10: as the engine's idLexer reads the serialiser's text back
    auto value = vm::vec<double, 5>{};
    for (auto k = std::size_t{0}; k < 5; ++k)
    {
      value[k] = double(serialisedFloat(point[k]));
    }
    m_snapshot.controlPoints.push_back(value);
  }

  const auto& subdivisions = patch.explicitSubdivisions();
  const auto patchIndex = count(m_snapshot.patches.size());
  m_snapshot.patches.push_back(lit::SnapshotPatch{
    addString(patch.materialName()),
    count(patch.pointRowCount()),
    count(patch.pointColumnCount()),
    lit::SnapshotRange{firstPoint, count(controlPoints.size())},
    subdivisions.has_value(),
    subdivisions ? count(subdivisions->horizontal) : 0u,
    subdivisions ? count(subdivisions->vertical) : 0u,
  });

  m_snapshot.nodes.push_back(lit::SnapshotNode{
    id,
    entity,
    lit::SnapshotNodeKind::Patch,
    visible,
    lit::SnapshotRange{patchIndex, 1},
    active,
  });
}

std::uint32_t LitSnapshotBuilder::addEntity(
  const lit::PreviewId id, const mdl::Entity& entity)
{
  const auto firstKey = count(m_snapshot.keyValues.size());
  for (const auto& property : entity.properties())
  {
    const auto key = addString(property.key());
    const auto value = addString(property.value());
    m_snapshot.keyValues.push_back(lit::SnapshotKeyValue{key, value});
  }

  const auto classname = addString(entity.classname());
  const auto index = count(m_snapshot.entities.size());
  m_snapshot.entities.push_back(lit::SnapshotEntity{
    id,
    classname,
    lit::SnapshotRange{firstKey, count(m_snapshot.keyValues.size()) - firstKey},
  });
  return index;
}

void LitSnapshotBuilder::addPointEntity(
  const lit::PreviewId id,
  const lit::SnapshotNodeKind kind,
  const bool visible,
  const bool active,
  const mdl::Entity& entity)
{
  const auto index = addEntity(id, entity);
  m_snapshot.nodes.push_back(lit::SnapshotNode{
    id,
    id,
    kind,
    visible,
    lit::SnapshotRange{index, 1},
    active,
  });
}

void LitSnapshotBuilder::addRemoved(const lit::PreviewId id)
{
  m_snapshot.removed.push_back(id);
}

void LitSnapshotBuilder::setMapPath(std::string mapPath)
{
  m_snapshot.mapPath = std::move(mapPath);
}

bool LitSnapshotBuilder::empty() const
{
  return m_snapshot.nodes.empty() && m_snapshot.removed.empty()
         && m_snapshot.entities.empty() && !m_snapshot.mapPath;
}

const lit::Snapshot& LitSnapshotBuilder::snapshot() const
{
  return m_snapshot;
}

lit::Snapshot LitSnapshotBuilder::take()
{
  m_strings.clear();
  return std::exchange(m_snapshot, lit::Snapshot{});
}

lit::SnapshotString LitSnapshotBuilder::addString(const std::string_view string)
{
  auto key = std::string{string};
  if (const auto it = m_strings.find(key); it != m_strings.end())
  {
    return it->second;
  }
  const auto result =
    lit::SnapshotString{count(m_snapshot.strings.size()), count(string.size())};
  m_snapshot.strings.append(string);
  m_strings.emplace(std::move(key), result);
  return result;
}

} // namespace tb::ui
