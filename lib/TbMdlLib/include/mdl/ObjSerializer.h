/*
 Copyright (C) 2010 Kristian Duske

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

#include "mdl/ExportOptions.h"
#include "mdl/GameConfig.h"
#include "mdl/NodeSerializer.h"

#include "vm/vec.h"

#include <array>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tb
{
namespace gl
{
class Material;
}

namespace mdl
{
class BrushNode;
class BrushFace;
class EntityProperty;
class Node;

class ObjSerializer : public NodeSerializer
{
public:
  template <typename V>
  class IndexMap
  {
  private:
    std::map<V, size_t> m_map;
    std::vector<V> m_list;

  public:
    const std::vector<V>& list() const { return m_list; }

    size_t index(const V& v)
    {
      const auto it = m_map.emplace(v, m_list.size()).first;
      const size_t index = it->second;
      if (index == m_list.size())
      {
        m_list.push_back(v);
      }
      return index;
    }

    /**
     * Values inserted after this is called will not reuse indices from before this
     * is called.
     */
    void clearIndices() { m_map.clear(); }
  };

  struct IndexedVertex
  {
    size_t vertex;
    size_t uvCoords;
    size_t normal;
  };

  struct BrushFaceInfo
  {
    std::vector<IndexedVertex> verts;
    std::string materialName;
    const gl::Material* material;
  };

  struct BrushObject
  {
    size_t entityNo;
    size_t brushNo;
    std::vector<BrushFaceInfo> faces;
  };

  struct PatchQuad
  {
    std::array<IndexedVertex, 4u> verts;
  };

  struct PatchObject
  {
    size_t entityNo;
    size_t patchNo;
    std::vector<PatchQuad> quads;
    std::string materialName;
    const gl::Material* material;
  };

  using Object = std::variant<BrushObject, PatchObject>;

  friend std::ostream& operator<<(std::ostream& str, const IndexedVertex& vertex);
  friend std::ostream& operator<<(std::ostream& str, const BrushFaceInfo& face);
  friend std::ostream& operator<<(std::ostream& str, const BrushObject& object);
  friend std::ostream& operator<<(std::ostream& str, const PatchQuad& quad);
  friend std::ostream& operator<<(std::ostream& str, const PatchObject& object);
  friend std::ostream& operator<<(std::ostream& str, const Object& object);

private:
  std::ostream& m_objStream;
  std::ostream& m_mtlStream;
  std::string m_mtlFilename;
  ObjExportOptions m_options;
  ModelConvention m_modelConvention;
  std::filesystem::path m_materialRoot;

  IndexMap<vm::vec3d> m_vertices;
  IndexMap<vm::vec2f> m_uvCoords;
  IndexMap<vm::vec3d> m_normals;

  std::optional<BrushObject> m_currentBrush;
  std::vector<Object> m_objects;

public:
  /**
   * Under ModelConvention::IdTech4, the file is written for RBDOOM-3-BFG's OBJ importer:
   * positions and normals are not turned, texture coordinates are written as (u, 1 - v),
   * faces are split into triangles, a new group starts where the material changes, and
   * the material names are the full decl names, which the given material root completes
   * for materials that have no path.
   */
  ObjSerializer(
    std::ostream& objStream,
    std::ostream& mtlStream,
    std::string mtlFilename,
    ObjExportOptions options,
    ModelConvention modelConvention = ModelConvention::Default,
    std::filesystem::path materialRoot = {});

private:
  std::string exportMaterialName(
    const std::string& materialName, const gl::Material* material) const;

  void doBeginFile(
    const std::vector<const Node*>& rootNodes, kdl::task_manager& taskManager) override;
  void doEndFile() override;

  void doBeginEntity(const Node& node) override;
  void doEndEntity(const Node& node) override;
  void doEntityProperty(const EntityProperty& property) override;

  void doBrush(const BrushNode& brushNode) override;
  void doBrushFace(const BrushFace& face) override;

  void doPatch(const PatchNode& patchNode) override;
};

} // namespace mdl
} // namespace tb
