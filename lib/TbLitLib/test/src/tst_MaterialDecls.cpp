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
#include "lit/MaterialDecls.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

// The engine's enum values, as renderer/Image.h, Material.h and GLState.h define them at
// the pinned commit. The expected values below are derived from the copied engine code.
namespace usage
{
constexpr int Specular = 0;
constexpr int Diffuse = 1;
constexpr int Default = 2;
constexpr int Bump = 3;
constexpr int Light = 5;
constexpr int Coverage = 10;
constexpr int SpecularPbrRmao = 12;
} // namespace usage

constexpr int FilterNearest = 1;
constexpr int FilterDefault = 3;
constexpr int RepeatRepeat = 0;
constexpr int RepeatClamp = 1;
constexpr int RepeatClampToZero = 2;
constexpr int CubeNative = 1;

constexpr int LightingAmbient = 0;
constexpr int LightingBump = 1;
constexpr int LightingDiffuse = 2;
constexpr int LightingSpecular = 3;
constexpr int LightingCoverage = 4;

constexpr int CoverageOpaque = 1;
constexpr int CoveragePerforated = 2;
constexpr int CoverageTranslucent = 3;

constexpr float SortOpaque = 0.0f;
constexpr float SortDecal = 2.0f;
constexpr float SortMedium = 4.0f;
constexpr float SortNearest = 7.0f;

constexpr int CullTwoSided = 2;
constexpr int DeformFlare = 3;
constexpr int DeformParticle = 7;
constexpr int TexgenSkybox = 4;
constexpr int VertexColorModulate = 1;

constexpr int MfPolygonOffset = 1 << 1;
constexpr int MfNoShadows = 1 << 2;
constexpr int SurfDiscrete = 1 << 10;
constexpr int SurfNoFragment = 1 << 11;
constexpr int ContentsSolid = 1 << 0;
constexpr int ContentsOpaque = 1 << 1;

constexpr std::uint64_t BlendSrcDstColor = 2;
constexpr std::uint64_t BlendSrcSrcAlpha = 4;
constexpr std::uint64_t BlendDstOne = 1 << 3;
constexpr std::uint64_t BlendDstOneMinusSrcAlpha = 5 << 3;
constexpr std::uint64_t DepthMask = 1 << 6;
constexpr std::uint64_t DepthFuncEqual = 3 << 13;

// what Parse adds to every stage of an opaque or perforated material, and of a
// translucent one
constexpr std::uint64_t OpaqueDepth = DepthFuncEqual | DepthMask;
constexpr std::uint64_t TranslucentDepth = DepthMask;

std::string readText(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto text = std::stringstream{};
  text << stream.rdbuf();
  return text.str();
}

// every file of the MaterialDecls fixture, with paths relative to the fixture folder
std::vector<DeclFile> fixtureFiles()
{
  const auto root = getFixtureRoot() / "test" / "lit" / "MaterialDecls";
  auto files = std::vector<DeclFile>{};
  for (const auto& entry : std::filesystem::recursive_directory_iterator{root})
  {
    if (entry.is_regular_file())
    {
      files.push_back(DeclFile{
        std::filesystem::relative(entry.path(), root).generic_string(),
        readText(entry.path()),
        0});
    }
  }
  return files;
}

MaterialImageKey key(
  std::string name,
  const int usage,
  const int filter = FilterDefault,
  const int repeat = RepeatRepeat,
  const int cube = 0)
{
  auto result = MaterialImageKey{};
  result.name = std::move(name);
  result.usage = usage;
  result.filter = filter;
  result.repeat = repeat;
  result.cube = cube;
  return result;
}

std::shared_ptr<const MaterialEntry> lookup(MaterialDecls& decls, const std::string& name)
{
  auto result = decls.lookup(name);
  INFO(result.error);
  REQUIRE(result);
  return *result.value;
}

std::vector<float> evaluate(
  MaterialDecls& decls,
  const MaterialEntry& material,
  const std::array<float, MaterialDecls::ShaderParmCount>& shaderParms,
  const float time)
{
  auto result = decls.evaluate(material, shaderParms, time);
  INFO(result.error);
  REQUIRE(result);
  return result.value->registers;
}

std::vector<std::string> messagesOf(
  const MaterialDecls& decls, const DeclMessageKind kind)
{
  auto texts = std::vector<std::string>{};
  for (const auto& message : decls.messages())
  {
    if (message.kind == kind)
    {
      texts.push_back(message.text);
    }
  }
  return texts;
}

bool hasMessage(
  const MaterialDecls& decls, const DeclMessageKind kind, const std::string& text)
{
  const auto texts = messagesOf(decls, kind);
  return std::find(texts.begin(), texts.end(), text) != texts.end();
}

constexpr auto NoParms = std::array<float, MaterialDecls::ShaderParmCount>{};

} // namespace

TEST_CASE("MaterialDecls")
{
  auto source = MemoryDeclSource{fixtureFiles()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  SECTION("construction")
  {
    SECTION("reads every decl folder, and only the duplicate warns")
    {
      // 18 materials, 4 tables in materials/ and 1 in def/, 3 entityDefs
      CHECK(decls.materialCount() == 18);
      CHECK(decls.tableCount() == 5);
      CHECK(decls.entityDefCount() == 3);

      // a type the registry did not know would be read as a decl name of the folder's
      // default type, and its next token would warn "Expecting '{'"
      CHECK(
        messagesOf(decls, DeclMessageKind::Warning)
        == std::vector<std::string>{
          "file materials/zz_lp1a_dup.mtr, line 6: material 'textures/lp1a/wall' "
          "previously defined at materials/lp1a.mtr:7"});
      CHECK(messagesOf(decls, DeclMessageKind::Error).empty());
      CHECK(messagesOf(decls, DeclMessageKind::FatalError).empty());
    }

    SECTION("keeps the decl text")
    {
      CHECK(decls.declTextBytes() > 0);
    }

    SECTION("allows one registry at a time")
    {
      auto other = MaterialDecls{source};
      CHECK(other.constructionError() == "another material registry exists");
      CHECK_FALSE(other.lookup("textures/lp1a/wall"));
      CHECK(lookup(decls, "textures/lp1a/wall")->name == "textures/lp1a/wall");
    }
  }

  SECTION("lookup")
  {
    SECTION("diffuse, bump and specular stages in the engine's order")
    {
      const auto wall = lookup(decls, "textures/lp1a/wall");
      CHECK(wall->sourceFile == "materials/lp1a.mtr");
      // the line where the previous decl ended (LoadAndParse takes it before the type)
      CHECK(wall->sourceLine == 7);
      CHECK_FALSE(wall->implicit);
      CHECK_FALSE(wall->defaulted);
      CHECK(wall->coverage == CoverageOpaque);
      CHECK(wall->sort == SortOpaque);
      CHECK(wall->contentFlags == (ContentsSolid | ContentsOpaque));
      CHECK(wall->materialFlags == 0);
      CHECK(wall->receivesLighting);
      CHECK(wall->surfaceCastsShadow);
      CHECK(wall->constantRegisters);

      REQUIRE(wall->stages.size() == 3);
      CHECK(wall->stages[0].lighting == LightingBump);
      CHECK(wall->stages[0].image == key("textures/lp1a/wall_local", usage::Bump));
      CHECK(wall->stages[1].lighting == LightingDiffuse);
      CHECK(wall->stages[1].image == key("textures/lp1a/wall_d", usage::Diffuse));
      CHECK(wall->stages[2].lighting == LightingSpecular);
      CHECK(wall->stages[2].image == key("textures/lp1a/wall_s", usage::Specular));
      for (const auto& stage : wall->stages)
      {
        CHECK(stage.drawStateBits == OpaqueDepth);
      }

      CHECK(wall->fastPathBumpImage == key("textures/lp1a/wall_local", usage::Bump));
      CHECK(wall->fastPathDiffuseImage == key("textures/lp1a/wall_d", usage::Diffuse));
      CHECK(wall->fastPathSpecularImage == key("textures/lp1a/wall_s", usage::Specular));
    }

    SECTION("_rmao usage and image-program names")
    {
      const auto pbr = lookup(decls, "textures/lp1a/pbr");
      REQUIRE(pbr->stages.size() == 3);
      CHECK(
        pbr->stages[0].image
        == key(
          "addnormals( textures/lp1a/pbr_local, heightmap( textures/lp1a/pbr_h, 4))",
          usage::Bump));
      CHECK(pbr->stages[1].image == key("textures/lp1a/pbr_d", usage::Diffuse));
      CHECK(pbr->stages[2].lighting == LightingSpecular);
      CHECK(
        pbr->stages[2].image == key("textures/lp1a/pbr_rmao", usage::SpecularPbrRmao));
    }

    SECTION("implicit _flat bump stage and the black fast-path specular")
    {
      const auto diffuseOnly = lookup(decls, "textures/lp1a/diffuseOnly");
      REQUIRE(diffuseOnly->stages.size() == 2);
      CHECK(diffuseOnly->stages[0].lighting == LightingBump);
      REQUIRE(diffuseOnly->stages[0].image);
      CHECK(diffuseOnly->stages[0].image->name == "_flat");
      CHECK(diffuseOnly->stages[0].image->intrinsic);
      CHECK(diffuseOnly->stages[1].image == key("textures/lp1a/plain", usage::Diffuse));

      REQUIRE(diffuseOnly->fastPathSpecularImage);
      CHECK(diffuseOnly->fastPathSpecularImage->name == "_black");
      CHECK(diffuseOnly->fastPathSpecularImage->intrinsic);
    }

    SECTION("an alpha-tested diffuse stage gets a coverage stage")
    {
      const auto grate = lookup(decls, "textures/lp1a/grate");
      CHECK(grate->coverage == CoveragePerforated);
      REQUIRE(grate->stages.size() == 3);
      CHECK(grate->stages[0].lighting == LightingBump);
      CHECK(grate->stages[1].lighting == LightingDiffuse);
      CHECK(grate->stages[1].image == key("textures/lp1a/grate_d", usage::Diffuse));
      CHECK_FALSE(grate->stages[1].hasAlphaTest);
      CHECK(grate->stages[2].lighting == LightingCoverage);
      CHECK(grate->stages[2].image == key("textures/lp1a/grate_d", usage::Coverage));
      CHECK(grate->stages[2].hasAlphaTest);
      for (const auto& stage : grate->stages)
      {
        CHECK(stage.drawStateBits == OpaqueDepth);
      }
    }

    SECTION("expressions, tables, conditions and a texture matrix")
    {
      const auto expr = lookup(decls, "textures/lp1a/expr");
      CHECK(expr->coverage == CoverageTranslucent);
      CHECK(expr->sort == SortMedium);
      CHECK((expr->materialFlags & MfNoShadows) != 0);
      CHECK_FALSE(expr->constantRegisters);
      REQUIRE(expr->stages.size() == 1);

      const auto& stage = expr->stages[0];
      CHECK(stage.lighting == LightingAmbient);
      CHECK(stage.drawStateBits == (BlendDstOne | TranslucentDepth));
      CHECK(stage.image == key("textures/lp1a/glow", usage::Default));
      CHECK(stage.hasTextureMatrix);
    }

    SECTION("alpha test and constant registers")
    {
      const auto constant = lookup(decls, "textures/lp1a/constant");
      CHECK(constant->constantRegisters);
      CHECK(constant->coverage == CoveragePerforated);
      CHECK(constant->sort == SortOpaque);
      REQUIRE(constant->stages.size() == 1);
      CHECK(constant->stages[0].drawStateBits == (BlendSrcDstColor | OpaqueDepth));
      CHECK(constant->stages[0].hasAlphaTest);
      // an alpha of 0.75 is not an identity colour, so there is no fast path
      CHECK_FALSE(constant->fastPathDiffuseImage);
    }

    SECTION("a program stage")
    {
      const auto program = lookup(decls, "textures/lp1a/program");
      REQUIRE(program->stages.size() == 1);
      const auto& stage = program->stages[0];
      CHECK(stage.newStage);
      CHECK(stage.vertexProgram == "lp1aEffect");
      CHECK(stage.fragmentProgram == "lp1aEffect");
      CHECK_FALSE(stage.image);
      REQUIRE(stage.fragmentImages.size() == 2);
      CHECK(stage.fragmentImages[0] == key("textures/lp1a/prog_map", usage::Default));
      CHECK(stage.fragmentImages[1] == key("textures/lp1a/prog_local", usage::Bump));
      CHECK(stage.vertexParms.size() == 1);
      CHECK(program->coverage == CoverageOpaque);
      CHECK(stage.drawStateBits == OpaqueDepth);
    }

    SECTION("videomap and soundmap stages")
    {
      const auto screen = lookup(decls, "textures/lp1a/screen");
      REQUIRE(screen->stages.size() == 1);
      CHECK(screen->stages[0].cinematic == "video/lp1a_intro.bik");
      CHECK_FALSE(screen->stages[0].soundMap);
      CHECK_FALSE(screen->stages[0].image);

      const auto soundviz = lookup(decls, "textures/lp1a/soundviz");
      REQUIRE(soundviz->stages.size() == 1);
      CHECK(soundviz->stages[0].cinematic == "waveform");
      CHECK(soundviz->stages[0].soundMap);

      // no stage without an image warned: cinematics need none
      CHECK(messagesOf(decls, DeclMessageKind::Warning).size() == 1);
    }

    SECTION("a gui surface")
    {
      const auto gui = lookup(decls, "textures/lp1a/guiscreen");
      CHECK(gui->gui == "guis/lp1a/screen.gui");
    }

    SECTION("deforms, sort and coverage keywords")
    {
      const auto flare = lookup(decls, "textures/lp1a/flare");
      CHECK(flare->deform == DeformFlare);
      CHECK(flare->sort == SortNearest);
      CHECK(flare->coverage == CoverageTranslucent);
      CHECK((flare->materialFlags & MfNoShadows) != 0);

      const auto smoke = lookup(decls, "textures/lp1a/smoke");
      CHECK(smoke->deform == DeformParticle);
      CHECK(smoke->deformDecl == "lp1a_smoke");

      const auto decal = lookup(decls, "textures/lp1a/decal");
      CHECK(decal->sort == SortDecal);
      CHECK(decal->polygonOffset == 1.0f);
      CHECK(decal->materialFlags == (MfPolygonOffset | MfNoShadows));
      CHECK((decal->surfaceFlags & SurfDiscrete) != 0);
      CHECK(decal->contentFlags == 0);
      // an unlit stage keeps the two-sided cull
      CHECK(decal->cullType == CullTwoSided);
      REQUIRE(decal->stages.size() == 1);
      CHECK(decal->stages[0].vertexColor == VertexColorModulate);
      CHECK(
        decal->stages[0].drawStateBits
        == (BlendSrcSrcAlpha | BlendDstOneMinusSrcAlpha | TranslucentDepth));
      CHECK(decal->stages[0].image == key("textures/lp1a/decal_d", usage::Default));
    }

    SECTION("explicit blend modes, cube maps and texgen")
    {
      const auto sky = lookup(decls, "textures/lp1a/sky");
      CHECK((sky->surfaceFlags & SurfNoFragment) != 0);
      REQUIRE(sky->stages.size() == 1);
      CHECK(sky->stages[0].drawStateBits == (BlendDstOne | TranslucentDepth));
      CHECK(sky->stages[0].texgen == TexgenSkybox);
      CHECK(
        sky->stages[0].image
        == key("env/lp1a_sky", usage::Default, FilterNearest, RepeatClamp, CubeNative));
    }

    SECTION("a light material")
    {
      const auto light = lookup(decls, "lights/lp1a_round");
      CHECK(
        light->lightFalloffImage
        == key(
          "makeIntensity( lights/lp1a_falloff)",
          usage::Default,
          FilterDefault,
          RepeatClamp));
      REQUIRE(light->stages.size() == 1);
      // a "lights" prefix makes the usage TD_LIGHT
      CHECK(
        light->stages[0].image
        == key("lights/lp1a_round", usage::Light, FilterDefault, RepeatClampToZero));
      CHECK(light->stages[0].colorRegisters == std::array<int, 4>{1, 2, 3, 4});
    }

    SECTION("a name without a decl gets the implicit material")
    {
      const auto implicit = lookup(decls, "textures/lp1a/nodecl");
      CHECK(implicit->implicit);
      CHECK_FALSE(implicit->defaulted);
      CHECK(implicit->sourceFile == "<implicit file>");
      CHECK(implicit->coverage == CoverageTranslucent);
      CHECK(implicit->sort == SortMedium);
      REQUIRE(implicit->stages.size() == 1);
      const auto& stage = implicit->stages[0];
      CHECK(stage.lighting == LightingAmbient);
      CHECK(
        stage.drawStateBits
        == (BlendSrcSrcAlpha | BlendDstOneMinusSrcAlpha | TranslucentDepth));
      CHECK(stage.colorRegisters == std::array<int, 4>{1, 2, 3, 4});
      CHECK(
        stage.image
        == key("textures/lp1a/nodecl", usage::Default, FilterDefault, RepeatClamp));
      CHECK(decls.materialCount() == 19);
    }

    SECTION("an implicit material beside a same-named image record")
    {
      // the wall's diffuse stage creates the record "textures/lp1a/wall_d" (diffuse,
      // repeat); the implicit material of that name asks for default usage and clamp, so
      // it gets a record of its own
      lookup(decls, "textures/lp1a/wall");
      const auto implicit = lookup(decls, "textures/lp1a/wall_d");
      CHECK(implicit->implicit);
      REQUIRE(implicit->stages.size() == 1);
      CHECK(
        implicit->stages[0].image
        == key("textures/lp1a/wall_d", usage::Default, FilterDefault, RepeatClamp));

      // an intrinsic name matches whatever the parameters
      const auto flat = lookup(decls, "_flat");
      REQUIRE(flat->stages.size() == 1);
      REQUIRE(flat->stages[0].image);
      CHECK(flat->stages[0].image->name == "_flat");
      CHECK(flat->stages[0].image->intrinsic);

      // the image program rewrites _white to the YCoCg-safe file
      const auto white = lookup(decls, "_white");
      REQUIRE(white->stages.size() == 1);
      CHECK(
        white->stages[0].image
        == key("guis/assets/white", usage::Default, FilterDefault, RepeatClamp));
    }

    SECTION("names are canonicalised as the engine does")
    {
      const auto wall = lookup(decls, "textures/lp1a/wall");
      const auto upper = lookup(decls, "Textures\\LP1A\\Wall.tga");
      CHECK(upper == wall);
      CHECK_FALSE(upper->implicit);
    }

    SECTION("a malformed decl is replaced by the default material")
    {
      const auto broken = lookup(decls, "textures/lp1a/broken");
      CHECK(broken->defaulted);
      CHECK_FALSE(broken->implicit);
      CHECK(hasMessage(
        decls,
        DeclMessageKind::Warning,
        "unknown general material parameter 'bogusKeyword' in 'textures/lp1a/broken'"));
      REQUIRE(broken->stages.size() == 1);
      REQUIRE(broken->stages[0].image);
      CHECK(broken->stages[0].image->name == "_default");
      CHECK(broken->stages[0].image->isDefault);
      CHECK(
        broken->stages[0].drawStateBits
        == (BlendSrcSrcAlpha | BlendDstOneMinusSrcAlpha | TranslucentDepth));
    }

    SECTION("a name too long for the engine's buffer")
    {
      CHECK_FALSE(decls.lookup(std::string(1024, 'a')));
      CHECK(decls.lookup(std::string(1023, 'a')));
    }
  }

  SECTION("evaluate")
  {
    SECTION("tables, time and parms")
    {
      const auto expr = lookup(decls, "textures/lp1a/expr");
      const auto& stage = expr->stages[0];

      auto parms = NoParms;
      parms[0] = 0.5f;
      parms[1] = 1.0f;
      const auto registers = evaluate(decls, *expr, parms, 0.25f);
      // lp1aRamp is clamped over { 0, 10, 20 }: 0.5 lands on 10
      CHECK(registers[size_t(stage.colorRegisters[0])] == 10.0f);
      CHECK(registers[size_t(stage.colorRegisters[2])] == 10.0f);
      // 0.5 * sinTable[ 0.25 ] = 0.5 * 1
      CHECK(registers[size_t(stage.colorRegisters[3])] == 0.5f);
      CHECK(registers[size_t(stage.conditionRegister)] == 1.0f);
      const auto& matrix = stage.textureMatrixRegisters;
      CHECK(registers[size_t(matrix[0][0])] == 1.0f);
      CHECK(registers[size_t(matrix[0][1])] == 0.0f);
      CHECK(registers[size_t(matrix[0][2])] == Catch::Approx(0.025f));
      CHECK(registers[size_t(matrix[1][1])] == 1.0f);
      CHECK(registers[size_t(matrix[1][2])] == 0.0f);

      parms[0] = 0.75f;
      parms[1] = 0.0f;
      const auto other = evaluate(decls, *expr, parms, 0.125f);
      // halfway between 10 and 20, and halfway between sinTable's 0 and 1
      CHECK(other[size_t(stage.colorRegisters[0])] == 15.0f);
      CHECK(other[size_t(stage.colorRegisters[3])] == 0.25f);
      CHECK(other[size_t(stage.conditionRegister)] == 0.0f);
    }

    SECTION("a snapped table and a table from a .def file")
    {
      const auto snap = lookup(decls, "textures/lp1a/snap");
      const auto& stage = snap->stages[0];
      auto parms = NoParms;
      parms[0] = 0.75f;
      const auto high = evaluate(decls, *snap, parms, 0.0f);
      CHECK(high[size_t(stage.colorRegisters[1])] == 1.0f);
      // lp1aDefTable { 2, 4 } at 0.25: halfway between 2 and 4
      CHECK(high[size_t(stage.colorRegisters[0])] == 3.0f);

      parms[0] = 0.25f;
      const auto low = evaluate(decls, *snap, parms, 0.0f);
      CHECK(low[size_t(stage.colorRegisters[1])] == 0.0f);
    }

    SECTION("rotate through sinTable and cosTable")
    {
      const auto rotate = lookup(decls, "textures/lp1a/rotate");
      const auto& matrix = rotate->stages[0].textureMatrixRegisters;
      const auto registers = evaluate(decls, *rotate, NoParms, 0.25f);
      CHECK(registers[size_t(matrix[0][0])] == 0.0f);
      CHECK(registers[size_t(matrix[0][1])] == -1.0f);
      CHECK(registers[size_t(matrix[0][2])] == 1.0f);
      CHECK(registers[size_t(matrix[1][0])] == 1.0f);
      CHECK(registers[size_t(matrix[1][1])] == 0.0f);
      CHECK(registers[size_t(matrix[1][2])] == 0.0f);
    }

    SECTION("constants and alpha test")
    {
      const auto constant = lookup(decls, "textures/lp1a/constant");
      const auto& stage = constant->stages[0];
      const auto registers = evaluate(decls, *constant, NoParms, 0.0f);
      CHECK(registers[size_t(stage.colorRegisters[3])] == 0.75f);
      CHECK(registers[size_t(stage.alphaTestRegister)] == 0.5f);
    }

    SECTION("vertex parms, colored stages and deform registers")
    {
      const auto program = lookup(decls, "textures/lp1a/program");
      const auto& parm = program->stages[0].vertexParms[0];
      const auto programRegisters = evaluate(decls, *program, NoParms, 0.5f);
      CHECK(programRegisters[size_t(parm[0])] == 0.5f);
      CHECK(programRegisters[size_t(parm[1])] == 1.0f);
      CHECK(programRegisters[size_t(parm[2])] == 0.0f);
      CHECK(programRegisters[size_t(parm[3])] == 1.0f);

      const auto light = lookup(decls, "lights/lp1a_round");
      auto parms = NoParms;
      parms[0] = 0.25f;
      parms[1] = 0.5f;
      parms[2] = 0.75f;
      parms[3] = 1.0f;
      const auto lightRegisters = evaluate(decls, *light, parms, 0.0f);
      CHECK(lightRegisters[1] == 0.25f);
      CHECK(lightRegisters[2] == 0.5f);
      CHECK(lightRegisters[3] == 0.75f);
      CHECK(lightRegisters[4] == 1.0f);

      const auto flare = lookup(decls, "textures/lp1a/flare");
      const auto flareRegisters = evaluate(decls, *flare, NoParms, 0.0f);
      CHECK(flareRegisters[size_t(flare->deformRegisters[0])] == 16.0f);
    }

    SECTION("a material of another registry")
    {
      auto foreign = MaterialEntry{};
      foreign.name = "textures/lp1a/elsewhere";
      foreign.index = 1000;
      CHECK_FALSE(decls.evaluate(foreign, NoParms, 0.0f));

      foreign.index = lookup(decls, "textures/lp1a/wall")->index;
      CHECK_FALSE(decls.evaluate(foreign, NoParms, 0.0f));
    }
  }

  SECTION("entityDef")
  {
    SECTION("inherited keys are merged under the entityDef's own")
    {
      const auto def = decls.entityDef("lp1a_lamp");
      INFO(def.error);
      REQUIRE(def);
      const auto& entry = **def.value;
      CHECK(entry.name == "lp1a_lamp");
      CHECK_FALSE(entry.dmapOnly);
      CHECK(
        entry.keys
        == EntityKeys{
          {"noshadows", "1"},
          {"model", "models/lp1a/lamp.lwo"},
          {"classname", "lp1a_lamp"},
          {"spawnclass", "idStaticEntity"},
          {"_color", "1 0.5 0.25"},
        });
    }

    SECTION("an unknown parent warns")
    {
      const auto def = decls.entityDef("lp1a_orphan");
      REQUIRE(def);
      CHECK(hasMessage(
        decls,
        DeclMessageKind::Warning,
        "file def/lp1a.def, line 23: Unknown entityDef 'lp1a_nope' inherited by "
        "'lp1a_orphan'"));
    }

    SECTION("an unknown entityDef")
    {
      const auto def = decls.entityDef("lp1a_missing");
      CHECK_FALSE(def);
      CHECK(def.error == "no entityDef 'lp1a_missing'");
    }
  }

  SECTION("canonicalName")
  {
    CHECK(
      MaterialDecls::canonicalName("Textures\\Base\\Wall.TGA") == "textures/base/wall");
    CHECK(MaterialDecls::canonicalName("a.b.c") == "a.b");
    CHECK(MaterialDecls::canonicalName("plain") == "plain");
  }
}

TEST_CASE("MaterialDecls former fatal sites")
{
  SECTION("a listed file that cannot be read")
  {
    auto source = MemoryDeclSource{{
      DeclFile{"materials/a_missing.mtr", std::nullopt, 0},
      DeclFile{"materials/b_present.mtr", "textures/lp1a/present { { map _white } }", 0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    // the engine would quit here; the copied LoadAndParse returns 0 after it
    CHECK(
      messagesOf(decls, DeclMessageKind::FatalError)
      == std::vector<std::string>{"couldn't load materials/a_missing.mtr"});
    CHECK_FALSE(lookup(decls, "textures/lp1a/present")->implicit);
  }

  SECTION("an image referenced with conflicting cube map states")
  {
    auto source = MemoryDeclSource{{
      DeclFile{
        "materials/conflict.mtr",
        "textures/lp1a/flat2d { { blend add map env/conflict } }\n"
        "textures/lp1a/cube { { blend add cubeMap env/conflict } }\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    lookup(decls, "textures/lp1a/flat2d");

    // ImageManager.cpp's Error returns here, and the lookup reports what the engine
    // stopped at, every time
    const auto cube = decls.lookup("textures/lp1a/cube");
    REQUIRE_FALSE(cube);
    CHECK(
      cube.error
      == "Image 'env/conflict' has been referenced with conflicting cube map states");
    CHECK_FALSE(decls.lookup("textures/lp1a/cube"));

    // the registry stays usable
    CHECK(lookup(decls, "textures/lp1a/flat2d")->stages.size() == 1);
  }
}

TEST_CASE("MaterialDecls refuses an overlong name")
{
  auto source = MemoryDeclSource{fixtureFiles()};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  const auto result = decls.lookup(std::string(2000, 'x'));
  CHECK_FALSE(result);
  CHECK_FALSE(result.error.empty());
}

namespace
{

// a source that throws where the registry's file system calls it (G3)
class ThrowingSource : public DeclSource
{
private:
  MemoryDeclSource m_source;

public:
  explicit ThrowingSource(std::vector<DeclFile> files)
    : m_source{std::move(files)}
  {
  }

  std::vector<std::string> listFiles(
    const std::string& folder, const std::string& extension) override
  {
    if (folder == "skins")
    {
      throw std::runtime_error{"an injected listing failure"};
    }
    return m_source.listFiles(folder, extension);
  }

  std::optional<std::string> readFile(
    const std::string& path, std::int64_t* timestamp) override
  {
    if (path == "materials/b_throws.mtr")
    {
      throw std::runtime_error{"an injected read failure"};
    }
    return m_source.readFile(path, timestamp);
  }
};

std::string stagesOf(const int count, const std::string& stage)
{
  auto text = std::string{};
  for (int i = 0; i < count; ++i)
  {
    text += stage;
  }
  return text;
}

// count characters of words of nine A and a space: no dot anywhere
std::string spacedWords(const std::size_t count)
{
  auto text = std::string{};
  while (text.size() < count)
  {
    text += text.size() % 10 == 9 ? ' ' : 'A';
  }
  return text;
}

const auto PlainStage = std::string{"\t{ blend add map _white }\n"};

// M-ID-GUARD-06's warning, and the copied LoadAndParse's FatalError for the file it read
// as unreadable
void checkNotRead(const MaterialDecls& decls, const std::string& path)
{
  CAPTURE(path);
  CHECK(hasMessage(
    decls,
    DeclMessageKind::Warning,
    path
      + " holds a token of 1024 or more characters with no '.' among the first 1024, "
        "which the engine's MakeNameCanonical cannot take; it is not read"));
  CHECK(hasMessage(decls, DeclMessageKind::FatalError, "couldn't load " + path));
}

void checkRead(const MaterialDecls& decls, const std::string& path)
{
  CAPTURE(path);
  CHECK_FALSE(hasMessage(decls, DeclMessageKind::FatalError, "couldn't load " + path));
  for (const auto& message : decls.messages())
  {
    CHECK(message.text.find(path) == std::string::npos);
  }
}

// M-ID-GUARD-05's warning, and the refused lookup
void checkStagesRefused(MaterialDecls& decls, const std::string& name)
{
  CAPTURE(name);
  CHECK(hasMessage(
    decls, DeclMessageKind::Warning, "material '" + name + "' exceeded 256 stages"));
  const auto refused = decls.lookup(name);
  REQUIRE_FALSE(refused);
  CHECK_THAT(refused.error, Catch::Matchers::ContainsSubstring("exceeded 256 stages"));
}

} // namespace

TEST_CASE("MaterialDecls catches what its source throws")
{
  auto source = ThrowingSource{{
    DeclFile{"materials/a_good.mtr", "textures/lp1a/good { { map _white } }", 0},
    DeclFile{"materials/b_throws.mtr", "textures/lp1a/thrown { { map _white } }", 0},
    DeclFile{"skins/a.skin", "skins/lp1a { * textures/lp1a/good }", 0},
  }};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  const auto warnings = messagesOf(decls, DeclMessageKind::Warning);
  CHECK(
    std::count(
      warnings.begin(),
      warnings.end(),
      "The decl files of skins cannot be listed: an injected listing failure")
    == 1);
  CHECK(
    std::count(
      warnings.begin(),
      warnings.end(),
      "materials/b_throws.mtr cannot be read: an injected read failure")
    == 1);
  CHECK(hasMessage(
    decls, DeclMessageKind::FatalError, "couldn't load materials/b_throws.mtr"));
  CHECK_FALSE(lookup(decls, "textures/lp1a/good")->implicit);
  CHECK(lookup(decls, "textures/lp1a/thrown")->implicit);
}

TEST_CASE("MaterialDecls releases its claim when its constructor throws")
{
  // W.5: a throw after the claim (an allocation failure in the refuse loop; here the test
  // hook, once the registry is built) destroys the registry and releases the one-registry
  // claim, so that the next registry can be built
  auto source = MemoryDeclSource{fixtureFiles()};
  CHECK_THROWS_WITH(
    MaterialDecls(
      source, nullptr, [] { throw std::runtime_error{"an injected failure"}; }),
    "an injected failure");

  auto decls = MaterialDecls{source};
  CHECK(decls.constructionError().empty());
  CHECK_FALSE(lookup(decls, "textures/lp1a/wall")->implicit);
}

TEST_CASE("MaterialDecls guards the engine's overflows")
{
  SECTION("a material of more than 256 stages is refused before it is parsed")
  {
    // M-ID-GUARD-05: the copied ParseStage writes past parseStages from the 257th stage;
    // brace blocks and stage shortcuts both count
    auto source = MemoryDeclSource{{
      DeclFile{
        "materials/stages.mtr",
        "textures/lp1a/blocks\n{\n" + stagesOf(257, PlainStage) + "}\n"
          + "textures/lp1a/shortcuts\n{\n" + stagesOf(200, PlainStage)
          + stagesOf(57, "\tdiffusemap _white\n") + "}\n"
          + "textures/lp1a/limit\n{\n// { a comment is no stage }\n\tqer_editorimage "
            "\"{ nor a string }\"\n"
          + stagesOf(256, PlainStage) + "}\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    checkStagesRefused(decls, "textures/lp1a/blocks");
    checkStagesRefused(decls, "textures/lp1a/shortcuts");
    CHECK(decls.defaultedCount("material") == 2);

    // 256 stages are the engine's limit, and parse
    const auto limit = lookup(decls, "textures/lp1a/limit");
    CHECK(limit->stages.size() == 256u);
    CHECK_FALSE(hasMessage(
      decls,
      DeclMessageKind::Warning,
      "material 'textures/lp1a/limit' exceeded 256 stages"));
  }

  SECTION("stages the parser reads at material level, which a brace count misses")
  {
    // W.1: the bound comes from the copied lexer's tokens, so every one of these reaches
    // the 257th ParseStage in the engine and is refused here
    auto source = MemoryDeclSource{{
      DeclFile{
        "materials/crafted_stages.mtr",
        // the shortcut takes "{" as its image, and 256 blocks follow at material level
        "textures/lp1a/swallowed\n{\n\tdiffusemap {\n" + stagesOf(256, PlainStage)
          + "}\n}\n"
          // qer_editorimage reads "{" on its line
          + "textures/lp1a/editorimage\n{\n\tqer_editorimage {\n"
          + stagesOf(257, PlainStage)
          + "}\n}\n"
          // "/*/" is a whole comment to idLexer (its end test looks at the previous
          // character)
          + "textures/lp1a/comment\n{\n\t/*/\n" + stagesOf(257, PlainStage)
          + "}\n"
          // ".5diffusemap" is a number, then a shortcut
          + "textures/lp1a/number\n{\n" + stagesOf(200, PlainStage)
          + stagesOf(57, "\tpolygonOffset .5diffusemap _white\n") + "}\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    for (const auto* name :
         {"textures/lp1a/swallowed",
          "textures/lp1a/editorimage",
          "textures/lp1a/comment",
          "textures/lp1a/number"})
    {
      checkStagesRefused(decls, name);
    }
    CHECK(decls.defaultedCount("material") == 4);
  }

  SECTION("200 stages parse; interaction stage blocks count three times (M-ID-DIFF-03)")
  {
    const auto diffuseStage = std::string{"\t{ blend diffusemap map _white }\n"};
    auto source = MemoryDeclSource{{
      DeclFile{
        "materials/many_stages.mtr",
        "textures/lp1a/two_hundred\n{\n" + stagesOf(200, PlainStage) + "}\n"
          + "textures/lp1a/diffuse85\n{\n" + stagesOf(85, diffuseStage) + "}\n"
          + "textures/lp1a/diffuse86\n{\n" + stagesOf(86, diffuseStage) + "}\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    CHECK(lookup(decls, "textures/lp1a/two_hundred")->stages.size() == 200u);
    // the bound counts each block's "{" once and its "diffusemap" twice: 255, so the
    // material parses, and AddImplicitStages adds the _flat bump stage
    CHECK(lookup(decls, "textures/lp1a/diffuse85")->stages.size() == 86u);
    // 258: refused, although the engine parses 87 stages
    checkStagesRefused(decls, "textures/lp1a/diffuse86");
    CHECK(decls.defaultedCount("material") == 1);
  }

  SECTION("a shortcut writes up to two stage slots, so it counts twice (X.1)")
  {
    // ParseStage adds an unchecked coverage stage after an alpha-tested diffuse stage,
    // and a shortcut's stage is parsed from its image name without the quotes
    const auto alphaShortcut = std::string{"\tdiffusemap \"_white alphaTest 0.5\"\n"};
    const auto spacedShortcut = std::string{"\tdiffusemap \"_white vertexColor\"\n"};
    auto source = MemoryDeclSource{{
      DeclFile{
        "materials/coverage_stages.mtr",
        // the engine clears parseStages[256] at the 129th shortcut
        "textures/lp1a/alpha129\n{\n" + stagesOf(129, alphaShortcut)
          + "}\n"
          // the engine writes the 128th shortcut's coverage stage at parseStages[256]
          // without a warning, and the 129th's past it
          + "textures/lp1a/block_alpha128\n{\n" + PlainStage
          + stagesOf(128, alphaShortcut) + "}\n" + "textures/lp1a/block_alpha129\n{\n"
          + PlainStage + stagesOf(129, alphaShortcut)
          + "}\n"
          // 256 slots, the engine's limit
          + "textures/lp1a/alpha128\n{\n" + stagesOf(128, alphaShortcut)
          + "}\n"
          // a quoted image name with spaces but no alpha test: one slot, counted as two
          + "textures/lp1a/spaced254\n{\n" + stagesOf(254, PlainStage) + spacedShortcut
          + "}\n" + "textures/lp1a/spaced255\n{\n" + stagesOf(255, PlainStage)
          + spacedShortcut + "}\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    for (const auto* name :
         {"textures/lp1a/alpha129",
          "textures/lp1a/block_alpha128",
          "textures/lp1a/block_alpha129",
          "textures/lp1a/spaced255"})
    {
      checkStagesRefused(decls, name);
    }
    CHECK(decls.defaultedCount("material") == 4);

    // 128 diffuse stages and their 128 coverage stages; at 256 AddImplicitStages adds
    // nothing
    const auto alpha128 = lookup(decls, "textures/lp1a/alpha128");
    REQUIRE(alpha128->stages.size() == 256u);
    CHECK(
      std::count_if(
        alpha128->stages.begin(),
        alpha128->stages.end(),
        [](const auto& stage) { return stage.lighting == LightingCoverage; })
      == 128);
    CHECK_FALSE(alpha128->defaulted);

    // a bound of 256: 254 stages, the shortcut's diffuse stage and the implicit _flat
    // bump stage; spaced255 makes 256 slots in the engine but has a bound of 257
    const auto spaced254 = lookup(decls, "textures/lp1a/spaced254");
    CHECK(spaced254->stages.size() == 256u);
    CHECK_FALSE(spaced254->defaulted);
  }

  SECTION("a decl file with a name of 1024 or more characters is not read")
  {
    // M-ID-GUARD-06: MakeNameCanonical writes one byte past its buffer for such a name,
    // which a decl name, an inherit value or a table reference can pass it
    const auto longName = "textures/lp1a/" + std::string(1024, 'n');
    const auto longString = "\"" + std::string(1024, 's') + "\"";
    auto source = MemoryDeclSource{{
      DeclFile{"materials/a_name.mtr", longName + " { { map _white } }", 0},
      DeclFile{
        "materials/b_string.mtr",
        "textures/lp1a/str { qer_editorimage " + longString + " }",
        0},
      DeclFile{
        "materials/c_short.mtr",
        "textures/lp1a/short { qer_editorimage " + std::string(1000, 'k') + " }",
        0},
      // a dot ends the canonical name first, as in the retail PDAs' long texts
      DeclFile{
        "materials/d_dotted.mtr",
        "textures/lp1a/dotted { qer_editorimage \"Text. " + std::string(1100, 't')
          + "\" }",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    checkNotRead(decls, "materials/a_name.mtr");
    checkNotRead(decls, "materials/b_string.mtr");
    CHECK(lookup(decls, "textures/lp1a/str")->implicit);
    CHECK_FALSE(lookup(decls, "textures/lp1a/short")->implicit);
    CHECK_FALSE(lookup(decls, "textures/lp1a/dotted")->implicit);
  }

  SECTION("tokens the lexer joins or splits, as the engine reads them")
  {
    // W.1: without the guard, each of these files names a decl with a dotless token of
    // 1100 or more characters, and MakeNameCanonical writes past its buffer
    const auto half = std::string(600, 'A');
    auto source = MemoryDeclSource{{
      // two strings joined across a backslash (LEXFL_ALLOWBACKSLASHSTRINGCONCAT)
      DeclFile{
        "materials/a_backslash.mtr",
        "\"" + half + "\" \\\n\"" + half + "\" { { map _white } }\n",
        0},
      // a multi-character literal with spaces (LEXFL_ALLOWMULTICHARLITERALS)
      DeclFile{
        "materials/b_literal.mtr", "'" + spacedWords(1100) + "' { { map _white } }\n", 0},
      // a number, then a name: "1.5" ends at the 'A' ("Expecting '{'" takes "1.5", and
      // the name names the next decl)
      DeclFile{
        "materials/c_number.mtr",
        "textures/lp1a/number 1.5" + std::string(1100, 'A') + " { { map _white } }\n",
        0},
      // a quote inside a block comment is no string
      DeclFile{
        "materials/d_comment.mtr",
        "/* \" */ \"" + spacedWords(1100) + "\" { { map _white } }\n",
        0},
      // the same shapes with a dot in the first 1024 characters are read (and named
      // "text" and "more": the canonical name ends at the last dot it saw)
      DeclFile{
        "materials/e_dotted.mtr",
        "\"Text. " + half + "\" \\\n\"" + half + "\" { { map _white } }\n" + "'More. "
          + spacedWords(1100) + "' { { map _white } }\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    checkNotRead(decls, "materials/a_backslash.mtr");
    checkNotRead(decls, "materials/b_literal.mtr");
    checkNotRead(decls, "materials/c_number.mtr");
    checkNotRead(decls, "materials/d_comment.mtr");
    checkRead(decls, "materials/e_dotted.mtr");
    CHECK(decls.declNames("material") == std::vector<std::string>{"text", "more"});
  }

  SECTION("a PDA text of more than 1024 characters with an early dot is read")
  {
    // the shape of the retail newpdas/enpro_hammer.pda: an email's text alone on its
    // line, a string of 1075 characters whose first dot is its fifth character
    auto text = std::string{"Sir. "};
    while (text.size() < 1075)
    {
      text += "word ";
    }
    auto source = MemoryDeclSource{{
      DeclFile{
        "newpdas/lp1a_long.pda",
        "pda pda_lp1a_long\n{\n\tname \"LP1A\"\n}\n\nemail lp1a_long_email\n{\n\tfrom "
        "\"lp1a\"\n\tsubject \"a long text\"\n\ttext\n\""
          + text + "\"\n}\n",
        0},
    }};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());

    checkRead(decls, "newpdas/lp1a_long.pda");
    CHECK(decls.declNames("email") == std::vector<std::string>{"lp1a_long_email"});
  }
}

TEST_CASE("MaterialDecls geometry flags and entityDefs (C22, C25)")
{
  auto source = MemoryDeclSource{{
    DeclFile{
      "materials/lp1b.mtr",
      "textures/lp1b/mikk { mikktspace diffusemap textures/lp1b/mikk_d }\n"
      "textures/lp1b/hard { unsmoothedTangents diffusemap textures/lp1b/hard_d }\n"
      "textures/lp1b/twoLit { twoSided diffusemap textures/lp1b/two_d }\n"
      "textures/lp1b/twoUnlit { twoSided { blend add map textures/lp1b/glow } }\n"
      "textures/lp1b/plain { diffusemap textures/lp1b/plain_d }\n",
      0},
    DeclFile{
      "def/lp1b.def",
      "entityDef lp1b_helper { \"dmapOnly\" \"1\" \"Model\" \"a\" }\n"
      "entityDef lp1b_derived { \"inherit\" \"lp1b_helper\" \"model\" \"b\" }\n"
      "entityDef lp1b_off { \"dmapOnly\" \"0\" }\n",
      0},
  }};
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());

  SECTION("the flags the copied parser sets")
  {
    const auto mikk = lookup(decls, "textures/lp1b/mikk");
    CHECK(mikk->mikktspace);
    CHECK_FALSE(mikk->unsmoothedTangents);
    CHECK_FALSE(mikk->createBackSides);

    const auto hard = lookup(decls, "textures/lp1b/hard");
    CHECK(hard->unsmoothedTangents);
    CHECK_FALSE(hard->mikktspace);

    // a lit two-sided material is made front-sided with back sides (Parse)
    const auto twoLit = lookup(decls, "textures/lp1b/twoLit");
    CHECK(twoLit->createBackSides);
    CHECK(twoLit->cullType == 0);

    // an unlit one stays two-sided
    const auto twoUnlit = lookup(decls, "textures/lp1b/twoUnlit");
    CHECK_FALSE(twoUnlit->createBackSides);
    CHECK(twoUnlit->cullType == CullTwoSided);

    const auto plain = lookup(decls, "textures/lp1b/plain");
    CHECK_FALSE(plain->mikktspace);
    CHECK_FALSE(plain->unsmoothedTangents);
    CHECK_FALSE(plain->createBackSides);
  }

  SECTION("the registry gives every material the default editor image size (C21)")
  {
    const auto plain = lookup(decls, "textures/lp1b/plain");
    CHECK(plain->editorImageSize == EditorImageSize{});
    CHECK(plain->editorImageSize.width == 8);
    CHECK(plain->editorImageSize.height == 8);
    CHECK(plain->editorImageSize.state == EditorImageSize::State::Default);
  }

  SECTION("dmapOnly comes from the def, inherited keys included")
  {
    const auto helper = decls.entityDef("lp1b_helper");
    REQUIRE(helper);
    CHECK((*helper.value)->dmapOnly);
    CHECK((*helper.value)->registry == decls.id());

    // IsMapOnlyClass reads the merged dict, which holds the parent's key
    const auto derived = decls.entityDef("lp1b_derived");
    REQUIRE(derived);
    CHECK((*derived.value)->dmapOnly);
    // the dicts' key pool ignores case, so "model" is spelled as the pool first saw it
    CHECK(
      (*derived.value)->keys
      == EntityKeys{
        {"Model", "b"},
        {"classname", "lp1b_derived"},
        {"dmapOnly", "1"},
      });
    CHECK(*findKey((*derived.value)->keys, "model") == "b");

    const auto off = decls.entityDef("lp1b_off");
    REQUIRE(off);
    CHECK_FALSE((*off.value)->dmapOnly);

    // the decl name is canonical
    const auto upper = decls.entityDef("LP1B_Helper");
    REQUIRE(upper);
    CHECK((*upper.value)->name == "lp1b_helper");
  }
}

TEST_CASE("findKey")
{
  const auto keys = EntityKeys{
    {"classname", "light"},
    {"Origin", "0 0 0"},
    {"_color", "1 1 1"},
    {"origin", "8 8 8"},
    {"\xC3\x84\xC3\x96", "upper"},
  };

  SECTION("ASCII letters compare without case")
  {
    REQUIRE(findKey(keys, "CLASSNAME"));
    CHECK(*findKey(keys, "CLASSNAME") == "light");
    CHECK(*findKey(keys, "_Color") == "1 1 1");
  }

  SECTION("the last of two keys that differ in case wins")
  {
    REQUIRE(findKey(keys, "ORIGIN"));
    CHECK(*findKey(keys, "ORIGIN") == "8 8 8");
    CHECK(findKey(keys, "origin") == &keys[3].second);
  }

  SECTION("only ASCII letters are folded, and the whole key compares")
  {
    CHECK(findKey(keys, "\xC3\xA4\xC3\xB6") == nullptr);
    CHECK(findKey(keys, "\xC3\x84\xC3\x96") == &keys[4].second);
    CHECK(findKey(keys, "origi") == nullptr);
    CHECK(findKey(keys, "origins") == nullptr);
  }

  SECTION("an empty name finds nothing")
  {
    CHECK(findKey(keys, "") == nullptr);
    CHECK(findKey(EntityKeys{{"", "x"}}, "") == nullptr);
  }
}

TEST_CASE("Engine messages (D63)")
{
  const auto pendingBefore = pendingEngineMessagesForTesting();

  SECTION("each thread's messages are kept apart, in order, until drained")
  {
    const auto report = [](const std::string& prefix) {
      reportEngineMessageForTesting(DeclMessageKind::Info, prefix + " info");
      reportEngineMessageForTesting(DeclMessageKind::Warning, prefix + " warning");
      reportEngineMessageForTesting(DeclMessageKind::Error, prefix + " error");
      // C17: the never-returning calls return
      reportEngineMessageForTesting(DeclMessageKind::FatalError, prefix + " fatal");
    };
    auto first = std::thread{[&] { report("first"); }};
    const auto firstId = first.get_id();
    first.join();
    auto second = std::thread{[&] { report("second"); }};
    const auto secondId = second.get_id();
    second.join();
    CHECK(pendingEngineMessagesForTesting() == pendingBefore + 8u);

    const auto firstMessages = drainEngineMessagesForThread(firstId);
    REQUIRE(firstMessages.size() == 4u);
    CHECK(firstMessages[0].kind == DeclMessageKind::Info);
    // without the newline Printf was given
    CHECK(firstMessages[0].text == "first info");
    CHECK(firstMessages[1].kind == DeclMessageKind::Warning);
    CHECK(firstMessages[1].text == "first warning");
    CHECK(firstMessages[2].kind == DeclMessageKind::Error);
    CHECK(firstMessages[3].kind == DeclMessageKind::FatalError);
    CHECK(firstMessages[3].text == "first fatal");
    CHECK(pendingEngineMessagesForTesting() == pendingBefore + 4u);
    CHECK(drainEngineMessagesForThread(firstId).empty());

    const auto secondMessages = drainEngineMessagesForThread(secondId);
    REQUIRE(secondMessages.size() == 4u);
    CHECK(secondMessages[0].text == "second info");
    CHECK(secondMessages[3].text == "second fatal");
    CHECK(pendingEngineMessagesForTesting() == pendingBefore);
  }

  SECTION("threads that report at once lose nothing")
  {
    constexpr auto ThreadCount = 4;
    constexpr auto MessageCount = 500;
    auto threads = std::vector<std::thread>{};
    for (auto t = 0; t < ThreadCount; ++t)
    {
      threads.emplace_back([t] {
        for (auto i = 0; i < MessageCount; ++i)
        {
          reportEngineMessageForTesting(
            DeclMessageKind::Warning, std::to_string(t) + ":" + std::to_string(i));
        }
      });
    }
    auto ids = std::vector<std::thread::id>{};
    for (auto& thread : threads)
    {
      ids.push_back(thread.get_id());
      thread.join();
    }
    for (std::size_t t = 0; t < ids.size(); ++t)
    {
      CAPTURE(t);
      const auto messages = drainEngineMessagesForThread(ids[t]);
      REQUIRE(messages.size() == std::size_t(MessageCount));
      for (std::size_t i = 0; i < messages.size(); ++i)
      {
        CHECK(messages[i].text == std::to_string(t) + ":" + std::to_string(i));
      }
    }
    CHECK(pendingEngineMessagesForTesting() == pendingBefore);
  }

  SECTION("a registry drains its own thread's messages, not another thread's")
  {
    // reported before the registry exists: no registry received it
    reportEngineMessageForTesting(DeclMessageKind::Warning, "before the registry");

    auto source = MemoryDeclSource{fixtureFiles()};
    auto decls = MaterialDecls{source};
    REQUIRE(decls.constructionError().empty());
    CHECK_FALSE(hasMessage(decls, DeclMessageKind::Warning, "before the registry"));

    auto other = std::thread{
      [] { reportEngineMessageForTesting(DeclMessageKind::Error, "another thread"); }};
    const auto otherId = other.get_id();
    other.join();
    reportEngineMessageForTesting(DeclMessageKind::Warning, "the registry's thread");

    CHECK(hasMessage(decls, DeclMessageKind::Warning, "the registry's thread"));
    CHECK_FALSE(hasMessage(decls, DeclMessageKind::Error, "another thread"));
    // another thread's error does not fail the registry's lookups
    CHECK(decls.lookup("textures/lp1a/wall"));

    const auto otherMessages = drainEngineMessagesForThread(otherId);
    REQUIRE(otherMessages.size() == 1u);
    CHECK(otherMessages[0].text == "another thread");
    CHECK(otherMessages[0].kind == DeclMessageKind::Error);
  }

  SECTION("an assert is reported from any thread")
  {
    auto thread = std::thread{
      [] { reportEngineMessageForTesting(DeclMessageKind::Assert, "x == y"); }};
    const auto id = thread.get_id();
    thread.join();
    const auto messages = drainEngineMessagesForThread(id);
    REQUIRE(messages.size() == 1u);
    CHECK(messages[0].kind == DeclMessageKind::Assert);
    CHECK(messages[0].text == "EngineMessages.cpp(0): x == y");
  }

  CHECK(pendingEngineMessagesForTesting() == pendingBefore);
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("MaterialDecls build and destroy leak nothing")
{
  auto source = MemoryDeclSource{fixtureFiles()};
  const auto names = std::vector<std::string>{
    "textures/lp1a/wall",        "textures/lp1a/pbr",
    "textures/lp1a/diffuseOnly", "textures/lp1a/grate",
    "textures/lp1a/expr",        "textures/lp1a/snap",
    "textures/lp1a/rotate",      "textures/lp1a/constant",
    "textures/lp1a/program",     "textures/lp1a/screen",
    "textures/lp1a/soundviz",    "textures/lp1a/guiscreen",
    "textures/lp1a/flare",       "textures/lp1a/decal",
    "textures/lp1a/sky",         "lights/lp1a_round",
    "textures/lp1a/broken",      "textures/lp1a/smoke",
    "textures/lp1a/nodecl",      "_white",
  };

  // builds the registry, parses every fixture material (the videomap and soundmap stages
  // included), evaluates them and parses the entityDefs, then destroys it all
  const auto cycle = [&]() {
    auto ok = true;
    auto decls = MaterialDecls{source};
    for (const auto& name : names)
    {
      auto material = decls.lookup(name);
      ok = ok && bool(material);
      if (material)
      {
        ok = ok && bool(decls.evaluate(**material.value, NoParms, 0.5f));
      }
    }
    ok = ok && bool(decls.entityDef("lp1a_lamp"));
    ok = ok && bool(decls.entityDef("lp1a_orphan"));
    return ok;
  };

  // the first cycle may allocate once-only runtime state (stdio, locale)
  REQUIRE(cycle());

  _CrtMemState before;
  _CrtMemCheckpoint(&before);

  auto ok = true;
  for (int i = 0; i < 100; ++i)
  {
    ok = cycle() && ok;
  }

  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  _CrtMemState difference;
  const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
  if (grew)
  {
    _CrtMemDumpStatistics(&difference);
  }

  CHECK(ok);
  CHECK_FALSE(grew);
}
#endif

} // namespace tb::lit
