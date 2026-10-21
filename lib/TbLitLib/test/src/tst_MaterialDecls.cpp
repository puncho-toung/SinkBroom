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
#include "lit/Materials.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
  const auto source = MemoryDeclSource{fixtureFiles()};
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

  SECTION("entityDefKeys")
  {
    SECTION("inherited keys are merged under the entityDef's own")
    {
      const auto keys = decls.entityDefKeys("lp1a_lamp");
      INFO(keys.error);
      REQUIRE(keys);
      CHECK(
        *keys.value
        == std::vector<std::pair<std::string, std::string>>{
          {"noshadows", "1"},
          {"model", "models/lp1a/lamp.lwo"},
          {"classname", "lp1a_lamp"},
          {"spawnclass", "idStaticEntity"},
          {"_color", "1 0.5 0.25"},
        });
    }

    SECTION("an unknown parent warns")
    {
      const auto keys = decls.entityDefKeys("lp1a_orphan");
      REQUIRE(keys);
      CHECK(hasMessage(
        decls,
        DeclMessageKind::Warning,
        "file def/lp1a.def, line 23: Unknown entityDef 'lp1a_nope' inherited by "
        "'lp1a_orphan'"));
    }

    SECTION("an unknown entityDef")
    {
      CHECK_FALSE(decls.entityDefKeys("lp1a_missing"));
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
    const auto source = MemoryDeclSource{{
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
    const auto source = MemoryDeclSource{{
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

TEST_CASE("Materials")
{
  const auto source = MemoryDeclSource{fixtureFiles()};

  SECTION("makeMaterialDecls")
  {
    auto decls = makeMaterialDecls(source);
    REQUIRE(decls.is_success());
    CHECK(makeMaterialDecls(source).is_error());
  }

  SECTION("lookupMaterial and evaluateMaterial")
  {
    auto decls = makeMaterialDecls(source) | kdl::value();
    const auto material = lookupMaterial(*decls, "textures/lp1a/constant") | kdl::value();
    CHECK(material->name == "textures/lp1a/constant");

    const auto registers =
      evaluateMaterial(*decls, *material, NoParms, 0.0f) | kdl::value();
    CHECK(registers.registers.size() == size_t(material->numRegisters));

    CHECK(lookupMaterial(*decls, std::string(2000, 'x')).is_error());
  }

  SECTION("lookupEntityDefKeys")
  {
    auto decls = makeMaterialDecls(source) | kdl::value();
    CHECK(lookupEntityDefKeys(*decls, "lp1a_base").is_success());
    CHECK(lookupEntityDefKeys(*decls, "lp1a_missing").is_error());
  }
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("MaterialDecls build and destroy leak nothing")
{
  const auto source = MemoryDeclSource{fixtureFiles()};
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
    ok = ok && bool(decls.entityDefKeys("lp1a_lamp"));
    ok = ok && bool(decls.entityDefKeys("lp1a_orphan"));
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
