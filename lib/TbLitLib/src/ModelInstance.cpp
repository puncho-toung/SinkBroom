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

#include "lit/ModelInstance.h"

#include <utility>

namespace tb::lit
{

const ModelSurface& SharedSurface::surface() const
{
  return record->surfaces[key.surface];
}

std::size_t SharedSurface::vertexCount() const
{
  return surface().verts.size();
}

std::size_t SharedSurface::indexCount() const
{
  return surface().indexes.size();
}

std::shared_ptr<const SharedSurface> makeSharedSurface(
  std::shared_ptr<const ModelRecord> record, const std::size_t surface)
{
  if (!record || surface >= record->surfaces.size())
  {
    return nullptr;
  }
  auto shared = std::make_shared<SharedSurface>();
  shared->key = SharedGeometryKey{record->name, surface, record->version};
  const auto& verts = record->surfaces[surface].verts;
  shared->positions.reserve(verts.size());
  for (const auto& vert : verts)
  {
    shared->positions.push_back(vert.xyz);
  }
  shared->record = std::move(record);
  return shared;
}

} // namespace tb::lit
