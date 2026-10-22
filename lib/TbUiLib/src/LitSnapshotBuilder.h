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

#include "lit/Snapshot.h"

#include "vm/vec.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tb::mdl
{
class BrushFace;
class BrushNode;
class EditorContext;
class Entity;
class Node;
class PatchNode;
} // namespace tb::mdl

namespace tb::ui
{

/**
 * D65, A8: EditorContext::visible (TbMdlLib's EditorContext.cpp) without its selection
 * override, so that a filtered node stays hidden in the preview when it is selected, as
 * in the game. It follows EditorContext::visible case by case with the selected()
 * early-outs left out: a hidden layer, group or node (Node::visible, inherited), the
 * hidden tags, a hidden entity definition and the show filters hide a node; a group or a
 * brush entity is visible when any child is. EditorContext::visible does not consult the
 * current group, so neither does this. Kept here, beside the snapshot builder, because no
 * upstream class is edited for the preview (C12). GUI thread.
 */
bool visibleInPreview(const mdl::EditorContext& context, const mdl::Node& node);

// a point entity's node kind: Light for what the light editor edits (mdl::isLightEntity)
lit::SnapshotNodeKind pointEntityKind(const mdl::Entity& entity);

/**
 * C23, P.19: one number as the map serialiser writes it (fmt's "{}", the shortest text
 * that reads back to the value in its own type) and the engine's idLexer reads it back: a
 * double, then a float.
 */
float serialisedFloat(double value);
float serialisedFloat(float value);

// C23, P.19: a face's Valve 220 texture values as the serialiser writes them, as floats
struct SerialisedValveTexture
{
  vm::vec3f uAxis = vm::vec3f{0, 0, 0};
  vm::vec3f vAxis = vm::vec3f{0, 0, 0};
  vm::vec2f offset = vm::vec2f{0, 0};
  vm::vec2f scale = vm::vec2f{1, 1};
};

/**
 * MapFileSerializer's writeValveMaterialInfo: BrushFace::uAxis and vAxis (doubles), and
 * uvAttributes' offset and scale (floats), each through serialisedFloat.
 */
SerialisedValveTexture serialisedValveTexture(const mdl::BrushFace& face);

/**
 * The C23 builder: copies document nodes into one lit::Snapshot on the GUI thread. Every
 * record is plain data; nothing in the snapshot points into the document (D39). Strings
 * are pooled, each distinct one once. The caller decides which nodes go in (the dirty
 * ones), their preview ids, visibility and active flags; the builder only copies.
 */
class LitSnapshotBuilder
{
private:
  lit::Snapshot m_snapshot;
  std::unordered_map<std::string, lit::SnapshotString> m_strings;

public:
  LitSnapshotBuilder(std::uint64_t generation, std::uint64_t sequence);

  /**
   * A brush: each face's BrushFace::polygon (world space, in its order), its three
   * points, its boundary plane, its material as the serialiser writes it (no textures/
   * prefix, __TB_empty for none) and its serialised Valve texture.
   */
  void addBrush(
    lit::PreviewId id,
    lit::PreviewId entity,
    bool visible,
    bool active,
    const mdl::BrushNode& brushNode);

  /**
   * A patch: its control points (x y z u v) row by row, its point row and column counts
   * (TrenchBroom's rows are the engine's width, SC48), its patchDef3 subdivisions if it
   * has them, and its material.
   */
  void addPatch(
    lit::PreviewId id,
    lit::PreviewId entity,
    bool visible,
    bool active,
    const mdl::PatchNode& patchNode);

  // an entity record without a node (worldspawn, a brush entity): its classname and its
  // keys in map order; returns its index in entities
  std::uint32_t addEntity(lit::PreviewId id, const mdl::Entity& entity);

  // a point entity or a light: its entity record, and its node, whose element is that
  // record
  void addPointEntity(
    lit::PreviewId id,
    lit::SnapshotNodeKind kind,
    bool visible,
    bool active,
    const mdl::Entity& entity);

  void addRemoved(lit::PreviewId id);
  void setMapPath(std::string mapPath);

  // no record, no removed id and no map path
  bool empty() const;
  const lit::Snapshot& snapshot() const;
  lit::Snapshot take();

private:
  lit::SnapshotString addString(std::string_view string);
};

} // namespace tb::ui
