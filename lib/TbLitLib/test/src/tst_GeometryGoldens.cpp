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

// D60: the LP1b goldens on the fixture map, against what the engine wrote for it in the
// sandbox (TOOLS/lit-preview/capture_fixtures.py): (a) the preview's st and normal rules
// at every .proc vertex of the axial brushes, exactly; (b) the copied tangent code on the
// .proc surfaces as ParseModel loads them, exactly against the .bproc; (c) the area of
// every face piece per (area, material) against the _areaN models; (d) the preview's own
// chunks against the .proc, with a tolerance (K9).

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "lit/AreaTree.h"
#include "lit/BprocModel.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineGeometry.h"
#include "lit/EntityState.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

struct Fixture
{
  Snapshot snapshot;
  std::map<std::string, std::pair<int, int>> sizes;
  std::shared_ptr<const MaterialTable> materials;
  std::shared_ptr<const ProcTree> proc;
  BprocFile bproc;
};

std::vector<std::byte> readBytes(const std::filesystem::path& path)
{
  auto in = std::ifstream{path, std::ios::binary};
  const auto chars = std::vector<char>{
    std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  auto bytes = std::vector<std::byte>(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

const Fixture& fixture()
{
  static const auto loaded = [] {
    const auto root = litFixtureFolder();
    const auto goldens = root / "goldens" / "lp1b_geometry";
    auto f = Fixture{};
    f.snapshot = snapshotOfMapText(readTextFile(root / "maps" / "lp1b_geometry.map"));
    f.sizes = listImagesSizes(readTextFile(goldens / "listImages.txt"));
    f.materials = fixtureMaterialTable(f.sizes);
    f.proc = std::make_shared<const ProcTree>(
      readProcFile(readTextFile(goldens / "lp1b_geometry.proc"), true)
      | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value());
    const auto bytes = readBytes(goldens / "lp1b_geometry.bproc");
    f.bproc = readBprocFile(bytes) | kdl::if_error([](const auto& e) { FAIL(e.msg); })
              | kdl::value();
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    return f;
  }();
  return loaded;
}

const MaterialEntry* entryOf(const Fixture& f, const std::string& name)
{
  const auto slot = f.materials->find(name);
  return slot ? slot->entry.get() : nullptr;
}

struct FaceRef
{
  const SnapshotFace* face;
  PreviewId entity;
};

std::vector<FaceRef> facesOf(const Snapshot& snapshot)
{
  auto faces = std::vector<FaceRef>{};
  for (const auto& node : snapshot.nodes)
  {
    if (node.kind == SnapshotNodeKind::Brush)
    {
      for (auto f = node.elements.first; f < node.elements.first + node.elements.count;
           ++f)
      {
        faces.push_back(FaceRef{&snapshot.faces[f], node.entity});
      }
    }
  }
  return faces;
}

std::vector<EngineVec3> engineWinding(const Snapshot& snapshot, const SnapshotFace& face)
{
  auto winding = std::vector<EngineVec3>{};
  for (auto v = face.polygon.first + face.polygon.count; v > face.polygon.first; --v)
  {
    const auto& p = snapshot.vertices[v - 1];
    winding.push_back({float(p.x()), float(p.y()), float(p.z())});
  }
  return winding;
}

EnginePlane sidePlane(const SnapshotFace& face)
{
  const auto f = [](const vm::vec3d& p) {
    return EngineVec3{float(p.x()), float(p.y()), float(p.z())};
  };
  return *engineSidePlane(f(face.points[0]), f(face.points[1]), f(face.points[2]));
}

double polygonArea(const std::vector<EngineVec3>& w)
{
  auto area = 0.0;
  for (auto i = std::size_t{2}; i < w.size(); ++i)
  {
    area += double(engineTriangleArea(w[0], w[i - 1], w[i]));
  }
  return area;
}

bool insidePolygon(const Snapshot& snapshot, const SnapshotFace& face, const vm::vec3d& p)
{
  // counter-clockwise seen from outside: inside is left of every edge, the edge included
  for (auto i = 0u; i < face.polygon.count; ++i)
  {
    const auto& a = snapshot.vertices[face.polygon.first + i];
    const auto& b = snapshot.vertices[face.polygon.first + (i + 1) % face.polygon.count];
    if (vm::dot(vm::cross(b - a, p - a), face.plane.normal) < -1e-6)
    {
      return false;
    }
  }
  return true;
}

// the .proc surfaces of the _areaN models, per (area, material): summed triangle area
std::map<std::pair<int, std::string>, double> procAreas(const ProcTree& proc)
{
  auto areas = std::map<std::pair<int, std::string>, double>{};
  for (const auto& model : proc.models)
  {
    if (model.area < 0)
    {
      continue;
    }
    for (const auto& surface : model.surfaces)
    {
      const auto stride = std::size_t(surface.vertexFormat == 1 ? 12 : 8);
      const auto at = [&](const int i) {
        const auto* v = surface.verts.data() + std::size_t(i) * stride;
        return EngineVec3{v[0], v[1], v[2]};
      };
      for (auto i = std::size_t{0}; i + 2 < surface.indexes.size(); i += 3)
      {
        areas[{model.area, surface.material}] += double(engineTriangleArea(
          at(surface.indexes[i]),
          at(surface.indexes[i + 1]),
          at(surface.indexes[i + 2])));
      }
    }
  }
  return areas;
}

// four bytes as numbers, for Catch's messages
std::array<int, 4> bytes(const std::array<std::uint8_t, 4>& b)
{
  return {b[0], b[1], b[2], b[3]};
}

bool closeArea(const double a, const double b, const double relative)
{
  return std::abs(a - b) <= relative * std::max(1.0, std::abs(b));
}

// every cell of the fixture's snapshot as one job, with its .proc and materials
ChunkJobInput fixtureInput(const Fixture& f)
{
  auto input = ChunkJobInput{};
  for (const auto& entity : sceneEntitiesOf(f.snapshot))
  {
    input.entities[entity->id] = entity;
    if (entity->classname == "worldspawn")
    {
      input.worldspawn = entity;
    }
  }
  auto cells = std::set<CellRef>{};
  for (const auto& node : sceneNodesOf(f.snapshot))
  {
    input.nodes.push_back(node);
    if (node->kind == SnapshotNodeKind::Brush || node->kind == SnapshotNodeKind::Patch)
    {
      const auto entity = input.entities.at(node->entity);
      cells.insert(CellRef{
        entity->classname == "func_group" ? input.worldspawn->id : node->entity,
        cellOfNode(*node)});
    }
  }
  input.cells.assign(cells.begin(), cells.end());
  input.proc = f.proc;
  input.materials = f.materials;
  return input;
}

// the entity whose name key is name
const SnapshotEntity* entityNamed(const Snapshot& snapshot, const std::string& name)
{
  for (const auto& entity : snapshot.entities)
  {
    for (auto k = entity.keys.first; k < entity.keys.first + entity.keys.count; ++k)
    {
      if (
        snapshot.string(snapshot.keyValues[k].key) == "name"
        && snapshot.string(snapshot.keyValues[k].value) == name)
      {
        return &entity;
      }
    }
  }
  return nullptr;
}

// the preview's st rule (D59) for a side vertex: relative to the entity's origin key,
// through SetTexCoord and the chunker's read-back of what dmap writes
EngineVec2 previewSt(
  const SnapshotFace& face, const EngineVec3& relative, const std::pair<int, int>& size)
{
  const auto texture = EngineValveTexture{
    {face.uAxis.x(), face.uAxis.y(), face.uAxis.z()},
    face.offset.x(),
    {face.vAxis.x(), face.vAxis.y(), face.vAxis.z()},
    face.offset.y(),
    face.scale.x(),
    face.scale.y()};
  const auto st =
    engineValveTexCoord(relative, texture, float(size.first), float(size.second));
  auto vert = engineClearedVert();
  engineSetTexCoord(vert, st[0], st[1]);
  const auto stored = engineTexCoord(vert);
  return {procReadBack(stored[0]), procReadBack(stored[1])};
}

} // namespace

TEST_CASE("Geometry goldens: the fixture's engine output reads back")
{
  const auto& f = fixture();
  CHECK(f.proc->version == 4);
  CHECK(f.proc->numAreas == 2);
  CHECK(f.proc->portals.size() == 1u);
  CHECK(f.proc->nodes.size() == 39u);
  CHECK(f.proc->models.size() == 4u);
  // the .bproc stores idLexer's results: its tree is the .proc's, exactly (D64)
  CHECK(f.bproc.areas.nodes.size() == f.proc->nodes.size());
  for (auto i = std::size_t{0}; i < f.proc->nodes.size(); ++i)
  {
    CHECK(f.bproc.areas.nodes[i].plane == f.proc->nodes[i].plane);
    CHECK(f.bproc.areas.nodes[i].children == f.proc->nodes[i].children);
  }
  CHECK(f.bproc.models.size() == f.proc->models.size());
  CHECK(f.sizes.at("textures/lp1b/floor") == std::pair{64, 32});
  CHECK(f.sizes.at("textures/lp1b/patch") == std::pair{64, 16});

  // R1.4: a CRLF checkout of the listing gives the same sizes
  auto crlf = std::string{};
  for (const auto c :
       readTextFile(litFixtureFolder() / "goldens" / "lp1b_geometry" / "listImages.txt"))
  {
    if (c == '\n')
    {
      crlf += '\r';
    }
    crlf += c;
  }
  CHECK(listImagesSizes(crlf) == f.sizes);

  // PointInArea: the two rooms, a wall, outside
  CHECK(pointInArea(*f.proc, {128, 0, 64}) == 0);
  CHECK(pointInArea(*f.proc, {-128, 0, 64}) == 1);
  CHECK(pointInArea(*f.proc, {264, 0, 64}) == -1);
  CHECK(pointInArea(*f.proc, {416, 16, 16}) == -1);
}

TEST_CASE(
  "Geometry goldens: (a) st and normals at the .proc vertices of the axial brushes")
{
  const auto& f = fixture();
  const auto faces = facesOf(f.snapshot);
  auto checked = 0;
  for (const auto& model : f.proc->models)
  {
    if (model.area < 0)
    {
      continue;
    }
    for (const auto& surface : model.surfaces)
    {
      if (surface.material == "textures/lp1b/patch")
      {
        continue;
      }
      const auto& size = f.sizes.at(surface.material);
      for (auto j = 0; j < surface.numVerts; ++j)
      {
        const auto* v = surface.verts.data() + std::size_t(j) * 8;
        const auto p = vm::vec3d{v[0], v[1], v[2]};
        const auto normal = vm::vec3d{v[5], v[6], v[7]};
        // the face of that material whose plane holds the vertex, facing the same way
        const auto it = std::find_if(faces.begin(), faces.end(), [&](const FaceRef& ref) {
          const auto& face = *ref.face;
          return engineSideMaterial(std::string{f.snapshot.string(face.material)})
                   == surface.material
                 && std::abs(vm::dot(face.plane.normal, p) - face.plane.distance) < 0.01
                 && vm::dot(face.plane.normal, normal) > 0.9
                 && insidePolygon(f.snapshot, *ref.face, p);
        });
        REQUIRE(it != faces.end());
        const auto& face = *it->face;
        CAPTURE(surface.material, j, v[0], v[1], v[2]);

        // the preview's st rule (D59), through SetTexCoord and the chunker's own
        // read-back of what dmap writes (R3.9: procReadBack, WriteFloat's snap included);
        // P.18: modulo integers
        const auto st = previewSt(face, {v[0], v[1], v[2]}, size);
        const auto ds = st[0] - v[3];
        const auto dt = st[1] - v[4];
        CHECK(ds == std::round(ds));
        CHECK(dt == std::round(dt));

        // the side plane's normal through SetNormal, again through SetNormal(GetNormal())
        // as ShareMapTriVerts stores it (dmap/output.cpp:250), and the mirror, exactly
        auto vert = engineClearedVert();
        const auto plane = sidePlane(face);
        engineSetNormal(vert, {plane.a, plane.b, plane.c});
        engineSetNormal(vert, engineNormal(vert));
        const auto n = engineNormal(vert);
        CHECK(procFloat(n[0]) == v[5]);
        CHECK(procFloat(n[1]) == v[6]);
        CHECK(procFloat(n[2]) == v[7]);
        ++checked;
      }
    }
  }
  CHECK(checked > 50);
}

TEST_CASE("Geometry goldens: (a) st at the .proc vertices of the brush entities (R3.6)")
{
  // A19: dmap's st come from the position relative to the origin key (TriListForSide
  // runs before WriteUTriangles subtracts originOffset), its vertices are relative to
  // origin + originOffset; so a vertex v of an entity model is at v + originOffset
  // relative to the origin
  const auto& f = fixture();
  const auto output = buildChunks(fixtureInput(f));
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  auto checked = 0;
  auto shifted = 0;
  for (const auto& model : f.proc->models)
  {
    if (model.area >= 0)
    {
      continue;
    }
    const auto* entity = entityNamed(f.snapshot, model.name);
    REQUIRE(entity);
    const auto state =
      std::find_if(output.entities.begin(), output.entities.end(), [&](const auto& s) {
        return s.id == entity->id;
      });
    REQUIRE(state != output.entities.end());
    const auto origin = state->parms.origin;
    const auto offset = state->originOffset;
    for (const auto& surface : model.surfaces)
    {
      const auto& size = f.sizes.at(surface.material);
      for (auto j = 0; j < surface.numVerts; ++j)
      {
        const auto* v = surface.verts.data() + std::size_t(j) * 8;
        const auto relative =
          EngineVec3{v[0] + offset[0], v[1] + offset[1], v[2] + offset[2]};
        const auto world = vm::vec3d{
          double(relative[0] + origin[0]),
          double(relative[1] + origin[1]),
          double(relative[2] + origin[2])};
        const auto normal = vm::vec3d{v[5], v[6], v[7]};
        const auto faces = facesOf(f.snapshot);
        const auto it = std::find_if(faces.begin(), faces.end(), [&](const FaceRef& ref) {
          const auto& face = *ref.face;
          return ref.entity == entity->id
                 && engineSideMaterial(std::string{f.snapshot.string(face.material)})
                      == surface.material
                 && std::abs(vm::dot(face.plane.normal, world) - face.plane.distance)
                      < 0.01
                 && vm::dot(face.plane.normal, normal) > 0.9
                 && insidePolygon(f.snapshot, face, world);
        });
        REQUIRE(it != faces.end());
        CAPTURE(model.name, surface.material, j, v[0], v[1], v[2]);
        const auto st = previewSt(*it->face, relative, size);
        const auto ds = st[0] - v[3];
        const auto dt = st[1] - v[4];
        CHECK(ds == std::round(ds));
        CHECK(dt == std::round(dt));
        // whether the rule's choice (origin, not origin + originOffset) shows here: the
        // st of the pivot-relative position differ by more than an integer
        const auto pivot = previewSt(*it->face, {v[0], v[1], v[2]}, size);
        const auto ps = pivot[0] - v[3];
        const auto pt = pivot[1] - v[4];
        shifted += ps != std::round(ps) || pt != std::round(pt) ? 1 : 0;
        ++checked;
      }
    }
  }
  CHECK(checked > 20);
  // the fixture's origin brush moves st by more than an integer, so the test tells
  // dmap's rule (origin) from the pivot's (origin + originOffset)
  CHECK(shifted > 0);
}

TEST_CASE("Geometry goldens: (a) the chunks' own st and normals at the .proc's vertices")
{
  // V.9: what the chunker made, not the rule recomposed: every brush vertex of a chunk
  // that is also a vertex of the .proc model of its entity (or of the world's areas),
  // with the same material and normal, has the .proc's st modulo integers (P.18)
  const auto& f = fixture();
  const auto input = fixtureInput(f);
  const auto output = buildChunks(input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  struct ProcVert
  {
    EngineVec3 xyz;
    EngineVec2 st;
    std::array<std::uint8_t, 4> normal;
  };
  // the world's by material, an entity model's by (name, material)
  auto procVerts = std::map<std::pair<std::string, std::string>, std::vector<ProcVert>>{};
  for (const auto& model : f.proc->models)
  {
    for (const auto& surface : model.surfaces)
    {
      if (surface.material == "textures/lp1b/patch")
      {
        continue;
      }
      const auto owner = model.area >= 0 ? std::string{} : model.name;
      for (auto j = 0; j < surface.numVerts; ++j)
      {
        const auto* v = surface.verts.data() + std::size_t(j) * 8;
        auto vert = engineClearedVert();
        engineSetNormal(vert, {v[5], v[6], v[7]});
        procVerts[{owner, surface.material}].push_back(
          ProcVert{{v[0], v[1], v[2]}, {v[3], v[4]}, vert.normal});
      }
    }
  }

  auto worldChecked = 0;
  auto entityChecked = 0;
  for (const auto& chunk : output.chunks)
  {
    if (chunk.key.material == "textures/lp1b/patch")
    {
      continue;
    }
    const auto world = chunk.key.entity == input.worldspawn->id;
    auto owner = std::string{};
    if (!world)
    {
      owner = std::string{spawnString(input.entities.at(chunk.key.entity)->keys, "name")};
    }
    const auto found = procVerts.find({owner, chunk.key.material});
    if (found == procVerts.end())
    {
      continue;
    }
    for (const auto& vert : chunk.verts)
    {
      // two coplanar faces of one material may meet there with different alignments:
      // the chunk's st agree with one of the .proc's vertices there
      const auto st = engineTexCoord(vert);
      auto matches = 0;
      auto congruent = false;
      for (const auto& p : found->second)
      {
        if (p.xyz == vert.xyz && p.normal == vert.normal)
        {
          ++matches;
          const auto ds = st[0] - p.st[0];
          const auto dt = st[1] - p.st[1];
          congruent = congruent || (ds == std::round(ds) && dt == std::round(dt));
        }
      }
      if (matches == 0)
      {
        continue;
      }
      CAPTURE(
        chunk.key.material, owner, vert.xyz[0], vert.xyz[1], vert.xyz[2], st[0], st[1]);
      CHECK(congruent);
      (world ? worldChecked : entityChecked) += 1;
    }
  }
  CHECK(worldChecked > 50);
  CHECK(entityChecked > 10);
}

TEST_CASE("Geometry goldens: (a) the patches' corner st (R3.5)")
{
  // dmap sends every patch triangle through TriVertsFromOriginal (fullCarve); the
  // preview's chunks against the .proc at each patch's four corner control points, which
  // both have as vertices, modulo integers (P.18)
  const auto& f = fixture();
  const auto output = buildChunks(fixtureInput(f));
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  auto checked = 0;
  for (const auto& sp : f.snapshot.patches)
  {
    const auto corner = [&](const std::uint32_t row, const std::uint32_t column) {
      return f.snapshot.controlPoints[sp.controlPoints.first + row * sp.columns + column];
    };
    for (const auto& p :
         {corner(0, 0),
          corner(0, sp.columns - 1),
          corner(sp.rows - 1, 0),
          corner(sp.rows - 1, sp.columns - 1)})
    {
      const auto xyz = EngineVec3{float(p[0]), float(p[1]), float(p[2])};
      CAPTURE(xyz[0], xyz[1], xyz[2]);
      // the .proc's vertex there
      auto procSt = std::optional<EngineVec2>{};
      for (const auto& model : f.proc->models)
      {
        for (const auto& surface : model.surfaces)
        {
          if (surface.material != "textures/lp1b/patch")
          {
            continue;
          }
          for (auto j = 0; j < surface.numVerts; ++j)
          {
            const auto* v = surface.verts.data() + std::size_t(j) * 8;
            if (EngineVec3{v[0], v[1], v[2]} == xyz)
            {
              procSt = EngineVec2{v[3], v[4]};
            }
          }
        }
      }
      // the chunks' vertex there
      auto chunkSt = std::optional<EngineVec2>{};
      for (const auto& chunk : output.chunks)
      {
        if (chunk.key.material != "textures/lp1b/patch")
        {
          continue;
        }
        for (const auto& vert : chunk.verts)
        {
          if (vert.xyz == xyz)
          {
            chunkSt = engineTexCoord(vert);
          }
        }
      }
      REQUIRE(procSt);
      REQUIRE(chunkSt);
      const auto ds = (*chunkSt)[0] - (*procSt)[0];
      const auto dt = (*chunkSt)[1] - (*procSt)[1];
      CHECK(ds == std::round(ds));
      CHECK(dt == std::round(dt));
      ++checked;
    }
  }
  CHECK(checked == 8);
}

TEST_CASE("Geometry goldens: (b) the copied tangent code against the .bproc")
{
  const auto& f = fixture();
  REQUIRE(f.bproc.models.size() == f.proc->models.size());
  auto surfacesChecked = 0;
  auto vertsChecked = 0;
  for (auto m = std::size_t{0}; m < f.proc->models.size(); ++m)
  {
    const auto& model = f.proc->models[m];
    const auto& binary = f.bproc.models[m];
    CHECK(binary.name == model.name);

    // ParseModel, then FinishSurfaces( false ): back sides appended, then each surface
    // through R_CleanupTriangles
    struct Loaded
    {
      std::string material;
      std::vector<EngineDrawVert> verts;
      std::vector<std::uint16_t> indexes;
    };
    auto loaded = std::vector<Loaded>{};
    for (const auto& surface : model.surfaces)
    {
      auto indexes = std::vector<std::uint16_t>{};
      for (const auto index : surface.indexes)
      {
        indexes.push_back(std::uint16_t(index));
      }
      loaded.push_back(
        Loaded{surface.material, loadProcSurface(surface), std::move(indexes)});
    }
    const auto originals = loaded.size();
    for (auto s = std::size_t{0}; s < originals; ++s)
    {
      const auto* entry = entryOf(f, loaded[s].material);
      REQUIRE(entry);
      if (entry->createBackSides)
      {
        auto reversed = engineReverseTriangles(loaded[s].verts, loaded[s].indexes);
        loaded.push_back(Loaded{
          loaded[s].material, std::move(reversed.verts), std::move(reversed.indexes)});
      }
    }
    REQUIRE(binary.surfaces.size() == loaded.size());
    for (auto s = std::size_t{0}; s < loaded.size(); ++s)
    {
      CAPTURE(model.name, s, loaded[s].material);
      const auto* entry = entryOf(f, loaded[s].material);
      const auto cleaned =
        engineCleanupTriangles(loaded[s].verts, loaded[s].indexes, tangentPath(entry));
      REQUIRE(cleaned);
      const auto& expected = binary.surfaces[s];
      CHECK(expected.material == loaded[s].material);
      CHECK(cleaned->indexes == expected.indexes);
      REQUIRE(cleaned->verts.size() == expected.verts.size());
      for (auto k = std::size_t{0}; k < expected.verts.size(); ++k)
      {
        CAPTURE(k);
        CHECK(cleaned->verts[k].xyz == expected.verts[k].xyz);
        CHECK(cleaned->verts[k].st == expected.verts[k].st);
        CHECK(bytes(cleaned->verts[k].normal) == bytes(expected.verts[k].normal));
        CHECK(bytes(cleaned->verts[k].tangent) == bytes(expected.verts[k].tangent));
        CHECK(bytes(cleaned->verts[k].color) == bytes(expected.verts[k].color));
        CHECK(bytes(cleaned->verts[k].color2) == bytes(expected.verts[k].color2));
        ++vertsChecked;
      }
      CHECK(cleaned->mirroredVerts == expected.mirroredVerts);
      ++surfacesChecked;
    }
  }
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  CHECK(surfacesChecked >= 10);
  CHECK(vertsChecked > 100);
}

TEST_CASE("Geometry goldens: (c) the area of each face piece per (area, material)")
{
  const auto& f = fixture();
  const auto worldspawn = f.snapshot.entities.front().id;
  auto previewAreas = std::map<std::pair<int, std::string>, double>{};
  auto solidPieces = 0;

  for (const auto& ref : facesOf(f.snapshot))
  {
    if (ref.entity != worldspawn)
    {
      continue;
    }
    const auto& face = *ref.face;
    const auto name = engineSideMaterial(std::string{f.snapshot.string(face.material)});
    if (name == "textures/editor/visportal")
    {
      continue;
    }
    for (const auto& piece : clipSideIntoAreas(
           f.proc.get(), engineWinding(f.snapshot, face), sidePlane(face)))
    {
      if (piece.area == SolidArea)
      {
        ++solidPieces;
        continue;
      }
      previewAreas[{piece.area, name}] += polygonArea(piece.points);
    }
  }
  // the world's patches: subdivided, then each triangle split into the areas
  for (const auto& node : f.snapshot.nodes)
  {
    if (node.kind != SnapshotNodeKind::Patch)
    {
      continue;
    }
    const auto& sp = f.snapshot.patches[node.elements.first];
    auto patch = EnginePatch{};
    patch.width = int(sp.rows);
    patch.height = int(sp.columns);
    patch.explicitSubdivisions = sp.explicitSubdivisions;
    patch.horizontalSubdivisions = int(sp.horizontalSubdivisions);
    patch.verticalSubdivisions = int(sp.verticalSubdivisions);
    patch.controlPoints.resize(sp.controlPoints.count);
    for (auto r = 0u; r < sp.rows; ++r)
    {
      for (auto c = 0u; c < sp.columns; ++c)
      {
        const auto& p =
          f.snapshot.controlPoints[sp.controlPoints.first + r * sp.columns + c];
        auto vert = engineClearedVert();
        vert.xyz = {float(p[0]), float(p[1]), float(p[2])};
        engineSetTexCoord(vert, float(p[3]), float(p[4]));
        patch.controlPoints[c * sp.rows + r] = vert;
      }
    }
    const auto surface = engineSubdividePatch(patch);
    REQUIRE(surface);
    for (auto i = std::size_t{0}; i + 2 < surface->indexes.size(); i += 3)
    {
      const auto tri = std::array<EngineVec3, 3>{
        surface->verts[std::size_t(surface->indexes[i])].xyz,
        surface->verts[std::size_t(surface->indexes[i + 1])].xyz,
        surface->verts[std::size_t(surface->indexes[i + 2])].xyz};
      for (const auto& piece : clipTriangleIntoAreas(f.proc.get(), tri))
      {
        if (piece.area >= 0)
        {
          previewAreas[{piece.area, "textures/lp1b/patch"}] += polygonArea(piece.points);
        }
      }
    }
  }

  const auto engineAreas = procAreas(*f.proc);
  // stated tolerances: 0.01 % for brush faces (dmap re-triangulates and adds T-junction
  // vertices, the surface stays the same), 0.1 % for patches, whose curved surface dmap
  // snaps to its vertex grid (K9)
  CHECK(previewAreas.size() == engineAreas.size());
  for (const auto& [key, area] : engineAreas)
  {
    const auto tolerance = key.second == "textures/lp1b/patch" ? 1e-3 : 1e-4;
    CAPTURE(key.first, key.second, area, previewAreas[key]);
    CHECK(closeArea(previewAreas[key], area, tolerance));
  }
  // K14: the brush outside the hull and the faces inside walls reach solid
  CHECK(solidPieces > 6);
}

TEST_CASE("Geometry goldens: wall faces on node planes, both orientations")
{
  const auto& f = fixture();
  // the partition wall's faces at x = 8 (normal +x) and x = -8 (normal -x)
  auto plusX = 0;
  auto minusX = 0;
  auto outside = 0;
  for (const auto& ref : facesOf(f.snapshot))
  {
    const auto& face = *ref.face;
    const auto n = face.plane.normal;
    const auto pieces =
      clipSideIntoAreas(f.proc.get(), engineWinding(f.snapshot, face), sidePlane(face));
    // node 9 is ( 1 0 0 -8 ): the face lies on it and agrees, so child 0, the +x room
    if (n == vm::vec3d{1, 0, 0} && face.plane.distance == 8.0)
    {
      for (const auto& piece : pieces)
      {
        CHECK((piece.area == 0 || piece.area == SolidArea));
        plusX += piece.area == 0 ? 1 : 0;
      }
    }
    // node 26 is ( 1 0 0 8 ): the face lies on its flip, so child 1, the -x room
    if (n == vm::vec3d{-1, 0, 0} && face.plane.distance == 8.0)
    {
      for (const auto& piece : pieces)
      {
        CHECK((piece.area == 1 || piece.area == SolidArea));
        minusX += piece.area == 1 ? 1 : 0;
      }
    }
    // the brush outside the hull, x from 400 to 432
    const auto& first = f.snapshot.vertices[face.polygon.first];
    if (first.x() >= 400.0)
    {
      for (const auto& piece : pieces)
      {
        CHECK(piece.area == SolidArea);
        ++outside;
      }
    }
  }
  CHECK(plusX > 0);
  CHECK(minusX > 0);
  CHECK(outside >= 6);

  // with the editor's winding order the same face would go the other way (§3.4)
  const auto quad =
    std::vector<EngineVec3>{{8, -128, 0}, {8, -128, 128}, {8, -32, 128}, {8, -32, 0}};
  const auto reversed = std::vector<EngineVec3>{quad.rbegin(), quad.rend()};
  const auto plane = EnginePlane{1, 0, 0, -8};
  const auto split = engineSplitWinding(quad, plane, 0.1f);
  const auto other = engineSplitWinding(reversed, plane, 0.1f);
  REQUIRE(split);
  REQUIRE(other);
  CHECK(split->side != other->side);
  CHECK(split->side != EngineSplitSide::Cross);
}

TEST_CASE("Geometry goldens: (d) the preview's chunks against the .proc")
{
  const auto& f = fixture();
  const auto input = fixtureInput(f);
  const auto output = buildChunks(input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  CHECK(output.warnings.empty());
  CHECK(output.pendingMaterials.empty());
  CHECK(output.solidPieces > 6);

  // the world's chunks per (area, material), back sides counted once
  auto chunkAreas = std::map<std::pair<int, std::string>, double>{};
  auto entityBounds = std::map<PreviewId, std::array<EngineVec3, 2>>{};
  for (const auto& chunk : output.chunks)
  {
    CHECK(chunk.verts.size() <= 65535u);
    CHECK(chunk.ranges.size() <= MaxChunkRanges);
    CHECK_FALSE(chunk.provisional);
    for (const auto& vert : chunk.verts)
    {
      CHECK(vert.color == std::array<std::uint8_t, 4>{255, 255, 255, 255});
      CHECK(vert.color2 == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    }
    const auto* entry = entryOf(f, chunk.key.material);
    const auto sides = entry && entry->createBackSides ? 2.0 : 1.0;
    if (chunk.key.entity == input.worldspawn->id)
    {
      auto area = 0.0;
      for (auto i = std::size_t{0}; i + 2 < chunk.indexes.size(); i += 3)
      {
        area += double(engineTriangleArea(
          chunk.verts[chunk.indexes[i]].xyz,
          chunk.verts[chunk.indexes[i + 1]].xyz,
          chunk.verts[chunk.indexes[i + 2]].xyz));
      }
      if (chunk.key.area >= 0)
      {
        chunkAreas[{chunk.key.area, chunk.key.material}] += area / sides;
      }
    }
    else if (const auto it = entityBounds.find(chunk.key.entity);
             it == entityBounds.end())
    {
      entityBounds[chunk.key.entity] = chunk.bounds;
    }
    else
    {
      // R3.6: the union of the entity's chunks
      for (auto k = std::size_t{0}; k < 3; ++k)
      {
        it->second[0][k] = std::min(it->second[0][k], chunk.bounds[0][k]);
        it->second[1][k] = std::max(it->second[1][k], chunk.bounds[1][k]);
      }
    }
    if (chunk.key.material == "textures/lp1b/grate")
    {
      CHECK(chunk.castClass == CastClass::Perforated);
    }
    if (chunk.key.material == "textures/lp1b/glass")
    {
      CHECK(chunk.castClass == CastClass::None);
    }
  }
  // K9: dmap snaps, adds T-junction vertices and re-triangulates; stated tolerance 0.1 %
  constexpr auto Tolerance = 1e-3;
  const auto engineAreas = procAreas(*f.proc);
  for (const auto& [key, area] : engineAreas)
  {
    CAPTURE(key.first, key.second, area, chunkAreas[key]);
    CHECK(closeArea(chunkAreas[key], area, Tolerance));
  }

  // the brush entities: entity-local vertices as dmap writes them (world - origin -
  // originOffset), so the chunk bounds are the .proc model's
  for (const auto& model : f.proc->models)
  {
    if (model.area >= 0)
    {
      continue;
    }
    const auto* entity = entityNamed(f.snapshot, model.name);
    REQUIRE(entity);
    auto points = std::vector<EngineVec3>{};
    for (const auto& surface : model.surfaces)
    {
      for (auto j = 0; j < surface.numVerts; ++j)
      {
        const auto* v = surface.verts.data() + std::size_t(j) * 8;
        points.push_back({v[0], v[1], v[2]});
      }
    }
    const auto expected = engineBoundsOfPoints(points);
    CAPTURE(model.name);
    REQUIRE(entityBounds.contains(entity->id));
    CHECK(entityBounds.at(entity->id) == expected);
  }

  // the entities' states: the pivot and axis of the rotation and origin-brush entity, and
  // the modelscale entity's
  auto states = std::map<std::string, ChunkEntityState>{};
  for (const auto& state : output.entities)
  {
    states[std::string{spawnString(input.entities.at(state.id)->keys, "name")}] = state;
  }
  REQUIRE(states.contains("lp1b_rotator"));
  const auto& rotator = states.at("lp1b_rotator");
  CHECK(rotator.originOffset == EngineVec3{12, 12, -12});
  CHECK(rotator.modelOrigin == EngineVec3{140, 76, 20});
  CHECK(rotator.parms.axis == EngineMat3{0, 1, 0, -1, 0, 0, 0, 0, 1});
  CHECK(rotator.visible);
  REQUIRE(states.contains("lp1b_scaled"));
  const auto& scaled = states.at("lp1b_scaled");
  CHECK(scaled.modelOrigin == EngineVec3{-128, -64, 32});
  CHECK(scaled.parms.axis == EngineMat3{2, 0, 0, 0, 2, 0, 0, 0, 2});
  CHECK(scaled.parms.shaderParms[3] == 1.0f);
}

} // namespace tb::lit
