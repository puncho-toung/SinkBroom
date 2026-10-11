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

#include "TestEnvironment.h"
#include "base/Logger.h"
#include "fs/DiskFileSystem.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "gl/CreateResource.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityModel.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/LoadIdTech4ObjModel.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/MaterialUtils.h"
#include "mdl/Quake3Shader.h"

#include "kd/result.h"

#include "vm/approx.h"
#include "vm/bbox_io.h" // IWYU pragma: keep
#include "vm/ray.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::mdl
{
namespace
{

class CollectingLogger : public Logger
{
public:
  std::vector<std::string> warnings;

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    if (level == LogLevel::Warn)
    {
      warnings.emplace_back(message);
    }
  }
};

std::string readFile(const fs::FileSystem& fs, const std::filesystem::path& path)
{
  const auto file = fs.openFile(path) | kdl::value();
  auto reader = file->reader().buffer();
  return std::string{reader.stringView()};
}

IdTech4ObjTriangle makeTriangle(
  const std::optional<size_t> materialIndex,
  const vm::vec3f& p0,
  const vm::vec2f& uv0,
  const vm::vec3f& p1,
  const vm::vec2f& uv1,
  const vm::vec3f& p2,
  const vm::vec2f& uv2)
{
  return {materialIndex, {{{p0, uv0}, {p1, uv1}, {p2, uv2}}}};
}

/**
 * The normal of a triangle by TrenchBroom's convention that front faces are clockwise
 * (see vm::from_points).
 */
vm::vec3f clockwiseNormal(const IdTech4ObjTriangle& triangle)
{
  const auto& [c0, c1, c2] = triangle.corners;
  return vm::normalize(vm::cross(c2.position - c0.position, c1.position - c0.position));
}

} // namespace

TEST_CASE("LoadIdTech4ObjModel")
{
  const auto fixturePath = getFixtureRoot() / "test/mdl/LoadIdTech4ObjModel";
  auto fs = fs::DiskFileSystem{fixturePath};

  const auto modelsPath = std::filesystem::path{"_tb/models/mapobjects/test"};

  SECTION("parseIdTech4Obj")
  {
    SECTION("an engine export keeps its positions, flips V back and reverses faces")
    {
      // written like idRenderModelStatic::ExportOBJ: surface 0 was (0,0,0) st(0,0),
      // (64,0,0) st(1,0), (0,32,16) st(0,0.25) and surface 1 (0,0,-8) st(0.5,0.5),
      // (64,0,-8) st(1,0.5), (0,32,-8) st(0.5,1), both with indexes 0 1 2
      const auto result = parseIdTech4Obj(readFile(fs, modelsPath / "wedge.obj"));
      REQUIRE(result.is_success());

      CHECK(
        result.value().materialNames
        == std::vector<std::filesystem::path>{
          "models/mapobjects/test/wedge", "textures/base_wall/lfwall27d"});

      // the file has vt 0 0.75 and f 3 2 1: the V flip gives back t = 0.25, and the
      // reversal gives back the engine's order 1 2 3
      // the second triangle uses global indices 4 to 6
      CHECK(
        result.value().triangles
        == std::vector<IdTech4ObjTriangle>{
          makeTriangle(0, {0, 0, 0}, {0, 0}, {64, 0, 0}, {1, 0}, {0, 32, 16}, {0, 0.25f}),
          makeTriangle(
            1,
            {0, 0, -8},
            {0.5f, 0.5f},
            {64, 0, -8},
            {1, 0.5f},
            {0, 32, -8},
            {0.5f, 1})});

      CHECK(result.value().engineIncompatibility == std::nullopt);
    }

    SECTION("the shared Doom 3 BFG fixture proxy, with CRLF line endings")
    {
      auto gameFs = fs::DiskFileSystem{getFixtureRoot() / "test/mdl/Game/Doom3BFG/base"};
      const auto str = readFile(gameFs, "_tb/models/tb_test/arrow.obj");
      REQUIRE(str.find("\r\n") != std::string::npos);

      const auto result = parseIdTech4Obj(str);
      REQUIRE(result.is_success());

      const auto& objData = result.value();
      CHECK(
        objData.materialNames
        == std::vector<std::filesystem::path>{
          "textures/tb_test/wall", "textures/tb_test/grate"});
      REQUIRE(objData.triangles.size() == 20u);

      // f 1/1/1 2/2/2 3/3/3 with vt 0 1, 1 1 and 1 0 on the cap at x = -32
      CHECK(
        objData.triangles[0]
        == makeTriangle(
          0, {-32, 4, 4}, {1, 1}, {-32, -4, 4}, {1, 0}, {-32, -4, -4}, {0, 0}));
      // clockwise seen from outside, so the cap faces away from the arrow
      CHECK(clockwiseNormal(objData.triangles[0]) == vm::vec3f{-1, 0, 0});
      // the first triangle of the second surface, f 37/37/37 38/38/38 39/39/39
      CHECK(
        objData.triangles[16]
        == makeTriangle(
          1, {-12, 0, 4}, {0, 1}, {-32, 0, 20}, {1, 0}, {-32, 0, 4}, {0, 0}));

      CHECK(
        std::ranges::count(
          objData.triangles, std::optional<size_t>{0}, &IdTech4ObjTriangle::materialIndex)
        == 16);
      CHECK(
        std::ranges::count(
          objData.triangles, std::optional<size_t>{1}, &IdTech4ObjTriangle::materialIndex)
        == 4);

      CHECK(objData.engineIncompatibility == std::nullopt);
    }

    SECTION("usemtl names are canonicalized like the engine's decl names")
    {
      const auto result = parseIdTech4Obj(readFile(fs, modelsPath / "noncanonical.obj"));
      REQUIRE(result.is_success());

      CHECK(
        result.value().materialNames
        == std::vector<std::filesystem::path>{"models/mapobjects/test/wedge"});
    }

    SECTION("polygons are split into fans, and relative indices are resolved")
    {
      const auto result =
        parseIdTech4Obj(readFile(fs, modelsPath / "polygon_and_nomtl.obj"));
      REQUIRE(result.is_success());

      CHECK(result.value().materialNames.empty());

      // the quad 1 2 3 4 becomes 1 2 3 and 1 3 4, each reversed; -3 -2 -1 are 5 6 7
      CHECK(
        result.value().triangles
        == std::vector<IdTech4ObjTriangle>{
          makeTriangle(
            std::nullopt, {32, 16, 0}, {1, 0}, {32, 0, 0}, {1, 1}, {0, 0, 0}, {0, 1}),
          makeTriangle(
            std::nullopt, {0, 16, 0}, {0, 0}, {32, 16, 0}, {1, 0}, {0, 0, 0}, {0, 1}),
          makeTriangle(
            std::nullopt,
            {0, 8, 8},
            {0.25f, 0.25f},
            {16, 0, 8},
            {0.5f, 0.5f},
            {0, 0, 8},
            {0.25f, 0.5f})});

      // a counter-clockwise polygon seen from +Z faces +Z
      CHECK(clockwiseNormal(result.value().triangles[0]) == vm::vec3f{0, 0, 1});

      // the quad is the first face that the engine can't read
      CHECK(
        result.value().engineIncompatibility
        == "At line 11: a face corner is not p/t/n with positive indices, and the engine "
           "can't read it");
    }

    SECTION("the rest of the grammar")
    {
      const auto str = std::string{
        "# a comment\r\n"
        "mtllib ignored.mtl\r\n"
        "o object\r\n"
        "g group\r\n"
        "s off\r\n"
        "\t v 0 0 0 1\r\n"
        "v +16 0 0\r\n"
        "v 0 16 0 \r\n"
        "vt 0.5\r\n"
        "vn 0 0 1\r\n"
        "l 1 2\r\n"
        "usemtl   textures/a b  \r\n"
        "f 1 2 3\r\n"
        "usemtl\r\n"
        "f 1//1 2//1 3//1\r\n"
        "usemtl Textures/A B.tga\r\n"
        "f 1/1/1 2/1/1 3/1/1"};

      const auto result = parseIdTech4Obj(str);
      REQUIRE(result.is_success());

      // the whole rest of a usemtl line is the name; an empty usemtl clears the material
      CHECK(
        result.value().materialNames
        == std::vector<std::filesystem::path>{"textures/a b"});
      CHECK(
        result.value().triangles
        == std::vector<IdTech4ObjTriangle>{
          // a corner without a texture coordinate gets (0, 0)
          makeTriangle(0, {0, 16, 0}, {0, 0}, {16, 0, 0}, {0, 0}, {0, 0, 0}, {0, 0}),
          makeTriangle(
            std::nullopt, {0, 16, 0}, {0, 0}, {16, 0, 0}, {0, 0}, {0, 0, 0}, {0, 0}),
          // vt u alone has v = 0, which is flipped to 1
          makeTriangle(
            0, {0, 16, 0}, {0.5f, 1}, {16, 0, 0}, {0.5f, 1}, {0, 0, 0}, {0.5f, 1}),
        });

      // all of it is accepted, though the engine would fail on the weight of the vertex
      CHECK(
        result.value().engineIncompatibility
        == "At line 6: a vertex has more than three numbers, and the engine fails on the "
           "fourth");
    }

    SECTION("errors")
    {
      const auto error = [](const std::string& msg) {
        return Result<IdTech4ObjData>{Error{msg}};
      };

      const auto triangle = std::string{"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"};

      CHECK(
        parseIdTech4Obj("")
        == error("Model has no vertices. (So no valid bounding box.)"));
      CHECK(
        parseIdTech4Obj("# nothing\n\n")
        == error("Model has no vertices. (So no valid bounding box.)"));
      CHECK(parseIdTech4Obj(triangle) == error("Model has no faces"));

      CHECK(
        parseIdTech4Obj(triangle + "foo bar\n")
        == error("At line 6: Unknown statement 'foo'"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1 2 4\n")
        == error(
          "At line 6: Vertex index 4 is out of range, the file has 3 vertices so far"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1 2 -4\n")
        == error(
          "At line 6: Vertex index -4 is out of range, the file has 3 vertices so far"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1 2 0\n")
        == error(
          "At line 6: Vertex index 0 is out of range, the file has 3 vertices so far"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1/1 2/1 3/2\n")
        == error("At line 6: Texture coordinate index 2 is out of range, the file has 1 "
                 "texture coordinates so far"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1//1 2//1 3//2\n")
        == error(
          "At line 6: Normal index 2 is out of range, the file has 1 normals so far"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1 2\n")
        == error("At line 6: A face needs at least three corners, but this one has 2"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1/ 2/ 3/\n")
        == error("At line 6: Malformed face corner '1/'"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1/1/ 2/1/ 3/1/\n")
        == error("At line 6: Malformed face corner '1/1/'"));
      CHECK(
        parseIdTech4Obj(triangle + "f 1 2 x\n")
        == error("At line 6: Malformed face corner 'x'"));
      CHECK(
        parseIdTech4Obj("v 0 0\n")
        == error("At line 1: Expected a vertex coordinate, but got ''"));
      CHECK(
        parseIdTech4Obj("v 0 0 1x\n")
        == error("At line 1: Expected a vertex coordinate, but got '1x'"));
      CHECK(
        parseIdTech4Obj("vt\n")
        == error("At line 1: Expected a texture coordinate, but got ''"));

      // std::from_chars reads these, but they are no numbers to the engine, and a
      // position that is not finite has no bounds
      CHECK(
        parseIdTech4Obj("v 0 nan 0\n")
        == error("At line 1: Expected a vertex coordinate, but got 'nan'"));
      CHECK(
        parseIdTech4Obj("v 0 0 -inf\n")
        == error("At line 1: Expected a vertex coordinate, but got '-inf'"));
      CHECK(
        parseIdTech4Obj("vt inf 0\n")
        == error("At line 1: Expected a texture coordinate, but got 'inf'"));
      CHECK(
        parseIdTech4Obj("vt 0 NaN\n")
        == error("At line 1: Expected a texture coordinate, but got 'NaN'"));
    }

    SECTION("lines that the engine's importer reads differently are noted")
    {
      const auto triangle = std::string{"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"};
      const auto face = std::string{"f 1/1/1 2/1/1 3/1/1\n"};

      using T = std::tuple<std::string, std::optional<std::string>>;

      // clang-format off
      const auto
      [lines,                                       expected] = GENERATE_COPY(values<T>({
      {face,                                        std::nullopt},
      {"v 0 0 0 # a comment\n" + face,              std::nullopt},
      {"v 0 0 0 1\n" + face,
       "At line 6: a vertex has more than three numbers, and the engine fails on the "
       "fourth"},
      {"vt 0.5\n" + face,
       "At line 6: a texture coordinate doesn't have two numbers, and the engine fails "
       "on it"},
      {"vt 0 0 0\n" + face,
       "At line 6: a texture coordinate doesn't have two numbers, and the engine fails "
       "on it"},
      {"f 1/1 2/1 3/1\n",
       "At line 6: a face corner is not p/t/n with positive indices, and the engine "
       "can't read it"},
      {"f 1//1 2//1 3//1\n",
       "At line 6: a face corner is not p/t/n with positive indices, and the engine "
       "can't read it"},
      {"f -3/-1/-1 -2/-1/-1 -1/-1/-1\n",
       "At line 6: a face corner is not p/t/n with positive indices, and the engine "
       "can't read it"},
      {"f 1/1/1 2/1/1 3/1/1 1/1/1\n",
       "At line 6: a face has 4 corners, and the engine reads only the first three"},
      // only the first such line
      {face + "f 1 2 3 1\n" + "v 0 0 0 1\n",
       "At line 7: a face corner is not p/t/n with positive indices, and the engine "
       "can't read it"},
      }));
      // clang-format on

      CAPTURE(lines);

      const auto result = parseIdTech4Obj(triangle + lines);
      REQUIRE(result.is_success());
      CHECK(result.value().engineIncompatibility == expected);
    }
  }

  SECTION("canLoadIdTech4ObjModel")
  {
    CHECK(canLoadIdTech4ObjModel("model.obj"));
    CHECK(canLoadIdTech4ObjModel("_tb/models/a/MODEL.OBJ"));
    CHECK(canLoadIdTech4ObjModel("model.Obj"));
    CHECK_FALSE(canLoadIdTech4ObjModel("model.ase"));
    CHECK_FALSE(canLoadIdTech4ObjModel("model.objx"));
    CHECK_FALSE(canLoadIdTech4ObjModel("obj"));
  }

  SECTION("loadIdTech4ObjModel")
  {
    auto logger = CollectingLogger{};

    const auto loadDefaultMaterial = [&](std::string name) {
      return mdl::loadDefaultMaterial(fs, std::move(name), logger);
    };

    // stands in for the material decls: only the wedge's own material exists
    const auto findMaterial =
      [&](const std::filesystem::path& name) -> std::optional<gl::Material> {
      if (name == "models/mapobjects/test/wedge")
      {
        return mdl::loadDefaultMaterial(fs, "wedge decl", logger);
      }
      return std::nullopt;
    };

    auto result = loadIdTech4ObjModel(
      "wedge.obj",
      readFile(fs, modelsPath / "wedge.obj"),
      findMaterial,
      loadDefaultMaterial,
      logger);
    REQUIRE(result.is_success());

    const auto& modelData = result.value();
    CHECK(modelData.pitchType() == PitchType::Normal);
    CHECK(modelData.orientation() == Orientation::Oriented);
    REQUIRE(modelData.frameCount() == 1u);
    REQUIRE(modelData.surfaceCount() == 1u);

    // the positions are the engine's: assimp would give {-8,0,0}-{16,64,32}
    CHECK(modelData.bounds(0) == vm::bbox3f{{0, 0, -8}, {64, 32, 16}});

    const auto& surface = modelData.surface(0);
    REQUIRE(surface.skinCount() == 3u);
    CHECK(surface.skin(0)->name() == "wedge decl");
    // an unresolved name keeps its name and gets the default texture
    CHECK(surface.skin(1)->name() == "textures/base_wall/lfwall27d");
    CHECK(surface.skin(1)->texture()->width() == 32u);
    CHECK(surface.skin(2)->name() == "__TB_empty");

    REQUIRE(logger.warnings.size() == 1u);
    CHECK_THAT(
      logger.warnings.front(),
      Catch::Matchers::ContainsSubstring("textures/base_wall/lfwall27d")
        && Catch::Matchers::ContainsSubstring("wedge.obj"));

    // the mesh is there for picking: straight down at (8, 4) the slope of the first
    // triangle is at z = 2 and the second triangle at z = -8
    const auto* frame = modelData.frame(0);
    REQUIRE(frame != nullptr);
    const auto hit = frame->intersect(vm::ray3f{{8, 4, 100}, {0, 0, -1}});
    REQUIRE(hit);
    CHECK(*hit == vm::approx{98.0f});

    SECTION("the shared Doom 3 BFG fixture proxy")
    {
      auto gameFs = fs::DiskFileSystem{getFixtureRoot() / "test/mdl/Game/Doom3BFG/base"};

      auto arrowResult = loadIdTech4ObjModel(
        "arrow.obj",
        readFile(gameFs, "_tb/models/tb_test/arrow.obj"),
        findMaterial,
        loadDefaultMaterial,
        logger);
      REQUIRE(arrowResult.is_success());

      CHECK(arrowResult.value().bounds(0) == vm::bbox3f{{-32, -8, -8}, {32, 12, 20}});
      CHECK(arrowResult.value().surface(0).skinCount() == 3u);
    }

    SECTION("a line that the engine reads differently is reported")
    {
      logger.warnings.clear();

      // the engine would show one triangle of the quad
      const auto quad = std::string{
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
        "f 1/1/1 2/1/1 3/1/1 4/1/1\n"};
      REQUIRE(
        loadIdTech4ObjModel("quad.obj", quad, findMaterial, loadDefaultMaterial, logger)
          .is_success());

      CHECK(
        logger.warnings
        == std::vector<std::string>{
          "Model 'quad.obj' may not load in RBDOOM-3-BFG as it is shown here. At line 7: "
          "a face has 4 corners, and the engine reads only the first three"});
    }

    SECTION("a parse error is returned")
    {
      CHECK(
        loadIdTech4ObjModel("bad.obj", "foo\n", findMaterial, loadDefaultMaterial, logger)
          .is_error());
    }
  }

  SECTION("usemtl names resolved as EntityModelManager resolves them")
  {
    auto logger = CollectingLogger{};
    const auto& materialConfig = Doom3BFGGameInfo.gameConfig.materialConfig;

    const auto createResource = [](auto resourceLoader) {
      return gl::createResourceSync(std::move(resourceLoader));
    };

    // a record as the .mtr parser makes it, outside of the material root
    const auto wedgeShader = Quake3Shader{
      .shaderPath = "models/mapobjects/test/wedge",
      .editorImage = "models/mapobjects/test/wedge_ed.tga",
      .format = Quake3Shader::Format::Doom3,
      .previewImages = {"models/mapobjects/test/wedge_ed.tga"},
    };

    const auto load =
      [&](const std::vector<Quake3Shader>& shaders, const std::filesystem::path& path) {
        const auto findMaterial = [&](const std::filesystem::path& materialName) {
          return findIdTech4Material(
                   fs, materialConfig, materialName, createResource, shaders)
                 | kdl::value();
        };
        const auto loadDefaultMaterial = [&](std::string name) {
          return mdl::loadDefaultMaterial(fs, std::move(name), logger);
        };

        return loadIdTech4ObjModel(
                 path.filename().string(),
                 readFile(fs, path),
                 findMaterial,
                 loadDefaultMaterial,
                 logger)
               | kdl::value();
      };

    SECTION("a material decl, an implicit material and the default skin")
    {
      const auto modelData = load({wedgeShader}, modelsPath / "wedge.obj");

      const auto& surface = modelData.surface(0);
      REQUIRE(surface.skinCount() == 3u);

      // the decl, previewed with its 4x2 editor image
      CHECK(surface.skin(0)->relativePath() == "models/mapobjects/test/wedge");
      REQUIRE(surface.skin(0)->texture());
      CHECK(surface.skin(0)->texture()->width() == 4u);
      CHECK(surface.skin(0)->texture()->height() == 2u);

      // no decl, but the 2x2 image textures/base_wall/lfwall27d.tga
      CHECK(surface.skin(1)->relativePath() == "textures/base_wall/lfwall27d");
      REQUIRE(surface.skin(1)->texture());
      CHECK(surface.skin(1)->texture()->width() == 2u);
      CHECK(surface.skin(1)->texture()->height() == 2u);

      CHECK(surface.skin(2)->name() == "__TB_empty");
      CHECK(logger.warnings.empty());
    }

    SECTION("a name that resolves to nothing keeps its name on the default texture")
    {
      const auto modelData = load({}, modelsPath / "wedge.obj");

      const auto& surface = modelData.surface(0);
      REQUIRE(surface.skinCount() == 3u);
      CHECK(surface.skin(0)->name() == "models/mapobjects/test/wedge");
      REQUIRE(surface.skin(0)->texture());
      CHECK(surface.skin(0)->texture()->width() == 32u);
      CHECK(surface.skin(0)->texture()->height() == 32u);

      REQUIRE(logger.warnings.size() == 1u);
      CHECK_THAT(
        logger.warnings.front(),
        Catch::Matchers::ContainsSubstring("models/mapobjects/test/wedge")
          && !Catch::Matchers::ContainsSubstring("lfwall27d"));
    }

    SECTION("a non-canonical usemtl name finds the material decl")
    {
      const auto modelData = load({wedgeShader}, modelsPath / "noncanonical.obj");

      const auto& surface = modelData.surface(0);
      REQUIRE(surface.skinCount() == 2u);
      CHECK(surface.skin(0)->relativePath() == "models/mapobjects/test/wedge");
      REQUIRE(surface.skin(0)->texture());
      CHECK(surface.skin(0)->texture()->width() == 4u);
    }
  }
}

} // namespace tb::mdl
