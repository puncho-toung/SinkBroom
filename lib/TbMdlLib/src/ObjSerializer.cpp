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

#include "mdl/ObjSerializer.h"

#include "gl/Material.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushGeometry.h"
#include "mdl/BrushNode.h"
#include "mdl/ExportOptions.h"
#include "mdl/IdTech4MaterialName.h"
#include "mdl/PatchNode.h"

#include "kd/contracts.h"
#include "kd/flat_map.h"
#include "kd/overload.h"

#include <fmt/format.h>

#include <iostream>
#include <span>
#include <utility>

namespace tb::mdl
{

std::ostream& operator<<(std::ostream& str, const ObjSerializer::IndexedVertex& vertex)
{
  str << " " << (vertex.vertex + 1u) << "/" << (vertex.uvCoords + 1u) << "/"
      << (vertex.normal + 1u);
  return str;
}

std::ostream& operator<<(std::ostream& str, const ObjSerializer::BrushObject& object)
{
  str << "o entity" << object.entityNo << "_brush" << object.brushNo << "\n";
  for (const auto& face : object.faces)
  {
    str << "usemtl " << face.materialName << "\n";
    str << "f";
    for (const auto& vertex : face.verts)
    {
      str << " " << vertex;
    }
    str << "\n";
  }
  return str;
}

std::ostream& operator<<(std::ostream& str, const ObjSerializer::PatchObject& object)
{
  str << "o entity" << object.entityNo << "_patch" << object.patchNo << "\n";
  str << "usemtl " << object.materialName << "\n";
  for (const auto& quad : object.quads)
  {
    str << "f";
    for (const auto& vertex : quad.verts)
    {
      str << " " << vertex;
    }
    str << "\n";
  }
  return str;
}

std::ostream& operator<<(std::ostream& str, const ObjSerializer::Object& object)
{
  std::visit(
    kdl::overload(
      [&](const ObjSerializer::BrushObject& brushObject) { str << brushObject; },
      [&](const ObjSerializer::PatchObject& patchObject) { str << patchObject; }),
    object);
  return str;
}

namespace
{

/**
 * Writes the given counter-clockwise polygon as a fan of triangles, which keeps its
 * order. RBDOOM-3-BFG's OBJ importer reads exactly three v/vt/vn corners per face and
 * drops the others.
 */
void writeIdTech4Triangles(
  std::ostream& str, const std::span<const ObjSerializer::IndexedVertex> polygon)
{
  for (size_t i = 1; i + 1 < polygon.size(); ++i)
  {
    str << "f" << polygon[0] << polygon[i] << polygon[i + 1] << "\n";
  }
}

void writeIdTech4Object(std::ostream& str, const ObjSerializer::Object& object)
{
  std::visit(
    kdl::overload(
      [&](const ObjSerializer::BrushObject& brushObject) {
        const auto objectName =
          fmt::format("entity{}_brush{}", brushObject.entityNo, brushObject.brushNo);
        str << "o " << objectName << "\n";

        const std::string* previousMaterialName = nullptr;
        for (const auto& face : brushObject.faces)
        {
          // the importer gives every face of a group the material that is current when
          // the group ends, so a new group starts where the material changes
          if (previousMaterialName && *previousMaterialName != face.materialName)
          {
            str << "g " << objectName << "\n";
          }
          previousMaterialName = &face.materialName;

          str << "usemtl " << face.materialName << "\n";
          writeIdTech4Triangles(str, face.verts);
        }
      },
      [&](const ObjSerializer::PatchObject& patchObject) {
        str << "o entity" << patchObject.entityNo << "_patch" << patchObject.patchNo
            << "\n";
        str << "usemtl " << patchObject.materialName << "\n";
        for (const auto& quad : patchObject.quads)
        {
          writeIdTech4Triangles(str, quad.verts);
        }
      }),
    object);
}

} // namespace

ObjSerializer::ObjSerializer(
  std::ostream& objStream,
  std::ostream& mtlStream,
  std::string mtlFilename,
  ObjExportOptions options,
  const ModelConvention modelConvention,
  std::filesystem::path materialRoot)
  : m_objStream{objStream}
  , m_mtlStream{mtlStream}
  , m_mtlFilename{std::move(mtlFilename)}
  , m_options{std::move(options)}
  , m_modelConvention{modelConvention}
  , m_materialRoot{std::move(materialRoot)}
{
  contract_pre(m_objStream.good());
  contract_pre(m_mtlStream.good());
}

void ObjSerializer::doBeginFile(
  const std::vector<const Node*>& /* rootNodes */, kdl::task_manager&)
{
}

static void writeMtlFile(
  std::ostream& str,
  const std::vector<ObjSerializer::Object>& objects,
  const ObjExportOptions& options)
{
  auto usedMaterials = kdl::flat_map<std::string, const gl::Material*>{};

  for (const auto& object : objects)
  {
    std::visit(
      kdl::overload(
        [&](const ObjSerializer::BrushObject& brushObject) {
          for (const auto& face : brushObject.faces)
          {
            usedMaterials[face.materialName] = face.material;
          }
        },
        [&](const ObjSerializer::PatchObject& patchObject) {
          usedMaterials[patchObject.materialName] = patchObject.material;
        }),
      object);
  }

  const auto basePath = options.exportPath.parent_path();
  for (const auto& [materialName, material] : usedMaterials)
  {
    str << "newmtl " << materialName << "\n";
    if (material)
    {
      switch (options.mtlPathMode)
      {
      case ObjMtlPathMode::RelativeToGamePath:
        str << "map_Kd " << material->relativePath().generic_string() << "\n";
        break;
      case ObjMtlPathMode::RelativeToExportPath:
        // materials loaded from image files (pak files) don't have absolute paths
        if (!material->absolutePath().empty())
        {
          const auto mtlPath = material->absolutePath().lexically_relative(basePath);
          str << "map_Kd " << mtlPath.generic_string() << "\n";
        }
        break;
      }
    }
    str << "\n";
  }
}

// RBDOOM-3-BFG reads OBJ files in its own Z-up coordinates and with t = 1 - v
// (renderer/Model_obj.cpp), so under the id Tech 4 convention nothing is turned

static void writeVertices(
  std::ostream& str, const std::vector<vm::vec3d>& vertices, const bool idTech4)
{
  str << "# vertices\n";
  for (const auto& elem : vertices)
  {
    if (idTech4)
    {
      fmt::format_to(
        std::ostreambuf_iterator<char>(str),
        "v {} {} {}\n",
        elem.x(),
        elem.y(),
        elem.z());
    }
    else
    {
      // no idea why I have to switch Y and Z
      fmt::format_to(
        std::ostreambuf_iterator<char>(str),
        "v {} {} {}\n",
        elem.x(),
        elem.z(),
        -elem.y());
    }
  }
}

static void writeUvCoords(
  std::ostream& str, const std::vector<vm::vec2f>& uvCoords, const bool idTech4)
{
  str << "# texture coordinates\n";
  for (const auto& elem : uvCoords)
  {
    if (idTech4)
    {
      // the engine reads back t = 1 - (1 - v) = v exactly
      fmt::format_to(
        std::ostreambuf_iterator<char>(str), "vt {} {}\n", elem.x(), 1.0f - elem.y());
    }
    else
    {
      // multiplying Y by -1 needed to get the UV's to appear correct in Blender and UE4
      // (see: https://github.com/TrenchBroom/TrenchBroom/issues/2851 )
      fmt::format_to(
        std::ostreambuf_iterator<char>(str), "vt {} {}\n", elem.x(), -elem.y());
    }
  }
}

static void writeNormals(
  std::ostream& str, const std::vector<vm::vec3d>& normals, const bool idTech4)
{
  str << "# normals\n";
  for (const auto& elem : normals)
  {
    if (idTech4)
    {
      fmt::format_to(
        std::ostreambuf_iterator<char>(str),
        "vn {} {} {}\n",
        elem.x(),
        elem.y(),
        elem.z());
    }
    else
    {
      // no idea why I have to switch Y and Z
      fmt::format_to(
        std::ostreambuf_iterator<char>(str),
        "vn {} {} {}\n",
        elem.x(),
        elem.z(),
        -elem.y());
    }
  }
}

static void writeObjFile(
  std::ostream& str,
  const std::string mtlFilename,
  const std::vector<vm::vec3d>& vertices,
  const std::vector<vm::vec2f>& uvCoords,
  const std::vector<vm::vec3d>& normals,
  const std::vector<ObjSerializer::Object>& objects,
  const bool idTech4)
{

  str << "mtllib " << mtlFilename << "\n";
  writeVertices(str, vertices, idTech4);
  str << "\n";
  writeUvCoords(str, uvCoords, idTech4);
  str << "\n";
  writeNormals(str, normals, idTech4);
  str << "\n";

  for (const auto& object : objects)
  {
    if (idTech4)
    {
      writeIdTech4Object(str, object);
    }
    else
    {
      str << object;
    }
    str << "\n";
  }
}

void ObjSerializer::doEndFile()
{
  writeMtlFile(m_mtlStream, m_objects, m_options);
  writeObjFile(
    m_objStream,
    m_mtlFilename,
    m_vertices.list(),
    m_uvCoords.list(),
    m_normals.list(),
    m_objects,
    m_modelConvention == ModelConvention::IdTech4);
}

std::string ObjSerializer::exportMaterialName(
  const std::string& materialName, const gl::Material* material) const
{
  if (m_modelConvention != ModelConvention::IdTech4)
  {
    return materialName;
  }

  // The engine takes the rest of a usemtl line as a material decl name. A material's
  // path is its decl, or the image that makes an implicit material of the same name;
  // a material that wasn't found has only its name, which leaves out the root. That name
  // is map text, which std::filesystem::path may fail to convert, so idTech4Path builds
  // its path.
  const auto path = material && !material->relativePath().empty()
                      ? material->relativePath()
                      : m_materialRoot / idTech4Path(materialName);
  return canonicalIdTech4DeclName(path.generic_string());
}

void ObjSerializer::doBeginEntity(const Node&) {}
void ObjSerializer::doEndEntity(const Node&) {}
void ObjSerializer::doEntityProperty(const EntityProperty&) {}

void ObjSerializer::doBrush(const BrushNode& brushNode)
{
  m_currentBrush = BrushObject{entityNo(), brushNo(), {}};
  m_currentBrush->faces.reserve(brushNode.brush().faceCount());

  // Vertex positions inserted from now on should get new indices
  m_vertices.clearIndices();

  for (const auto& face : brushNode.brush().faces())
  {
    doBrushFace(face);
  }

  m_objects.emplace_back(std::move(*m_currentBrush));
  m_currentBrush = std::nullopt;
}

void ObjSerializer::doBrushFace(const BrushFace& face)
{
  const auto& normal = face.boundary().normal;
  const auto normalIndex = m_normals.index(normal);

  auto indexedVertices = std::vector<IndexedVertex>{};
  indexedVertices.reserve(face.vertexCount());

  for (const auto* vertex : face.vertices())
  {
    const auto& position = vertex->position();
    const auto uvCoords = face.uvCoords(position);

    const auto vertexIndex = m_vertices.index(position);
    const auto uvCoordsIndex = m_uvCoords.index(uvCoords);

    indexedVertices.push_back(IndexedVertex{vertexIndex, uvCoordsIndex, normalIndex});
  }

  m_currentBrush->faces.push_back(BrushFaceInfo{
    std::move(indexedVertices),
    exportMaterialName(face.materialName(), face.material()),
    face.material(),
  });
}

void ObjSerializer::doPatch(const PatchNode& patchNode)
{
  const auto& patch = patchNode.patch();
  auto patchObject = PatchObject{
    entityNo(),
    brushNo(),
    {},
    exportMaterialName(patch.materialName(), patch.material()),
    patch.material()};

  const auto& patchGrid = patchNode.grid();
  patchObject.quads.reserve(patchGrid.quadRowCount() * patchGrid.quadColumnCount());

  // Vertex positions inserted from now on should get new indices
  m_vertices.clearIndices();

  const auto makeIndexedVertex = [&](const auto& p) {
    const auto positionIndex = m_vertices.index(p.position);
    const auto uvCoordsIndex = m_uvCoords.index(vm::vec2f{p.uvCoords});
    const auto normalIndex = m_normals.index(p.normal);

    return IndexedVertex{positionIndex, uvCoordsIndex, normalIndex};
  };

  for (size_t row = 0u; row < patchGrid.pointRowCount - 1u; ++row)
  {
    for (size_t col = 0u; col < patchGrid.pointColumnCount - 1u; ++col)
    {
      // counter clockwise order
      patchObject.quads.push_back(PatchQuad{{
        makeIndexedVertex(patchGrid.point(row, col)),
        makeIndexedVertex(patchGrid.point(row + 1u, col)),
        makeIndexedVertex(patchGrid.point(row + 1u, col + 1u)),
        makeIndexedVertex(patchGrid.point(row, col + 1u)),
      }});
    }
  }

  m_objects.emplace_back(std::move(patchObject));
}

} // namespace tb::mdl
