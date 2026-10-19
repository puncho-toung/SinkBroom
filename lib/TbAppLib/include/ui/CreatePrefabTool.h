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

#include "ui/Tool.h"

#include "vm/bbox.h"
#include "vm/plane.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <optional>
#include <string>

namespace tb
{
namespace mdl
{
class Node;
class PickResult;
} // namespace mdl

namespace ui
{
class MapDocument;

/**
 * Places a prefab from the map's prefab library by drag and drop.
 *
 * createPrefab opens a long running transaction and adds the prefab: an entity snapshot
 * as a plain entity, an assembly as a linked duplicate of a group already placed from
 * the same file, or else as a new group that records the file (C3). The prefab is then
 * moved with the cursor until commit or cancel closes the transaction.
 */
class CreatePrefabTool : public Tool
{
private:
  MapDocument& m_document;
  // the added entity or group; not null while a placement is in progress
  mdl::Node* m_prefabNode = nullptr;
  vm::bbox3d m_referenceBounds;
  // the plane of the first face hit in 3D that does not belong to the prefab
  std::optional<vm::plane3d> m_placementPlane;

public:
  explicit CreatePrefabTool(MapDocument& document);

  /**
   * Resolves the given drop payload (C4) through the map's prefab library and adds the
   * prefab inside a new long running transaction, selected. Returns false and opens no
   * transaction if the payload names no readable prefab.
   */
  bool createPrefab(const std::string& payload);

  bool placing() const;

  void updateCreatedPrefab(const vm::vec3d& delta);
  void updatePrefabPosition2D(const vm::ray3d& pickRay);
  void updatePrefabPosition3D(
    const vm::ray3d& pickRay, const mdl::PickResult& pickResult);

  void commit();
  void cancel();

private:
  bool isPrefabNode(const mdl::Node& node) const;
};

} // namespace ui
} // namespace tb
