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

#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/BprocModel.h"
#include "lit/EngineModels.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/ModelLoading.h"
#include "lit/ModelRecord.h"

#include "kd/result.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
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
    CHECK(engineFileExtension("models/test/n01") == "");
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

} // namespace tb::lit
