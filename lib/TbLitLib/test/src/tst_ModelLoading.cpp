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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model.cpp, renderer/Model_gltf.cpp and
// renderer/ModelManager.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.1a (C51): model loading on the CPU: the binary path with the engine's time test
// (D125), the GLMB tail (P7.19).

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "ModelValidators.h"
#include "fs/TestEnvironment.h"
#include "lit/BprocModel.h"
#include "lit/CpuPool.h"
#include "lit/EngineModels.h"
#include "lit/EngineTables.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/ModelLoading.h"
#include "lit/ModelRecord.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

std::span<const std::byte> bytesOf(const std::string& text)
{
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/**
 * A BRM 110 stream as WriteBinaryModel writes one (Model.cpp:632-790, read back by
 * LoadBinaryModel :386-623): big-endian numbers, little-endian floats, strings as a
 * little-endian length and the bytes.
 */
class BrmWriter
{
public:
  std::string out;

  void big(const std::uint64_t value, const int bytes)
  {
    for (auto i = bytes - 1; i >= 0; --i)
    {
      out.push_back(char((value >> (8 * i)) & 0xFF));
    }
  }

  void boolean(const bool value) { out.push_back(value ? char(1) : char(0)); }

  void little(const void* data, const std::size_t size)
  {
    out.append(static_cast<const char*>(data), size);
  }

  void vec3(const EngineVec3& v) { little(v.data(), 12); }

  void string(const std::string& text)
  {
    const auto length = std::int32_t(text.size());
    little(&length, 4);
    out += text;
  }

  void header(const std::int64_t timeStamp, const int surfaces)
  {
    // 'B' 'R' 'M' 110
    big(
      (std::uint64_t('B') << 24) | (std::uint64_t('R') << 16) | (std::uint64_t('M') << 8)
        | 110,
      4);
    big(std::uint64_t(timeStamp), 8);
    big(std::uint64_t(std::uint32_t(surfaces)), 4);
  }

  void surface(
    const int id,
    const std::string& material,
    const std::vector<EngineDrawVert>& verts,
    const std::vector<std::uint16_t>& indexes,
    const std::vector<int>& mirrored,
    const std::vector<int>& dupVerts,
    const std::array<EngineVec3, 2>& bounds)
  {
    big(std::uint64_t(std::uint32_t(id)), 4);
    string(material);
    boolean(true);
    vec3(bounds[0]);
    vec3(bounds[1]);
    // generateNormals, tangentsCalculated, perfectHull, referencedIndexes
    boolean(false);
    boolean(true);
    boolean(false);
    boolean(false);
    big(verts.size(), 4);
    big(verts.size(), 4);
    for (const auto& v : verts)
    {
      vec3(v.xyz);
      big(v.st[0], 2);
      big(v.st[1], 2);
      little(v.normal.data(), 4);
      little(v.tangent.data(), 4);
      little(v.color.data(), 4);
      little(v.color2.data(), 4);
    }
    big(indexes.size(), 4);
    for (const auto index : indexes)
    {
      big(index, 2);
    }
    // no sil indexes in the file
    big(0, 4);
    big(mirrored.size(), 4);
    for (const auto v : mirrored)
    {
      big(std::uint64_t(std::uint32_t(v)), 4);
    }
    big(dupVerts.size() / 2, 4);
    for (const auto v : dupVerts)
    {
      big(std::uint64_t(std::uint32_t(v)), 4);
    }
    // no dominant tris; no masked occlusion vertexes or indexes
    boolean(false);
    big(0, 4);
    big(0, 4);
  }

  // the model's bounds, the three frame counters, the name and the nine bools
  void tail(const std::array<EngineVec3, 2>& bounds, const std::array<bool, 9>& bools)
  {
    vec3(bounds[0]);
    vec3(bounds[1]);
    big(0, 4);
    big(0, 4);
    big(0, 4);
    string("models/test/hand.obj");
    for (const auto b : bools)
    {
      boolean(b);
    }
  }

  // Model_gltf.cpp's GLMB tail, as its writer writes the fields LoadBinaryModel reads
  void glmb(const std::int64_t declTime, const int modelState, const int animations)
  {
    big(
      (std::uint64_t('M') << 24) | (std::uint64_t('L') << 16) | (std::uint64_t('G') << 8)
        | 106,
      4);
    big(std::uint64_t(declTime), 8);
    string("");
    string("");
    big(std::uint64_t(std::uint32_t(modelState)), 4);
    big(0, 4);
    string("models/test/hand.glb");
    big(std::uint64_t(std::uint32_t(animations)), 4);
    for (auto i = 0; i < animations; ++i)
    {
      big(std::uint64_t(i), 4);
    }
    // the joints, bones and poses the reader does not read
    big(0, 4);
  }
};

EngineDrawVert handVert(const float x, const float y, const float z)
{
  auto v = EngineDrawVert{};
  v.xyz = {x, y, z};
  v.st = {0x3C00, 0x0000};
  v.normal = {128, 128, 255, 0};
  v.tangent = {255, 128, 128, 255};
  v.color = {255, 255, 255, 255};
  return v;
}

const auto HandBounds =
  std::array<EngineVec3, 2>{EngineVec3{0, 0, 0}, EngineVec3{8, 8, 0}};

// a one-surface stream of time; bools are the nine after the name
std::string handBinary(const std::int64_t time, const std::array<bool, 9>& bools)
{
  auto w = BrmWriter{};
  w.header(time, 1);
  w.surface(
    0,
    "lp7/n01",
    {handVert(0, 0, 0), handVert(8, 0, 0), handVert(8, 8, 0)},
    {0, 1, 2},
    {},
    {},
    HandBounds);
  w.tail(HandBounds, bools);
  return w.out;
}

constexpr auto StoredTime = std::int64_t{1451520000};

} // namespace

TEST_CASE(
  "ModelLoading: the binary model's trailing fields (LP7.1a, C51, Model.cpp:607-622)")
{
  SECTION("the nine bools are read; the has-surfaces flags kept")
  {
    // isStaticWorldModel, defaulted, purged, fastLoad, reloadable, levelLoadReferenced,
    // then hasDrawingSurfaces, hasInteractingSurfaces, hasShadowCastingSurfaces
    for (const auto& bools : {
           std::array<bool, 9>{true, true, true, true, true, true, true, false, true},
           std::array<bool, 9>{
             false, false, false, false, false, false, false, true, false},
         })
    {
      const auto stream = handBinary(StoredTime, bools);
      auto offset = std::size_t{0};
      const auto model = readBinaryModel(bytesOf(stream), offset);
      REQUIRE(model.is_success());
      CHECK(offset == stream.size());
      CHECK(model.value().hasDrawingSurfaces == bools[6]);
      CHECK(model.value().hasInteractingSurfaces == bools[7]);
      CHECK(model.value().hasShadowCastingSurfaces == bools[8]);
      CHECK(model.value().timeStamp == StoredTime);
      CHECK(model.value().name == "models/test/hand.obj");
    }
  }

  SECTION("a stream cut inside the nine bools is refused")
  {
    auto stream = handBinary(StoredTime, {});
    stream.resize(stream.size() - 3);
    auto offset = std::size_t{0};
    CHECK_FALSE(readBinaryModel(bytesOf(stream), offset).is_success());
  }

  SECTION("the GLMB tail: the modelDef time, model_state and the animation count (P7.19)")
  {
    auto w = BrmWriter{};
    w.out = handBinary(StoredTime, {});
    w.glmb(1700000000, 1, 3);
    auto offset = std::size_t{0};
    REQUIRE(readBinaryModel(bytesOf(w.out), offset).is_success());
    const auto tail = readGlmbTail(bytesOf(w.out), offset);
    REQUIRE(tail.is_success());
    CHECK(tail.value().declTimeStamp == 1700000000);
    CHECK(tail.value().modelState == 1);
    CHECK(tail.value().animationCount == 3);

    // another magic is not a GLMB tail
    auto bad = handBinary(StoredTime, {});
    bad += "XXXX";
    offset = 0;
    REQUIRE(readBinaryModel(bytesOf(bad), offset).is_success());
    CHECK_FALSE(readGlmbTail(bytesOf(bad), offset).is_success());
  }
}

TEST_CASE("ModelLoading, the binary lookup and the engine's time test (LP7.1a, D125)")
{
  SECTION("the rule: -1 and 0 always pass, another time must equal the stored one")
  {
    CHECK(binaryModelTimeAccepted(-1, StoredTime));
    CHECK(binaryModelTimeAccepted(0, StoredTime));
    CHECK(binaryModelTimeAccepted(StoredTime, StoredTime));
    CHECK_FALSE(binaryModelTimeAccepted(StoredTime + 1, StoredTime));
  }

  SECTION("GetModel's binary name (ModelManager.cpp:446-450)")
  {
    CHECK(
      engineGeneratedModelPath("models/test/n01.obj")
      == "generated/rendermodels/models/test/n01.bobj");
    CHECK(
      engineGeneratedModelPath("models\\a.b\\c.lwo")
      == "generated/rendermodels/models/a.b/c.blwo");
    CHECK(engineFileExtension("models/test/n01.obj") == "obj");
    CHECK(engineFileExtension("models/test/n01").empty());
  }

  // a loose game folder: the sources stamped through the file source, never the
  // checkout's time (P7.4)
  const auto env = fs::TestEnvironment{};
  const auto base = env.dir() / "base";
  const auto binary =
    handBinary(StoredTime, {false, false, false, false, true, false, true, true, true});
  const auto lookUp = [&](const std::string& canonical) {
    auto source = GameFileSource{env.dir()};
    const auto sourceTime = source.probe(canonical).time;
    auto lookup = lookUpModelBinary(source, canonical, sourceTime);
    source.closeFiles();
    return std::pair{lookup, sourceTime};
  };

  SECTION("no source (-1): the binary is accepted")
  {
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "a.bobj", binary);
    const auto [lookup, sourceTime] = lookUp("models/test/a.obj");
    CHECK(sourceTime == -1);
    CHECK(lookup.result == ModelBinaryLookup::Result::Accepted);
    REQUIRE(lookup.model);
    const auto record =
      modelRecordFromBinary("models/test/a.obj", *lookup.model, lookup.glmb, sourceTime);
    CHECK(record.state == ModelRecord::State::Ready);
    CHECK(record.source == ModelRecord::SourceKind::Binary);
    CHECK(record.binaryTime == StoredTime);
    CHECK(record.sourceTime == -1);
    REQUIRE(record.surfaces.size() == 1u);
    CHECK(record.surfaces[0].verts.size() == 3u);
    CHECK(record.surfaces[0].material == "lp7/n01");
    CHECK(record.surfaces[0].tangentsCalculated);
    CHECK(record.hasDrawingSurfaces);
    CHECK(record.hasInteractingSurfaces);
    CHECK(record.hasShadowCastingSurfaces);
  }

  SECTION("a source in a .pk4 (0): the binary is accepted")
  {
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "b.bobj", binary);
    writePk4(base / "models.pk4", {{"models/test/b.obj", "v 0 0 0\n"}});
    const auto [lookup, sourceTime] = lookUp("models/test/b.obj");
    CHECK(sourceTime == 0);
    CHECK(lookup.result == ModelBinaryLookup::Result::Accepted);
  }

  SECTION("a loose source of the stored time: accepted; of another time: refused")
  {
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "c.bobj", binary);
    const auto sourcePath = base / "models" / "test" / "c.obj";
    writeTextFile(sourcePath, "v 0 0 0\n");
    REQUIRE(setUnixModificationTime(sourcePath, StoredTime));
    {
      const auto [lookup, sourceTime] = lookUp("models/test/c.obj");
      CHECK(sourceTime == StoredTime);
      CHECK(lookup.result == ModelBinaryLookup::Result::Accepted);
    }
    REQUIRE(setUnixModificationTime(sourcePath, StoredTime + 86400));
    {
      const auto [lookup, sourceTime] = lookUp("models/test/c.obj");
      CHECK(sourceTime == StoredTime + 86400);
      CHECK(lookup.result == ModelBinaryLookup::Result::Stale);
      CHECK_FALSE(lookup.model);
      CHECK_THAT(lookup.reason, Catch::Matchers::ContainsSubstring("stale"));
    }
  }

  SECTION("no binary; an unreadable one")
  {
    {
      const auto [lookup, sourceTime] = lookUp("models/test/d.obj");
      CHECK(lookup.result == ModelBinaryLookup::Result::None);
    }
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "e.bobj",
      binary.substr(0, 40));
    const auto [lookup, sourceTime] = lookUp("models/test/e.obj");
    CHECK(lookup.result == ModelBinaryLookup::Result::Unreadable);
  }

  SECTION("a .bglb: the GLMB tail's fields reach the record (P7.19)")
  {
    auto w = BrmWriter{};
    w.out = binary;
    w.glmb(0, 1, 2);
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "f.bglb", w.out);
    const auto [lookup, sourceTime] = lookUp("models/test/f.glb");
    CHECK(lookup.result == ModelBinaryLookup::Result::Accepted);
    REQUIRE(lookup.glmb);
    REQUIRE(lookup.model);
    const auto record =
      modelRecordFromBinary("models/test/f.glb", *lookup.model, lookup.glmb, sourceTime);
    CHECK(record.glbModelState == 1);
    CHECK(record.glbAnimations == 2);
  }

  SECTION("an .md5mesh's .bmd5mesh is MD5B: never read")
  {
    writeTextFile(
      base / "generated" / "rendermodels" / "models" / "test" / "g.bmd5mesh", "MD5B");
    const auto [lookup, sourceTime] = lookUp("models/test/g.md5mesh");
    CHECK(lookup.result == ModelBinaryLookup::Result::Md5b);
    CHECK_FALSE(lookup.model);
  }
}

namespace
{

// the stand-in values of a plain drawn, lit, shadowing material without back sides
EngineModelMaterial plainMaterial(const std::string& name, const int index)
{
  auto material = EngineModelMaterial{};
  material.index = index;
  material.name = name;
  material.castsShadow = true;
  material.drawn = true;
  material.receivesLighting = true;
  return material;
}

// Model.cpp:226-254 AddCubeFace's corners, in MakeDefaultModel's face order (:283-290)
std::vector<std::array<float, 5>> cubeTranscription()
{
  using V = std::array<float, 3>;
  const auto faces = std::vector<std::array<V, 4>>{
    {V{-1, 1, 1}, V{1, 1, 1}, V{1, -1, 1}, V{-1, -1, 1}},
    {V{-1, 1, -1}, V{-1, -1, -1}, V{1, -1, -1}, V{1, 1, -1}},
    {V{1, -1, 1}, V{1, 1, 1}, V{1, 1, -1}, V{1, -1, -1}},
    {V{-1, -1, 1}, V{-1, -1, -1}, V{-1, 1, -1}, V{-1, 1, 1}},
    {V{-1, -1, 1}, V{1, -1, 1}, V{1, -1, -1}, V{-1, -1, -1}},
    {V{-1, 1, 1}, V{-1, 1, -1}, V{1, 1, -1}, V{1, 1, 1}},
  };
  const auto st = std::array<std::array<float, 2>, 4>{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
  auto verts = std::vector<std::array<float, 5>>{};
  for (const auto& face : faces)
  {
    for (auto i = 0; i < 4; ++i)
    {
      verts.push_back(
        {face[i][0] * 8, face[i][1] * 8, face[i][2] * 8, st[i][0], st[i][1]});
    }
  }
  return verts;
}

} // namespace

TEST_CASE(
  "ModelLoading: the dispatch of a canonical name (LP7.1a, D127, K40, K41, K46, K47)")
{
  const auto pathOf = [](const std::string& name) { return modelDispatch(name).path; };
  CHECK(pathOf("") == ModelPath::None);
  CHECK(pathOf("models/test/n01.obj") == ModelPath::Source);
  CHECK(pathOf("models/test/n04.ase") == ModelPath::PendingLoader);
  CHECK(pathOf("models/test/n05.lwo") == ModelPath::PendingLoader);
  CHECK(pathOf("models/test/a.ma") == ModelPath::MayaBinaryOnly);
  CHECK(pathOf("models/test/a.glb") == ModelPath::GltfBinaryOnly);
  CHECK(pathOf("models/test/a.gltf") == ModelPath::GltfBinaryOnly);
  CHECK(pathOf("models/test/a.md5mesh") == ModelPath::Skinned);
  CHECK(pathOf("_default") == ModelPath::DefaultModel);
  for (const auto* name :
       {"_beam",
        "_sprite",
        "models/a.md3",
        "models/a.prt",
        "models/a.liquid",
        "models/a.flt",
        "models/a.fbx",
        "models/noextension"})
  {
    INFO(name);
    const auto dispatch = modelDispatch(name);
    CHECK(dispatch.path == ModelPath::Default);
    CHECK_FALSE(dispatch.reason.empty());
  }
  CHECK_THAT(
    modelDispatch("models/test/n04.ase").reason,
    Catch::Matchers::ContainsSubstring("LP7.1b"));
  CHECK(modelDispatch("models/a.fbx").extension == "fbx");
}

TEST_CASE("ModelLoading: the default cube (LP7.1a, C51, Model.cpp:226-296)")
{
  // against a transcription only: the engine never writes its default model as a
  // binary ("default models shouldn't be cached", ModelManager.cpp:489-499), so no
  // capture holds a golden of it
  const auto cube =
    engineDefaultModel("models/test/missing.obj", plainMaterial("_default", 7));
  CHECK(cube.defaulted);
  CHECK_FALSE(cube.isStaticWorldModel);
  REQUIRE(cube.surfaces.size() == 1u);
  const auto& surface = cube.surfaces[0];
  CHECK(surface.material == "_default");
  CHECK(surface.materialIndex == -1);
  CHECK(surface.generateNormals);
  CHECK(surface.tangentsCalculated);

  // 24 vertices of AddCubeFace (no mirrored duplicates: no face shares a vertex)
  const auto expected = cubeTranscription();
  REQUIRE(surface.verts.size() == 24u);
  CHECK(surface.numMirroredVerts == 0);
  for (std::size_t i = 0; i < expected.size(); ++i)
  {
    INFO(i);
    CHECK(
      surface.verts[i].xyz == EngineVec3{expected[i][0], expected[i][1], expected[i][2]});
    const auto st = engineTexCoord(surface.verts[i]);
    CHECK(st == EngineVec2{expected[i][3], expected[i][4]});
    // the generated normal points out of the cube
    const auto n = engineNormal(surface.verts[i]);
    CHECK(n[0] * expected[i][0] + n[1] * expected[i][1] + n[2] * expected[i][2] > 0.0f);
    CHECK(int(surface.verts[i].color[0]) == 0);
  }
  // 36 indexes, (0, 1, 2), (0, 2, 3) per face
  REQUIRE(surface.indexes.size() == 36u);
  for (std::uint16_t f = 0; f < 6; ++f)
  {
    const auto b = std::uint16_t(f * 4);
    const auto at = std::size_t(f) * 6;
    CHECK(
      std::vector<std::uint16_t>(
        surface.indexes.begin() + std::ptrdiff_t(at),
        surface.indexes.begin() + std::ptrdiff_t(at) + 6)
      == std::vector<std::uint16_t>{
        b,
        std::uint16_t(b + 1),
        std::uint16_t(b + 2),
        b,
        std::uint16_t(b + 2),
        std::uint16_t(b + 3)});
  }
  // R_CreateDupVerts: each corner's later two vertices point at its first: 16 pairs
  CHECK(surface.dupVerts.size() == 32u);
  CHECK(
    surface.bounds
    == std::array<EngineVec3, 2>{EngineVec3{-8, -8, -8}, EngineVec3{8, 8, 8}});
  CHECK(cube.bounds == surface.bounds);
  CHECK(cube.hasDrawingSurfaces);
  CHECK(cube.hasInteractingSurfaces);
  CHECK(cube.hasShadowCastingSurfaces);

  SECTION("a record of it: Missing or Default, flagged")
  {
    const auto record = modelRecordFromEngine(
      "models/test/missing.obj",
      cube,
      ModelRecord::State::Missing,
      ModelRecord::SourceKind::Default,
      -1,
      "no file");
    CHECK(record.state == ModelRecord::State::Missing);
    CHECK(record.source == ModelRecord::SourceKind::Default);
    CHECK(record.reason == "no file");
    REQUIRE(record.surfaces.size() == 1u);
    CHECK(record.surfaces[0].verts == surface.verts);
    CHECK(record.surfaces[0].dupVerts == surface.dupVerts);
    CHECK(modelRecordBytes(record) > 24 * sizeof(EngineDrawVert));
  }

  SECTION("an _area name is a static world model (InitEmpty, Model.cpp:915-934)")
  {
    CHECK(engineDefaultModel("_area3", plainMaterial("_default", 7)).isStaticWorldModel);
  }
}

namespace
{

EngineDrawVert stVert(
  const float x,
  const float y,
  const float z,
  const float s,
  const float t,
  const EngineVec3& normal = {0, 0, 1})
{
  auto v = engineClearedVert();
  v.xyz = {x, y, z};
  engineSetTexCoord(v, s, t);
  engineSetNormal(v, normal);
  return v;
}

EngineVec3 normalized(const EngineVec3& v)
{
  const auto length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return {v[0] / length, v[1] / length, v[2] / length};
}

bool near(const EngineVec3& a, const EngineVec3& b, const float tolerance)
{
  return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance
         && std::abs(a[2] - b[2]) <= tolerance;
}

// a quad of two triangles with v/vt/vn corners on one material
const auto QuadObj = std::string{
  "# a quad\n"
  "v 0 0 0\nv 32 0 0\nv 32 32 0\nv 0 32 0\n"
  "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
  "vn 0 0 1\n"
  "usemtl lp7/test\n"
  "f 1/1/1 2/2/1 3/3/1\n"
  "f 1/1/1 3/3/1 4/4/1\n"};

std::vector<DeclFile> modelDeclFiles()
{
  return {DeclFile{
    "materials/lp7_models.mtr",
    materialText("lp7/test", "textures/lp7/test") + "\n"
      + materialText("lp7/other", "textures/lp7/other"),
    0}};
}

// the parse thread's half: OBJ_Load and the materials' capture
struct ParsedObj
{
  std::shared_ptr<const EngineObjModel> model;
  EngineModelMaterials materials;
};

ParsedObj parseObj(MaterialDecls& decls, const std::string& text, const std::int64_t time)
{
  auto parsed = ParsedObj{};
  auto read = decls.parseObjModel("models/test/quad.obj", text, time);
  REQUIRE(read.error.empty());
  REQUIRE(read.model);
  parsed.model = read.model;
  for (const auto& name : read.model->materialNames())
  {
    auto material = decls.modelMaterial(name);
    REQUIRE(material);
    parsed.materials.names.emplace_back(name, parsed.materials.materials.size());
    parsed.materials.materials.push_back(*material.value);
  }
  auto fallback = decls.defaultModelMaterial();
  REQUIRE(fallback);
  parsed.materials.defaultMaterial = *fallback.value;
  static_cast<void>(decls.takeMessages());
  return parsed;
}

} // namespace

TEST_CASE(
  "ModelLoading: R_CleanupTriangles whole (LP7.1a, C51 (3), tr_trisurf.cpp:1810-1847)")
{
  SECTION("the 2016 rule (Model.cpp:366-375): strictly after 1451638800")
  {
    CHECK_FALSE(engineUseMikktspace(1451520000));
    CHECK_FALSE(engineUseMikktspace(1451638800));
    CHECK(engineUseMikktspace(1451638801));
    CHECK(engineUseMikktspace(1483228800));
    CHECK_FALSE(engineUseMikktspace(0));
    CHECK_FALSE(engineUseMikktspace(-1));
  }

  SECTION("R_CreateDupVerts: the later vertices of a position point at its first")
  {
    // 3 and 4 repeat 2's and 1's positions with other texture coordinates
    const auto verts = std::vector<EngineDrawVert>{
      stVert(0, 0, 0, 0, 0),
      stVert(16, 0, 0, 1, 0),
      stVert(16, 16, 0, 1, 1),
      stVert(16, 16, 0, 0.5f, 0.5f),
      stVert(16, 0, 0, 0.5f, 0),
      stVert(0, 16, 0, 0, 1),
    };
    const auto result = engineCleanupModelTriangles(
      verts, std::vector<std::uint16_t>{0, 1, 2, 4, 3, 5}, true, false, false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    REQUIRE(result);
    CHECK(result->dupVerts == std::vector<int>{3, 2, 4, 1});
    CHECK(result->tangentsCalculated);
    CHECK(
      result->bounds
      == std::array<EngineVec3, 2>{EngineVec3{0, 0, 0}, EngineVec3{16, 16, 0}});
  }

  SECTION("generated normals sum over the dupVerts (tr_trisurf.cpp:984-994): the cube")
  {
    auto verts = std::vector<EngineDrawVert>{};
    for (const auto& c : cubeTranscription())
    {
      verts.push_back(stVert(c[0], c[1], c[2], c[3], c[4]));
    }
    auto indexes = std::vector<std::uint16_t>{};
    for (std::uint16_t f = 0; f < 6; ++f)
    {
      const auto b = std::uint16_t(f * 4);
      for (const auto i : {0, 1, 2, 0, 2, 3})
      {
        indexes.push_back(std::uint16_t(b + i));
      }
    }
    const auto result = engineCleanupModelTriangles(verts, indexes, true, false, false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    REQUIRE(result);
    REQUIRE(result->dupVerts.size() == 32u);
    REQUIRE(result->triangles.indexes == indexes);

    // a transcription of R_DeriveNormalsAndTangents' normals (:904-1012): the triangles'
    // unit normals summed per vertex, a duplicate given its first vertex's sum, the first
    // the last duplicate's, normalised
    auto sums = std::vector<EngineVec3>(verts.size(), EngineVec3{0, 0, 0});
    for (std::size_t i = 0; i < indexes.size(); i += 3)
    {
      const auto& a = verts[indexes[i]].xyz;
      const auto& b = verts[indexes[i + 1]].xyz;
      const auto& c = verts[indexes[i + 2]].xyz;
      const auto d0 = EngineVec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const auto d1 = EngineVec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const auto n = normalized(
        {d1[1] * d0[2] - d1[2] * d0[1],
         d1[2] * d0[0] - d1[0] * d0[2],
         d1[0] * d0[1] - d1[1] * d0[0]});
      for (auto k = 0; k < 3; ++k)
      {
        for (auto j = 0; j < 3; ++j)
        {
          sums[indexes[i + std::size_t(k)]][std::size_t(j)] += n[std::size_t(j)];
        }
      }
    }
    const auto& dup = result->dupVerts;
    for (std::size_t i = 0; i < dup.size(); i += 2)
    {
      for (auto j = 0; j < 3; ++j)
      {
        sums[std::size_t(dup[i])][std::size_t(j)] +=
          sums[std::size_t(dup[i + 1])][std::size_t(j)];
      }
    }
    for (std::size_t i = 0; i < dup.size(); i += 2)
    {
      sums[std::size_t(dup[i + 1])] = sums[std::size_t(dup[i])];
    }
    for (std::size_t i = 0; i < verts.size(); ++i)
    {
      INFO(i);
      const auto expected = normalized(sums[i]);
      const auto n = engineNormal(result->triangles.verts[i]);
      CHECK(near(n, expected, 0.02f));
      // smoothed: no corner keeps its face's axis normal
      const auto axes = int(std::abs(n[0]) > 0.1f) + int(std::abs(n[1]) > 0.1f)
                        + int(std::abs(n[2]) > 0.1f);
      CHECK(axes >= 2);
    }
  }

  SECTION(
    "MikkTSpace through the copied library: a flat quad with sheared texture coordinates")
  {
    // s = x / 32, t = y / 32 + x / 64: dP/ds is (32, -16, 0), the tangent MikkTSpace
    // makes on a flat surface, its sign positive
    const auto st = [](const float x, const float y) {
      return std::array<float, 2>{x / 32, y / 32 + x / 64};
    };
    auto verts = std::vector<EngineDrawVert>{};
    for (const auto& p : {std::array<float, 2>{0, 0}, {32, 0}, {32, 32}, {0, 32}})
    {
      const auto uv = st(p[0], p[1]);
      verts.push_back(stVert(p[0], p[1], 0, uv[0], uv[1]));
    }
    const auto indexes = std::vector<std::uint16_t>{0, 1, 2, 0, 2, 3};
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    const auto mikk = engineCleanupModelTriangles(verts, indexes, false, false, true);
    const auto messages = drainEngineMessagesForThread(std::this_thread::get_id());
    REQUIRE(mikk);
    CHECK(mikk->tangentsCalculated);
    CHECK(std::ranges::none_of(messages, [](const DeclMessage& m) {
      return m.text.find("Mikkelsen") != std::string::npos;
    }));
    const auto expected = normalized({32, -16, 0});
    for (const auto& v : mikk->triangles.verts)
    {
      CHECK(near(engineTangent(v), expected, 0.02f));
      CHECK(engineBiTangentSign(v) > 0.0f);
    }
  }

  SECTION(
    "the mutant: on smooth, folded, mirrored geometry the WithoutNormals path differs")
  {
    // a roof of two triangles folded along x = 16 with averaged normals, its right half
    // mirrored in s: MikkTSpace weights each face by its corner angle and keeps the
    // mirror's sign per face; R_DeriveTangentsWithoutNormals sums and splits mirrored
    // vertices (R_DuplicateMirroredVertexes)
    const auto up = normalized({0, 0, 1});
    const auto left = normalized({-1, 0, 1});
    const auto right = normalized({1, 0, 1});
    const auto verts = std::vector<EngineDrawVert>{
      stVert(0, 0, 0, 0, 0, left),
      stVert(16, 0, 12, 0.5f, 0.1f, up),
      stVert(16, 32, 12, 0.6f, 1, up),
      stVert(32, 0, 0, 0, 0.3f, right),
      stVert(0, 40, 0, 0.1f, 1.2f, left),
    };
    const auto indexes = std::vector<std::uint16_t>{0, 1, 2, 1, 3, 2, 0, 2, 4};
    const auto mikk = engineCleanupModelTriangles(verts, indexes, false, false, true);
    const auto plain = engineCleanupModelTriangles(verts, indexes, false, false, false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    REQUIRE(mikk);
    REQUIRE(plain);
    REQUIRE(mikk->triangles.verts.size() == plain->triangles.verts.size());
    auto differing = 0;
    for (std::size_t i = 0; i < mikk->triangles.verts.size(); ++i)
    {
      if (mikk->triangles.verts[i].tangent != plain->triangles.verts[i].tangent)
      {
        ++differing;
      }
    }
    CHECK(differing > 0);
  }
}

TEST_CASE("ModelLoading, FinishSurfaces on an OBJ through the copies (LP7.1a, D137)")
{
  auto source = MemoryDeclSource{modelDeclFiles()};

  SECTION("the source path: the corners reversed, merged by the slop, white, finished")
  {
    auto decls = MaterialDecls{source};
    const auto parsed = parseObj(decls, QuadObj, 1451520000);
    CHECK(parsed.model->timeStamp() == 1451520000);
    CHECK(parsed.model->materialNames() == std::vector<std::string>{"lp7/test"});
    const auto model = engineFinishObjModel(
      *parsed.model, parsed.materials, "models/test/quad.obj", false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    CHECK_FALSE(model.defaulted);
    CHECK(model.timeStamp == 1451520000);
    CHECK(model.materialMisses == 0);
    REQUIRE(model.surfaces.size() == 1u);
    const auto& surface = model.surfaces[0];
    CHECK(surface.material == "lp7/test");
    CHECK(surface.materialIndex == 0);
    CHECK_FALSE(surface.generateNormals);
    CHECK(surface.tangentsCalculated);
    // 4 positions of 6 corners
    CHECK(surface.verts.size() == 4u);
    CHECK(surface.indexes.size() == 6u);
    // the first triangle's corners in reverse: 3, 2, 1 (Model.cpp:1594)
    CHECK(surface.verts[surface.indexes[0]].xyz == EngineVec3{32, 32, 0});
    CHECK(surface.verts[surface.indexes[2]].xyz == EngineVec3{0, 0, 0});
    for (const auto& v : surface.verts)
    {
      CHECK(v.color == std::array<std::uint8_t, 4>{255, 255, 255, 255});
    }
    CHECK(
      model.bounds
      == std::array<EngineVec3, 2>{EngineVec3{0, 0, 0}, EngineVec3{32, 32, 0}});
  }

  SECTION(
    "a material with back sides: a reversed copy as a surface of its own (:1252-1276)")
  {
    auto decls = MaterialDecls{source};
    auto parsed = parseObj(decls, QuadObj, 1451520000);
    parsed.materials.materials[0].createBackSides = true;
    const auto model = engineFinishObjModel(
      *parsed.model, parsed.materials, "models/test/quad.obj", false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    REQUIRE(model.surfaces.size() == 2u);
    const auto& front = model.surfaces[0];
    const auto& back = model.surfaces[1];
    CHECK(back.material == front.material);
    REQUIRE(back.indexes.size() == front.indexes.size());
    CHECK(back.indexes[0] == front.indexes[1]);
    CHECK(back.indexes[1] == front.indexes[0]);
    // R_CopyStaticTriSurf copies vertices and indexes only: the copy does not generate
    // its normals, R_ReverseTriangles' negated ones stand
    CHECK_FALSE(back.generateNormals);
    CHECK(engineNormal(back.verts[0])[2] < -0.99f);
    CHECK(engineNormal(front.verts[0])[2] > 0.99f);
  }

  SECTION("a deforming material grows the bounds (:1346-1360)")
  {
    auto decls = MaterialDecls{source};
    auto parsed = parseObj(decls, QuadObj, 1451520000);
    // DFRM_SPRITE
    parsed.materials.materials[0].deform = 1;
    const auto model = engineFinishObjModel(
      *parsed.model, parsed.materials, "models/test/quad.obj", false);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    const auto radius = std::sqrt(16.0f * 16.0f + 16.0f * 16.0f) + 20.0f;
    CHECK(std::abs(model.bounds[0][0] - (16 - radius)) < 0.001f);
    CHECK(std::abs(model.bounds[1][1] - (16 + radius)) < 0.001f);
    CHECK(std::abs(model.bounds[0][2] + radius) < 0.001f);
  }

  SECTION("the pool job reads nothing of the registry: it is gone when the job runs")
  {
    auto parsed = ParsedObj{};
    auto reference = EngineModel{};
    {
      auto decls = MaterialDecls{source};
      parsed = parseObj(decls, QuadObj, 1483228800);
      reference = engineFinishObjModel(
        *parsed.model, parsed.materials, "models/test/quad.obj", true);
      static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    }
    auto pool = CpuPool::create(1);
    REQUIRE(pool.is_success());
    auto future =
      pool.value()->run([parsed](const CpuJobContext&) -> Result<EngineModel> {
        return engineFinishObjModel(
          *parsed.model, parsed.materials, "models/test/quad.obj", true);
      });
    REQUIRE(future.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    auto output = future.get();
    REQUIRE(output.result.is_success());
    const auto& model = output.result.value();
    REQUIRE(model.surfaces.size() == reference.surfaces.size());
    for (std::size_t i = 0; i < model.surfaces.size(); ++i)
    {
      CHECK(model.surfaces[i].verts == reference.surfaces[i].verts);
      CHECK(model.surfaces[i].indexes == reference.surfaces[i].indexes);
      CHECK(model.surfaces[i].dupVerts == reference.surfaces[i].dupVerts);
    }
    CHECK(model.bounds == reference.bounds);
    CHECK(model.hasDrawingSurfaces == reference.hasDrawingSurfaces);

    // and again while another registry holds the globals
    auto other = MaterialDecls{source};
    auto again = pool.value()->run([parsed](const CpuJobContext&) -> Result<EngineModel> {
      return engineFinishObjModel(
        *parsed.model, parsed.materials, "models/test/quad.obj", true);
    });
    REQUIRE(again.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
    auto againOutput = again.get();
    REQUIRE(againOutput.result.is_success());
    CHECK(againOutput.result.value().surfaces[0].verts == reference.surfaces[0].verts);
    pool.value()->stop();
  }
}

namespace
{

// the corners of a valid triangle after the lists of three positions, one texture
// coordinate and one normal
const auto ObjHead = std::string{"v 0 0 0\nv 16 0 0\nv 16 16 0\nvt 0 0\nvn 0 0 1\n"};

std::vector<DeclFile> discreteDeclFiles()
{
  auto files = modelDeclFiles();
  files.push_back(DeclFile{
    "materials/lp7_discrete.mtr",
    "lp7/discrete\n{\n\tdeform sprite\n\t{\n\t\tblend add\n\t\tmap "
    "textures/lp7/discrete\n\t}\n}\n",
    0});
  return files;
}

} // namespace

TEST_CASE("ModelLoading: the OBJ validator (LP7.1a, D136, P7.2)")
{
  SECTION("what the copies read without a fatal site passes")
  {
    CHECK_FALSE(objFileProblem(QuadObj));
    CHECK_FALSE(objFileProblem(ObjHead + "usemtl a b\nf 1/1/1 2/1/1 3/1/1\n"));
    // Blender's header, groups, smoothing, mtllib, CRLF line ends, exponents, signs
    CHECK_FALSE(objFileProblem(
      "# Blender 4.2\r\nmtllib x.mtl\r\no Cube\r\nv -1.000000 1e-05 .5\r\nv 1. 2.5e+3 "
      "0\r\nv 0 -0.25 3\r\nvt 0.5 1\r\nvn 0 0 -1\r\ns off\r\ng x\r\nl 1 2\r\nf 1/1/1 "
      "2/1/1 3/1/1\r\n"));
    // no face at all: an empty model, as the engine reads it
    CHECK_FALSE(objFileProblem("# nothing\n"));
  }

  const auto refused = [](const std::string& text, const std::string& reason) {
    INFO(text);
    const auto problem = objFileProblem(text);
    REQUIRE(problem);
    CHECK_THAT(*problem, Catch::Matchers::ContainsSubstring(reason));
  };

  SECTION("each refusal names its line and cause")
  {
    // :186's FatalError: a quad, two corners, v//vn corners
    refused(ObjHead + "v 0 16 0\nf 1/1/1 2/1/1 3/1/1 4/1/1\n", "three corners");
    refused(ObjHead + "f 1/1/1 2/1/1\n", "three corners");
    refused(ObjHead + "f 1//1 2//1 3//1\n", "positive number");
    // a negative index; an index past the count read so far
    refused(ObjHead + "f -1/1/1 2/1/1 3/1/1\n", "positive number");
    refused(ObjHead + "f 1/1/1 2/1/1 4/1/1\n", "past the 3");
    refused(ObjHead + "f 1/2/1 2/1/1 3/1/1\n", "texture coordinate past the 1");
    refused(
      "f 1/1/1 2/1/1 3/1/1\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvn 0 0 1\n", "past the 0");
    // :211's FatalError: an unknown token, a stray number, "##", a C++ comment
    refused(ObjHead + "vp 0 0 0\n", "unexpected token 'vp'");
    refused("v 0 0 0 1\n", "takes 3 numbers, not 4");
    refused("vt 0 0 0\n", "takes 2 numbers, not 3");
    refused("## x\n", "'##'");
    refused("// x\n", "unexpected token '//'");
    // ParseFloat's Errors: an octal, a hex, two dots, an upper-case exponent, a plus
    refused("v 01 0 0\n", "'01'");
    refused("v 0x1 0 0\n", "'0x1'");
    refused("v 1.2.3 0 0\n", "'1.2.3'");
    refused("v 1E5 0 0\n", "'1E5'");
    refused("v +1 0 0\n", "'+1'");
    // a truncated file, an empty one, a NUL byte, a long usemtl
    refused(ObjHead + "f 1/1/1 2/1/1 3/1", "is not v/vt/vn");
    refused("v 0 0", "takes 3 numbers, not 2");
    refused("", "empty");
    refused(std::string{"v 0 0 0\n\0", 9}, "NUL");
    refused("usemtl " + std::string(256, 'm') + "\n", "256");
    CHECK_THAT(*objFileProblem(ObjHead + "vp\n"), Catch::Matchers::StartsWith("line 6:"));
  }

  SECTION("the corner bound")
  {
    auto text = ObjHead;
    for (auto i = std::size_t{0}; i < MaxObjCorners / 3; ++i)
    {
      text += "f 1/1/1 2/1/1 3/1/1\n";
    }
    CHECK_FALSE(objFileProblem(text));
    text += "f 1/1/1 2/1/1 3/1/1\n";
    refused(text, "face corners");
  }
}

TEST_CASE(
  "ModelLoading, the OBJ source path through the validator and the copies (LP7.1a, C51 "
  "(3))")
{
  CHECK(engineModelCvars().mergeModelSurfaces == EngineFrame.mergeModelSurfaces);
  CHECK(engineModelCvars().slopVertex == EngineFrame.slopVertex);
  CHECK(engineModelCvars().slopTexCoord == EngineFrame.slopTexCoord);
  CHECK(engineModelCvars().slopNormal == EngineFrame.slopNormal);

  auto source = MemoryDeclSource{discreteDeclFiles()};
  auto decls = MaterialDecls{source};
  const auto finished = [&](const std::string& text, const std::int64_t time) {
    auto read = readObjSource(decls, "models/test/x.obj", text, time);
    static_cast<void>(decls.takeMessages());
    REQUIRE(read.reason.empty());
    REQUIRE(read.input);
    auto model = finishModel(*read.input);
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    return std::pair{std::move(*read.input), std::move(model)};
  };

  SECTION("a refused file: Default with the reason; the copies never read it")
  {
    for (const auto& text :
         {ObjHead + "v 0 16 0\nf 1/1/1 2/1/1 3/1/1 4/1/1\n",
          ObjHead + "f -1/1/1 2/1/1 3/1/1\n",
          ObjHead + "f 1/1/1 2/1/1 9/1/1\n",
          ObjHead + "usemtl lp7/test\nbogus\n",
          ObjHead + "f 1/1/1 2/",
          std::string{}})
    {
      INFO(text);
      const auto messagesBefore = decls.messages().size();
      const auto read = readObjSource(decls, "models/test/x.obj", text, 1451520000);
      CHECK_THAT(read.reason, Catch::Matchers::ContainsSubstring("models/test/x.obj"));
      // the cube's input, and nothing the copied lexer reported
      REQUIRE(read.input);
      CHECK_FALSE(read.input->obj);
      const auto& messages = decls.messages();
      for (auto i = messagesBefore; i < messages.size(); ++i)
      {
        CHECK(messages[i].kind != DeclMessageKind::Error);
        CHECK(messages[i].kind != DeclMessageKind::FatalError);
      }
      const auto cube = finishModel(*read.input);
      CHECK(cube.defaulted);
      CHECK(cube.surfaces.size() == 1u);
      const auto record = modelRecordFromEngine(
        "models/test/x.obj",
        cube,
        ModelRecord::State::Default,
        ModelRecord::SourceKind::Default,
        1451520000,
        read.reason);
      CHECK(record.state == ModelRecord::State::Default);
      CHECK(record.reason == read.reason);
    }
  }

  SECTION("a valid OBJ through the copies: the 2016 rule on the time handed in")
  {
    const auto [input2015, model2015] = finished(QuadObj, 1451520000);
    CHECK_FALSE(input2015.useMikktspace);
    const auto [input2017, model2017] = finished(QuadObj, 1483228800);
    CHECK(input2017.useMikktspace);
    REQUIRE(model2015.surfaces.size() == 1u);
    CHECK(model2015.surfaces[0].verts.size() == 4u);
    CHECK(model2015.timeStamp == 1451520000);
    CHECK(model2017.timeStamp == 1483228800);
    CHECK(input2015.materials.names.size() == 1u);
    CHECK(input2015.materials.materials[0].name == "lp7/test");
    CHECK(input2015.materials.defaultMaterial.name == "_default");
  }

  SECTION("the slop merge: positions 0.005 apart are one vertex, 0.02 apart two")
  {
    const auto obj = [](const std::string& x) {
      return "v 0 0 0\nv 32 0 0\nv 32 32 0\nv 0 32 0\nv " + x
             + " 32 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nusemtl "
               "lp7/test\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 5/3/1 4/4/1\n";
    };
    const auto [nearInput, nearModel] = finished(obj("32.005"), 1451520000);
    const auto [farInput, farModel] = finished(obj("32.02"), 1451520000);
    REQUIRE(nearModel.surfaces.size() == 1u);
    REQUIRE(farModel.surfaces.size() == 1u);
    CHECK(nearModel.surfaces[0].verts.size() == 4u);
    CHECK(farModel.surfaces[0].verts.size() == 5u);
  }

  SECTION("objects of one material merge into one surface, unless it IsDiscrete")
  {
    const auto obj = [](const std::string& material) {
      return ObjHead + "o first\nusemtl " + material
             + "\nf 1/1/1 2/1/1 3/1/1\no second\nusemtl " + material
             + "\nf 1/1/1 3/1/1 2/1/1\n";
    };
    const auto [plainInput, plain] = finished(obj("lp7/test"), 1451520000);
    CHECK(plainInput.obj->objectCount() == 2u);
    CHECK(plain.surfaces.size() == 1u);
    const auto [discreteInput, discrete] = finished(obj("lp7/discrete"), 1451520000);
    REQUIRE(discreteInput.materials.materials.size() == 1u);
    CHECK(discreteInput.materials.materials[0].discrete);
    CHECK(discreteInput.materials.materials[0].deform == 1);
    CHECK(discrete.surfaces.size() == 2u);
  }

  SECTION("two spellings of one material are one material, as FindMaterial's pointer")
  {
    const auto text = ObjHead + "o a\nusemtl lp7/test\nf 1/1/1 2/1/1 3/1/1\no b\nusemtl LP7/Test\nf 1/1/1 3/1/1 2/1/1\n";
    const auto [input, model] = finished(text, 1451520000);
    CHECK(input.materials.names.size() == 2u);
    CHECK(input.materials.materials.size() == 1u);
    CHECK(model.surfaces.size() == 1u);
  }
}

namespace
{

std::filesystem::path modelsFixture()
{
  return litFixtureFolder() / "models";
}

// the fixture's model materials, as the engine's registry parses them
std::vector<DeclFile> fixtureModelDecls()
{
  return {DeclFile{
    "materials/lp7_models.mtr",
    readTextFile(litFixtureFolder() / "materials" / "lp7_models.mtr"),
    0}};
}

// the preview's source load of a fixture model (the parse thread's half and the pool's)
// with the time handed in, as MaterialService's Model item makes it
EngineModel sourceLoad(
  MaterialDecls& decls,
  const std::string& name,
  const std::int64_t time,
  const bool* mikktspace = nullptr)
{
  const auto bytes = readTextFile(litFixtureFolder() / name);
  auto read = readObjSource(decls, name, bytes, time);
  static_cast<void>(decls.takeMessages());
  INFO(read.reason);
  REQUIRE(read.reason.empty());
  REQUIRE(read.input);
  if (mikktspace)
  {
    read.input->useMikktspace = *mikktspace;
  }
  auto model = finishModel(*read.input);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  return model;
}

} // namespace

TEST_CASE("ModelLoading, the fixture models before the capture (LP7.1a, C56, P7.10)")
{
  auto source = MemoryDeclSource{fixtureModelDecls()};
  auto decls = MaterialDecls{source};

  SECTION("every fixture OBJ passes the validator")
  {
    for (const auto* name :
         {"models/test/n01.obj", "models/test/n02.obj", "models/test/n03.obj"})
    {
      INFO(name);
      CHECK_FALSE(objFileProblem(readTextFile(litFixtureFolder() / name)));
    }
  }

  SECTION("N03's MikkTSpace tangents are not the WithoutNormals path's (P7.10)")
  {
    const auto mikk = true;
    const auto without = false;
    const auto withMikk = sourceLoad(decls, "models/test/n03.obj", 1483228800, &mikk);
    const auto withoutMikk =
      sourceLoad(decls, "models/test/n03.obj", 1483228800, &without);
    REQUIRE(withMikk.surfaces.size() == 1u);
    REQUIRE(withoutMikk.surfaces.size() == 1u);
    const auto& a = withMikk.surfaces[0].verts;
    const auto& b = withoutMikk.surfaces[0].verts;
    REQUIRE(a.size() == b.size());
    auto differing = std::size_t{0};
    for (std::size_t i = 0; i < a.size(); ++i)
    {
      differing += a[i].tangent != b[i].tangent ? 1 : 0;
    }
    std::cout << fmt::format(
      "N03: {} vertices ({} mirrored), {} with other tangents under MikkTSpace\n",
      a.size(),
      withMikk.surfaces[0].numMirroredVerts,
      differing);
    CHECK(differing > 0);
    CHECK(withMikk.surfaces[0].numMirroredVerts > 0);
  }
}

namespace
{

// stamps.json's "stamps" object: each fixture source's stamp (parity.py capture-models)
std::int64_t fixtureStamp(const std::string& source)
{
  const auto text = readTextFile(modelsFixture() / "stamps.json");
  const auto key = "\"" + source + "\": ";
  const auto at = text.find(key, text.find("\"stamps\""));
  REQUIRE(at != std::string::npos);
  return std::stoll(text.substr(at + key.size(), 16));
}

// the engine's binary of a fixture source, as the preview reads it
ModelRecord goldenRecord(const std::string& golden, const std::int64_t sourceTime)
{
  const auto bytes = readTextFile(litFixtureFolder() / golden);
  auto offset = std::size_t{0};
  const auto read = readBinaryModel(
    std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()},
    offset);
  REQUIRE(read.is_success());
  CHECK(offset == bytes.size());
  return modelRecordFromBinary(golden, read.value(), std::nullopt, sourceTime);
}

// every field of the record the golden holds, compared; the first that differs is named
std::vector<std::string> recordDifferences(
  const ModelRecord& preview, const ModelRecord& engine)
{
  auto differences = std::vector<std::string>{};
  const auto differ = [&](const std::string& what) { differences.push_back(what); };
  if (preview.surfaces.size() != engine.surfaces.size())
  {
    differ(fmt::format(
      "surface count {} != {}", preview.surfaces.size(), engine.surfaces.size()));
    return differences;
  }
  for (std::size_t s = 0; s < preview.surfaces.size(); ++s)
  {
    const auto& a = preview.surfaces[s];
    const auto& b = engine.surfaces[s];
    if (a.material != b.material)
    {
      differ(fmt::format("surface {} material {} != {}", s, a.material, b.material));
    }
    if (a.verts.size() != b.verts.size())
    {
      differ(fmt::format(
        "surface {} vertex count {} != {}", s, a.verts.size(), b.verts.size()));
    }
    for (std::size_t v = 0; v < std::min(a.verts.size(), b.verts.size()); ++v)
    {
      const auto& p = a.verts[v];
      const auto& q = b.verts[v];
      if (p.xyz != q.xyz)
      {
        differ(fmt::format("surface {} vertex {} xyz", s, v));
      }
      if (p.st != q.st)
      {
        differ(fmt::format("surface {} vertex {} st", s, v));
      }
      if (p.normal != q.normal)
      {
        differ(fmt::format("surface {} vertex {} normal", s, v));
      }
      if (p.tangent != q.tangent)
      {
        differ(fmt::format(
          "surface {} vertex {} tangent ({} {} {} {} != {} {} {} {})",
          s,
          v,
          p.tangent[0],
          p.tangent[1],
          p.tangent[2],
          p.tangent[3],
          q.tangent[0],
          q.tangent[1],
          q.tangent[2],
          q.tangent[3]));
      }
      if (p.color != q.color)
      {
        differ(fmt::format("surface {} vertex {} color", s, v));
      }
      if (p.color2 != q.color2)
      {
        differ(fmt::format("surface {} vertex {} color2", s, v));
      }
    }
    if (a.indexes != b.indexes)
    {
      differ(fmt::format("surface {} indexes", s));
    }
    if (a.dupVerts != b.dupVerts)
    {
      differ(fmt::format("surface {} dupVerts", s));
    }
    if (a.mirroredVerts != b.mirroredVerts)
    {
      differ(fmt::format("surface {} mirroredVerts", s));
    }
    if (a.bounds != b.bounds)
    {
      differ(fmt::format("surface {} bounds", s));
    }
    if (a.generateNormals != b.generateNormals)
    {
      differ(fmt::format("surface {} generateNormals", s));
    }
    if (a.tangentsCalculated != b.tangentsCalculated)
    {
      differ(fmt::format("surface {} tangentsCalculated", s));
    }
  }
  if (preview.bounds != engine.bounds)
  {
    differ("the model's bounds");
  }
  if (
    preview.hasDrawingSurfaces != engine.hasDrawingSurfaces
    || preview.hasInteractingSurfaces != engine.hasInteractingSurfaces
    || preview.hasShadowCastingSurfaces != engine.hasShadowCastingSurfaces)
  {
    differ("the has-surfaces flags");
  }
  return differences;
}

} // namespace

TEST_CASE(
  "ModelLoading, the source path against the engine's binaries (LP7.1a, C56, the "
  "goldens)")
{
  auto source = MemoryDeclSource{fixtureModelDecls()};
  auto decls = MaterialDecls{source};

  // N01 (2015, WithoutNormals), N02 (the same OBJ under a second name), N03 (2017,
  // MikkTSpace on smooth normals with sheared, mirrored coordinates)
  for (const auto& [name, golden] : std::vector<std::pair<std::string, std::string>>{
         {"models/test/n01.obj", "models/test/n01.bobj"},
         {"models/test/n02.obj", "models/test/n02.bobj"},
         {"models/test/n03.obj", "models/test/n03.bobj"}})
  {
    DYNAMIC_SECTION(name)
    {
      const auto stamp = fixtureStamp(name);
      const auto engine = goldenRecord(golden, stamp);
      // the engine stored the source's time it read (Model.cpp:400, MM:496)
      CHECK(engine.binaryTime == stamp);
      CHECK(binaryModelTimeAccepted(stamp, engine.binaryTime));

      const auto model = sourceLoad(decls, name, stamp);
      const auto preview = modelRecordFromEngine(
        name,
        model,
        ModelRecord::State::Ready,
        ModelRecord::SourceKind::Source,
        stamp,
        {});
      const auto differences = recordDifferences(preview, engine);
      for (const auto& difference : differences)
      {
        UNSCOPED_INFO(difference);
      }
      CHECK(differences.empty());

      // the binary reader on the same file: the golden itself, read twice the same
      CHECK(recordDifferences(goldenRecord(golden, stamp), engine).empty());
    }
  }

  SECTION("the mutant: N03 through the WithoutNormals path fails its golden (P7.10)")
  {
    const auto stamp = fixtureStamp("models/test/n03.obj");
    const auto engine = goldenRecord("models/test/n03.bobj", stamp);
    const auto without = false;
    const auto model = sourceLoad(decls, "models/test/n03.obj", stamp, &without);
    const auto preview = modelRecordFromEngine(
      "models/test/n03.obj",
      model,
      ModelRecord::State::Ready,
      ModelRecord::SourceKind::Source,
      stamp,
      {});
    const auto differences = recordDifferences(preview, engine);
    CHECK_FALSE(differences.empty());
    CHECK(std::ranges::all_of(
      differences, [](const auto& d) { return d.find("tangent") != std::string::npos; }));
  }

  SECTION("N01 and N02 hold the same geometry; their names make their bytes differ")
  {
    const auto n01 =
      goldenRecord("models/test/n01.bobj", fixtureStamp("models/test/n01.obj"));
    const auto n02 =
      goldenRecord("models/test/n02.bobj", fixtureStamp("models/test/n02.obj"));
    CHECK(recordDifferences(n01, n02).empty());
  }

  SECTION("N04's ASE and N05's LWO goldens exist for LP7.1b, their stamps 2015")
  {
    CHECK(std::filesystem::is_regular_file(modelsFixture() / "test" / "n04.base"));
    CHECK(std::filesystem::is_regular_file(modelsFixture() / "test" / "n05.blwo"));
    CHECK(fixtureStamp("models/test/n04.ase") == 1451520000);
    CHECK(fixtureStamp("models/test/n05.lwo") == 1451520000);
    CHECK(goldenRecord("models/test/n04.base", 1451520000).binaryTime == 1451520000);
  }
}

} // namespace tb::lit
