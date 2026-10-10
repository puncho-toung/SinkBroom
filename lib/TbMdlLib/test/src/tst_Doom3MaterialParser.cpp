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
#include "TestParserStatus.h"
#include "base/Logger.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/Reader.h"
#include "gl/Material.h"
#include "mdl/CatchConfig.h"
#include "mdl/Doom3MaterialParser.h"
#include "mdl/Quake3Shader.h"

#include "kd/flat_set.h"
#include "kd/result.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

using Lighting = Quake3ShaderStage::Lighting;
using BlendFunc = Quake3ShaderStage::BlendFunc;
using Coverage = Quake3Shader::Coverage;
using Culling = Quake3Shader::Culling;
using Format = Quake3Shader::Format;

const auto GreaterEqual = gl::MaterialAlphaFunc::Compare::GreaterEqual;

std::vector<Quake3Shader> parseFile(
  const std::filesystem::path& path, TestParserStatus& status)
{
  auto file = fs::Disk::openFile(path) | kdl::value();
  auto reader = file->reader().buffer();
  return Doom3MaterialParser{reader.stringView()}.parse(status);
}

const Quake3Shader& findShader(
  const std::vector<Quake3Shader>& shaders, const std::filesystem::path& shaderPath)
{
  const auto it = std::ranges::find_if(
    shaders, [&](const auto& shader) { return shader.shaderPath == shaderPath; });
  REQUIRE(it != shaders.end());
  return *it;
}

std::vector<std::filesystem::path> shaderPaths(const std::vector<Quake3Shader>& shaders)
{
  auto result = std::vector<std::filesystem::path>{};
  for (const auto& shader : shaders)
  {
    result.push_back(shader.shaderPath);
  }
  return result;
}

std::vector<std::filesystem::path> paths(const std::vector<std::string>& strs)
{
  return {strs.begin(), strs.end()};
}

// A material the engine replaces with its default material "{ { blend blend map
// _default } }", which keeps only the editor image name.
Quake3Shader defaultedShader(
  const std::string& shaderPath, const std::string& editorImage = "")
{
  return Quake3Shader{
    .shaderPath = shaderPath,
    .editorImage = editorImage,
    .lightImage = "",
    .culling = Culling::Front,
    .surfaceParms = {"coverage_translucent", "engine_defaulted", "noshadows"},
    .stages =
      {
        {.map = "_default",
         .blendFunc = {"GL_SRC_ALPHA", "GL_ONE_MINUS_SRC_ALPHA"},
         .alphaFunc = {},
         .lighting = Lighting::None},
      },
    .qerAlphaFunc = {},
    .format = Format::Doom3,
    .coverage = Coverage::Translucent,
    .previewImages = editorImage.empty() ? paths({}) : paths({editorImage}),
    .engineDefaulted = true,
  };
}

} // namespace

TEST_CASE("Doom3MaterialParser")
{
  const auto fixturePath = getFixtureRoot() / "test" / "mdl" / "Doom3MaterialParser";
  auto status = TestParserStatus{};

  SECTION("Empty input and comments only")
  {
    CHECK(Doom3MaterialParser{""}.parse(status).empty());
    CHECK(Doom3MaterialParser{R"(
// a line comment
/* a block comment
   over two lines */
  //
)"}
            .parse(status)
            .empty());
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Tool materials without stages")
  {
    CHECK(
      parseFile(fixturePath / "tool_materials.mtr", status)
      == std::vector<Quake3Shader>{
        {
          .shaderPath = "textures/fixture/caulk",
          .editorImage = "textures/fixture/caulk_ed.tga",
          .lightImage = "",
          .culling = Culling::Front,
          .surfaceParms = {"noshadows", "forceopaque", "coverage_opaque", "nostages"},
          .stages = {},
          .qerAlphaFunc = {},
          .format = Format::Doom3,
          .coverage = Coverage::Opaque,
          .previewImages = {"textures/fixture/caulk_ed.tga"},
          .engineDefaulted = false,
        },
        {
          .shaderPath = "textures/fixture/nodraw",
          .editorImage = "textures/fixture/nodraw_ed.tga",
          .lightImage = "",
          .culling = Culling::Front,
          .surfaceParms = {"nonsolid", "noshadows", "coverage_translucent", "nostages"},
          .stages = {},
          .qerAlphaFunc = {},
          .format = Format::Doom3,
          .coverage = Coverage::Translucent,
          .previewImages = {"textures/fixture/nodraw_ed.tga"},
          .engineDefaulted = false,
        },
        {
          .shaderPath = "textures/fixture/clip",
          .editorImage = "textures/fixture/clip_ed.tga",
          .lightImage = "",
          .culling = Culling::Front,
          .surfaceParms =
            {"noimpact",
             "nonsolid",
             "monsterclip",
             "playerclip",
             "noshadows",
             "coverage_translucent",
             "nostages"},
          .stages = {},
          .qerAlphaFunc = {},
          .format = Format::Doom3,
          .coverage = Coverage::Translucent,
          .previewImages = {"textures/fixture/clip_ed.tga"},
          .engineDefaulted = false,
        },
      });
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Shorthand maps")
  {
    const auto data = R"(
textures/fixture/panel
{
	qer_editorimage	textures/fixture/panel_ed.tga
	bumpmap			textures/fixture/panel_local.tga
	diffusemap		textures/fixture/panel_d.tga
	specularmap		textures/fixture/panel_s.tga
}

textures/fixture/aliases
{
	normalmap		textures/fixture/aliases_n.tga
	basecolormap	textures/fixture/aliases_c.tga
	rmaomap			textures/fixture/aliases_rmao.tga
	reflectionmap	textures/fixture/aliases_r.tga
	pbrmap			textures/fixture/aliases_p.tga
}
)";

    CHECK(
      Doom3MaterialParser{data}.parse(status)
      == std::vector<Quake3Shader>{
        {
          .shaderPath = "textures/fixture/panel",
          .editorImage = "textures/fixture/panel_ed.tga",
          .lightImage = "",
          .culling = Culling::Front,
          .surfaceParms = {"coverage_opaque"},
          .stages =
            {
              {.map = "textures/fixture/panel_local.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Bump},
              {.map = "textures/fixture/panel_d.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Diffuse},
              {.map = "textures/fixture/panel_s.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Specular},
            },
          .qerAlphaFunc = {},
          .format = Format::Doom3,
          .coverage = Coverage::Opaque,
          .previewImages =
            {"textures/fixture/panel_ed.tga", "textures/fixture/panel_d.tga"},
          .engineDefaulted = false,
        },
        {
          .shaderPath = "textures/fixture/aliases",
          .editorImage = "",
          .lightImage = "",
          .culling = Culling::Front,
          .surfaceParms = {"coverage_opaque"},
          .stages =
            {
              {.map = "textures/fixture/aliases_n.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Bump},
              {.map = "textures/fixture/aliases_c.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Diffuse},
              {.map = "textures/fixture/aliases_rmao.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Specular},
              {.map = "textures/fixture/aliases_r.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Specular},
              {.map = "textures/fixture/aliases_p.tga",
               .blendFunc = {"", ""},
               .alphaFunc = {},
               .lighting = Lighting::Specular},
            },
          .qerAlphaFunc = {},
          .format = Format::Doom3,
          .coverage = Coverage::Opaque,
          .previewImages = {"textures/fixture/aliases_c.tga"},
          .engineDefaulted = false,
        },
      });
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Long-form stages")
  {
    const auto data = R"(
textures/fixture/lamp
{
	qer_editorimage	textures/fixture/lamp_ed.tga

	{if ( parm7 == 0 )
		blend		bumpmap
		map			textures/fixture/lamp_local.tga
	}
	{if ( parm7 == 0 )
		blend		diffusemap
		map			textures/fixture/lamp_d.tga
	}
	{//a comment right after the brace
		blend		add
		map			textures/fixture/lamp_glow.tga
		rgb			parm0 * 0.5
	}
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);

    const auto& lamp = shaders.front();
    CHECK(
      lamp.stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/fixture/lamp_local.tga",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Bump},
        {.map = "textures/fixture/lamp_d.tga",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Diffuse},
        {.map = "textures/fixture/lamp_glow.tga",
         .blendFunc = {"GL_ONE", "GL_ONE"},
         .alphaFunc = {},
         .lighting = Lighting::None},
      });
    CHECK(
      lamp.previewImages
      == paths({"textures/fixture/lamp_ed.tga", "textures/fixture/lamp_d.tga"}));
    CHECK(lamp.coverage == Coverage::Opaque);
    CHECK_FALSE(lamp.engineDefaulted);
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Image program spanning lines")
  {
    const auto data = R"(
textures/fixture/floor
{
	qer_editorimage textures/fixture/floor_ed.tga

	bumpmap	addnormals ( textures/fixture/floor_local.tga, heightmap (
textures/fixture/floor_h.tga, 1 ) )
	diffusemap	textures/fixture/floor_d.tga
	specularmap	textures/fixture/floor_s.tga
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK(
      shaders.front().stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/fixture/floor_local.tga",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Bump},
        {.map = "textures/fixture/floor_d.tga",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Diffuse},
        {.map = "textures/fixture/floor_s.tga",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Specular},
      });
    CHECK(
      shaders.front().previewImages
      == paths({"textures/fixture/floor_ed.tga", "textures/fixture/floor_d.tga"}));
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Image program leaves")
  {
    const auto data = R"(
textures/fixture/leaf_heightmap	{ diffusemap heightmap( textures/fixture/h, 4 ) }
textures/fixture/leaf_makealpha	{ diffusemap makeAlpha( textures/fixture/x ) }
textures/fixture/leaf_combine	{ diffusemap combineRgba( textures/fixture/r, textures/fixture/g, textures/fixture/b ) }
textures/fixture/leaf_nested	{ diffusemap invertColor( smoothnormals( scale( textures/fixture/s, 1, 1, 1, 1 ) ) ) }
textures/fixture/leaf_quoted	{ { blend diffusemap map "textures/fixture/q.tga" } }
textures/fixture/leaf_white		{ diffusemap _white }
textures/fixture/leaf_black		{ diffusemap _black }
textures/fixture/leaf_flat		{ bumpmap _flat diffusemap textures/fixture/d }
textures/fixture/leaf_intrinsic	{ { blend add map _flat } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 9);

    const auto checkLeaf = [&](const auto& shaderPath, const auto& image) {
      CAPTURE(shaderPath);
      const auto& shader = findShader(shaders, shaderPath);
      REQUIRE(shader.stages.size() == 1);
      CHECK(shader.stages.front().map == std::filesystem::path{image});
      CHECK(shader.previewImages == paths({image}));
    };

    checkLeaf("textures/fixture/leaf_heightmap", "textures/fixture/h");
    checkLeaf("textures/fixture/leaf_makealpha", "textures/fixture/x");
    checkLeaf("textures/fixture/leaf_combine", "textures/fixture/r");
    checkLeaf("textures/fixture/leaf_nested", "textures/fixture/s");
    checkLeaf("textures/fixture/leaf_quoted", "textures/fixture/q.tga");
    // the image program maps _white and _black to image files
    checkLeaf("textures/fixture/leaf_white", "guis/assets/white");
    checkLeaf("textures/fixture/leaf_black", "textures/black");

    // other names starting with '_' are generated images: they are kept on the stage
    // but are never preview images
    const auto& flat = findShader(shaders, "textures/fixture/leaf_flat");
    REQUIRE(flat.stages.size() == 2);
    CHECK(flat.stages[0].map == std::filesystem::path{"_flat"});
    CHECK(flat.previewImages == paths({"textures/fixture/d"}));

    const auto& intrinsic = findShader(shaders, "textures/fixture/leaf_intrinsic");
    REQUIRE(intrinsic.stages.size() == 1);
    CHECK(intrinsic.stages[0].map == std::filesystem::path{"_flat"});
    CHECK(intrinsic.previewImages.empty());

    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Implicit stages and preview order")
  {
    const auto data = R"(
textures/fixture/bump_only
{
	bumpmap		textures/fixture/bump_only_local.tga
}

textures/fixture/bump_only_editor
{
	qer_editorimage	textures/fixture/bump_only_ed.tga
	bumpmap		textures/fixture/bump_only_local.tga
}

textures/fixture/specular_only
{
	{
		blend	add
		map		textures/fixture/specular_only_glow.tga
	}
	specularmap	textures/fixture/specular_only_s.tga
}

textures/fixture/editor_without_diffuse
{
	qer_editorimage	textures/fixture/editor_without_diffuse_ed.tga
	{
		blend	add
		map		textures/fixture/editor_without_diffuse_glow.tga
	}
}

textures/fixture/second_diffuse
{
	{
		blend	add
		map		textures/fixture/second_diffuse_glow.tga
	}
	diffusemap	textures/fixture/second_diffuse_d.tga
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 5);

    // a bump stage without a diffuse or specular stage gets an implicit _white diffuse
    // stage, which the engine previews
    CHECK(
      findShader(shaders, "textures/fixture/bump_only").previewImages
      == paths({"guis/assets/white"}));
    CHECK(
      findShader(shaders, "textures/fixture/bump_only_editor").previewImages
      == paths({"textures/fixture/bump_only_ed.tga", "guis/assets/white"}));

    // the implicit _flat bump stage sorts before the specular stage, but after the
    // ambient stage, which stays first
    CHECK(
      findShader(shaders, "textures/fixture/specular_only").previewImages
      == paths({"textures/fixture/specular_only_glow.tga"}));

    // with an editor image, the engine only falls back to a diffuse stage
    CHECK(
      findShader(shaders, "textures/fixture/editor_without_diffuse").previewImages
      == paths({"textures/fixture/editor_without_diffuse_ed.tga"}));

    // the first diffuse stage wins over stage 0
    CHECK(
      findShader(shaders, "textures/fixture/second_diffuse").previewImages
      == paths({"textures/fixture/second_diffuse_d.tga"}));

    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("First diffuse stage without an image")
  {
    const auto data = R"(
textures/fixture/video
{
	{
		blend		diffusemap
		videomap	video/fixture.roq
	}
	{
		blend		add
		map			textures/fixture/video_glow.tga
	}
}

textures/fixture/video_editor
{
	qer_editorimage	textures/fixture/video_ed.tga
	{
		blend		diffusemap
		videomap	loop video/fixture.roq
	}
	{
		blend		add
		map			textures/fixture/video_glow.tga
	}
}

textures/fixture/plain_diffuse
{
	{
		blend		diffusemap
	}
	{
		blend		add
		map			textures/fixture/plain_glow.tga
	}
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 3);

    // a video leaves the first diffuse stage without an image, so the engine takes
    // the first stage after sorting: the ambient glow stage
    CHECK(
      findShader(shaders, "textures/fixture/video").previewImages
      == paths({"textures/fixture/video_glow.tga"}));
    CHECK(
      findShader(shaders, "textures/fixture/video_editor").previewImages
      == paths({"textures/fixture/video_ed.tga", "textures/fixture/video_glow.tga"}));

    // a stage without map, video or program gets the default image, which is no preview
    CHECK(findShader(shaders, "textures/fixture/plain_diffuse").previewImages.empty());

    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Quoted shorthand images")
  {
    const auto data = R"(
textures/fixture/quoted_plain	{ diffusemap "textures/fixture/plain.tga" }
textures/fixture/quoted_split	{ diffusemap "textures/fixture/x-y.tga" }
textures/fixture/quoted_stage	{ { blend diffusemap map "textures/fixture/x-y.tga" } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 3);

    // the engine reads a shorthand's image again without quotes, which splits x-y.tga
    CHECK(
      findShader(shaders, "textures/fixture/quoted_plain").previewImages
      == paths({"textures/fixture/plain.tga"}));
    CHECK(
      findShader(shaders, "textures/fixture/quoted_split")
      == defaultedShader("textures/fixture/quoted_split"));
    CHECK(
      findShader(shaders, "textures/fixture/quoted_stage").previewImages
      == paths({"textures/fixture/x-y.tga"}));

    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 3, column 44: Quoted image 'textures/fixture/x-y.tga' splits when the "
        "engine reads the diffusemap shorthand again without quotes in material "
        "'textures/fixture/quoted_split'; the engine replaces this material with its "
        "default material",
      }));
  }

  SECTION("Tables and other typed decls")
  {
    const auto shaders = parseFile(fixturePath / "typed_decls.mtr", status);
    CHECK(
      shaderPaths(shaders)
      == paths(
        {"textures/fixture/first", "textures/fixture/typed", "textures/fixture/last"}));
    CHECK(
      findShader(shaders, "textures/fixture/last").previewImages
      == paths({"textures/fixture/last_d.tga"}));
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Blend forms")
  {
    const auto data = R"(
textures/fixture/blend_add		{ { blend add		map textures/fixture/a } }
textures/fixture/blend_filter	{ { blend filter	map textures/fixture/a } }
textures/fixture/blend_modulate	{ { blend modulate	map textures/fixture/a } }
textures/fixture/blend_blend	{ { blend blend		map textures/fixture/a } }
textures/fixture/blend_none		{ { blend none		map textures/fixture/a } }
textures/fixture/blend_factors	{ { blend GL_DST_COLOR, GL_ZERO				map textures/fixture/a } }
textures/fixture/blend_lower	{ { blend gl_one, gl_one_minus_src_alpha	map textures/fixture/a } }
textures/fixture/blend_bad		{ { blend GL_SRC_COLOR, GL_ONE				map textures/fixture/a } }
textures/fixture/blend_after	{ { blend add		map textures/fixture/a } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 9);

    const auto checkBlend = [&](const auto& shaderPath, const BlendFunc& blendFunc) {
      CAPTURE(shaderPath);
      const auto& shader = findShader(shaders, shaderPath);
      REQUIRE(shader.stages.size() == 1);
      CHECK(shader.stages.front().blendFunc == blendFunc);
      CHECK_FALSE(shader.engineDefaulted);
    };

    checkBlend("textures/fixture/blend_add", {"GL_ONE", "GL_ONE"});
    checkBlend("textures/fixture/blend_filter", {"GL_DST_COLOR", "GL_ZERO"});
    checkBlend("textures/fixture/blend_modulate", {"GL_DST_COLOR", "GL_ZERO"});
    checkBlend(
      "textures/fixture/blend_blend", {"GL_SRC_ALPHA", "GL_ONE_MINUS_SRC_ALPHA"});
    checkBlend("textures/fixture/blend_none", {"GL_ZERO", "GL_ONE"});
    checkBlend("textures/fixture/blend_factors", {"GL_DST_COLOR", "GL_ZERO"});
    checkBlend("textures/fixture/blend_lower", {"GL_ONE", "GL_ONE_MINUS_SRC_ALPHA"});
    checkBlend("textures/fixture/blend_after", {"GL_ONE", "GL_ONE"});

    // GL_SRC_COLOR is no source factor; the engine defaults the material
    const auto& bad = findShader(shaders, "textures/fixture/blend_bad");
    CHECK(bad == defaultedShader("textures/fixture/blend_bad"));
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 9, column 39: Unknown blend source factor 'GL_SRC_COLOR' in material "
        "'textures/fixture/blend_bad'; the engine replaces this material with its "
        "default material",
      }));
  }

  SECTION("Coverage")
  {
    const auto data = R"(
textures/fixture/cov_translucent	{ translucent { blend blend map textures/fixture/a } }
textures/fixture/cov_alphatest		{ { blend diffusemap map textures/fixture/a alphaTest 0.5 } }
textures/fixture/cov_threshold		{ { blend diffusemap map textures/fixture/a alphaTest 0.25 } }
textures/fixture/cov_parm			{ { blend diffusemap map textures/fixture/a alphaTest parm7 } }
textures/fixture/cov_expression		{ { blend diffusemap map textures/fixture/a alphaTest 0.25 * parm7 } }
textures/fixture/cov_blended		{ { blend add map textures/fixture/a } }
textures/fixture/cov_unblended		{ { blend GL_ONE, GL_ZERO map textures/fixture/a } }
textures/fixture/cov_interaction	{ diffusemap textures/fixture/a { blend add map textures/fixture/b } }
textures/fixture/cov_empty			{ }
textures/fixture/cov_last_alpha		{ translucent { blend diffusemap map textures/fixture/a alphaTest 0.5 } }
textures/fixture/cov_last_opaque	{ { blend diffusemap map textures/fixture/a alphaTest 0.5 } forceOpaque }
textures/fixture/cov_mirror			{ translucent mirror }
textures/fixture/cov_current		{ forceOpaque { blend filter map _currentRender } }
textures/fixture/cov_portalsky		{ forceOpaque sort portalSky
	{ blend filter map _currentRender } }
textures/fixture/cov_fragment		{ { program fixture.vfp fragmentMap 0 _currentRender } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 15);

    const auto coverageOf = [&](const auto& shaderPath) {
      return findShader(shaders, shaderPath).coverage;
    };
    const auto alphaFuncOf = [&](const auto& shaderPath) {
      const auto& shader = findShader(shaders, shaderPath);
      REQUIRE(shader.stages.size() == 1);
      return shader.stages.front().alphaFunc;
    };

    CHECK(coverageOf("textures/fixture/cov_translucent") == Coverage::Translucent);

    CHECK(coverageOf("textures/fixture/cov_alphatest") == Coverage::Perforated);
    CHECK(
      alphaFuncOf("textures/fixture/cov_alphatest")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.5f});
    CHECK(
      alphaFuncOf("textures/fixture/cov_threshold")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.25f});
    CHECK(
      alphaFuncOf("textures/fixture/cov_parm")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.5f});
    CHECK(
      alphaFuncOf("textures/fixture/cov_expression")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.5f});

    CHECK(coverageOf("textures/fixture/cov_blended") == Coverage::Translucent);
    CHECK(coverageOf("textures/fixture/cov_unblended") == Coverage::Opaque);
    CHECK(coverageOf("textures/fixture/cov_interaction") == Coverage::Opaque);

    const auto& empty = findShader(shaders, "textures/fixture/cov_empty");
    CHECK(empty.coverage == Coverage::Translucent);
    CHECK(
      empty.surfaceParms
      == kdl::flat_set<std::string>{"coverage_translucent", "nostages"});

    // the last explicit coverage wins
    CHECK(coverageOf("textures/fixture/cov_last_alpha") == Coverage::Perforated);
    CHECK(coverageOf("textures/fixture/cov_last_opaque") == Coverage::Opaque);
    CHECK(coverageOf("textures/fixture/cov_mirror") == Coverage::Opaque);

    // _currentRender makes a material translucent, unless it is sorted as a portal sky
    CHECK(coverageOf("textures/fixture/cov_current") == Coverage::Translucent);
    CHECK(coverageOf("textures/fixture/cov_portalsky") == Coverage::Opaque);
    CHECK(coverageOf("textures/fixture/cov_fragment") == Coverage::Translucent);

    CHECK(
      findShader(shaders, "textures/fixture/cov_alphatest").surfaceParms
      == kdl::flat_set<std::string>{"coverage_perforated"});

    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Culling")
  {
    const auto data = R"(
textures/fixture/cull_default	{ diffusemap textures/fixture/a }
textures/fixture/cull_two		{ twoSided diffusemap textures/fixture/a }
textures/fixture/cull_back		{ backSided diffusemap textures/fixture/a }
textures/fixture/cull_sprite	{ deform sprite diffusemap textures/fixture/a }
textures/fixture/cull_flare		{ deform flare 1.5 diffusemap textures/fixture/a }
textures/fixture/cull_last		{ twoSided backSided diffusemap textures/fixture/a }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 6);

    CHECK(findShader(shaders, "textures/fixture/cull_default").culling == Culling::Front);
    CHECK(findShader(shaders, "textures/fixture/cull_two").culling == Culling::None);
    CHECK(findShader(shaders, "textures/fixture/cull_back").culling == Culling::Back);
    CHECK(findShader(shaders, "textures/fixture/cull_sprite").culling == Culling::None);
    CHECK(findShader(shaders, "textures/fixture/cull_flare").culling == Culling::None);
    CHECK(findShader(shaders, "textures/fixture/cull_last").culling == Culling::Back);

    CHECK(
      findShader(shaders, "textures/fixture/cull_two").surfaceParms
      == kdl::flat_set<std::string>{"twosided", "coverage_opaque"});
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Unknown keywords default the material")
  {
    SECTION("Unknown keyword before the editor image")
    {
      const auto data = R"(
textures/fixture/before
{
	diffusemap	textures/fixture/before_d.tga
}

textures/fixture/unknown
{
	noshadows
	nomipmaps
	qer_editorimage	textures/fixture/unknown_ed.tga
	diffusemap		textures/fixture/unknown_d.tga
}

textures/fixture/after
{
	diffusemap	textures/fixture/after_d.tga
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 3);

      // parsing stopped at the unknown keyword, before the editor image
      CHECK(
        findShader(shaders, "textures/fixture/unknown")
        == defaultedShader("textures/fixture/unknown"));

      for (const auto& shaderPath : {"textures/fixture/before", "textures/fixture/after"})
      {
        CAPTURE(shaderPath);
        const auto& shader = findShader(shaders, shaderPath);
        CHECK_FALSE(shader.engineDefaulted);
        CHECK(shader.stages.size() == 1);
        CHECK(shader.previewImages.size() == 1);
      }

      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 10, column 2: Unknown keyword 'nomipmaps' in material "
          "'textures/fixture/unknown'; the engine replaces this material with its "
          "default material",
        }));
    }

    SECTION("Unknown keyword after the editor image and a diffuse map")
    {
      const auto data = R"(
textures/fixture/late
{
	qer_editorimage	textures/fixture/late_ed.tga
	diffusemap		textures/fixture/late_d.tga
	nodrop
	nonsolid
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 1);

      // the engine replaces a defaulted material with its default material and keeps
      // only the editor image name, so the parsed diffuse map is no preview image
      CHECK(
        shaders.front()
        == defaultedShader("textures/fixture/late", "textures/fixture/late_ed.tga"));
      CHECK(status.countStatus(LogLevel::Warn) == 1);
    }

    SECTION("Top-level flare")
    {
      const auto data = R"(
models/fixture/barrel_flare
{
	discrete
	flare	( 0 204 187 ) ( 8 8 ) ( 1 1 ) ( 8 8)
}

models/fixture/barrel
{
	diffusemap	models/fixture/barrel_d.tga
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 2);
      CHECK(findShader(shaders, "models/fixture/barrel_flare").engineDefaulted);
      CHECK_FALSE(findShader(shaders, "models/fixture/barrel").engineDefaulted);
      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 5, column 2: Unknown keyword 'flare' in material "
          "'models/fixture/barrel_flare'; the engine replaces this material with its "
          "default material",
        }));
    }

    SECTION("Unknown stage keyword")
    {
      const auto data = R"(
textures/fixture/stage_error
{
	diffusemap	textures/fixture/stage_error_d.tga
	{
		blend	add
		map		textures/fixture/stage_error_glow.tga
		nomipmaps
	}
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 1);

      CHECK(shaders.front() == defaultedShader("textures/fixture/stage_error"));
      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 8, column 3: Unknown stage keyword 'nomipmaps' in material "
          "'textures/fixture/stage_error'; the engine replaces this material with "
          "its default material",
        }));
    }

    SECTION("Bad expression term")
    {
      const auto data = R"(
textures/fixture/bad_term		{ { blend add map textures/fixture/a rgb time/2 } }
textures/fixture/good_term		{ { blend add map textures/fixture/a rgb time / 2 } }
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 2);

      // "time/2" is one name, and no table of that name exists
      CHECK(findShader(shaders, "textures/fixture/bad_term").engineDefaulted);
      CHECK_FALSE(findShader(shaders, "textures/fixture/good_term").engineDefaulted);
      CHECK(status.countStatus(LogLevel::Warn) == 1);
    }
  }

  SECTION("Broken file")
  {
    const auto shaders = parseFile(fixturePath / "broken.mtr", status);

    // b swallowed c, as in the engine
    CHECK(
      shaderPaths(shaders)
      == paths({"textures/fixture/a", "textures/fixture/b", "textures/fixture/d"}));

    const auto& a = findShader(shaders, "textures/fixture/a");
    CHECK_FALSE(a.engineDefaulted);
    CHECK(a.previewImages == paths({"textures/fixture/a_d.tga"}));

    // c's name is an unknown keyword inside b
    CHECK(
      findShader(shaders, "textures/fixture/b") == defaultedShader("textures/fixture/b"));

    // d hits the end of the file
    CHECK(
      findShader(shaders, "textures/fixture/d") == defaultedShader("textures/fixture/d"));

    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 16, column 1: Possible missing '}' before line 16: material "
        "'textures/fixture/b' now contains 'textures/fixture/c'",
        "At line 16, column 1: Unknown keyword 'textures/fixture/c' in material "
        "'textures/fixture/b'; the engine replaces this material with its default "
        "material",
        "At line 24, column 1: Material 'textures/fixture/d' has no matching '}' and "
        "runs to the end of the file",
      }));
  }

  SECTION("Stray tokens at file level")
  {
    const auto data = R"({
	diffusemap	textures/fixture/orphan_d.tga
}

textures/fixture/junk junk
{
	diffusemap	textures/fixture/junk_d.tga
}

textures/fixture/next
{
	diffusemap	textures/fixture/next_d.tga
}

table)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    CHECK(shaderPaths(shaders) == paths({"textures/fixture/next"}));
    CHECK_FALSE(shaders.front().engineDefaulted);

    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 1, column 1: Missing decl name; skipping this block",
        "At line 5, column 23: Expecting '{' but found 'junk'",
        "At line 6, column 1: Missing decl name; skipping this block",
        "At line 15, column 1: Type without definition at end of file",
      }));
  }

  SECTION("Names")
  {
    const auto data = R"(
Textures\Fixture\Mixed_Case.tga
{
	diffusemap	textures/fixture/a
}

textures/fixture/guide
{
	qer_editorimage	textures/fixture/guide.tga
}

models/fixture/crate		{ diffusemap models/fixture/crate_d.tga }
lights/fixture/round		{ { blend add map lights/fixture/round.tga } }
guis/fixture/screen			{ { blend blend map guis/fixture/screen.tga } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    CHECK(
      shaderPaths(shaders)
      == paths(
        {"textures/fixture/mixed_case",
         "textures/fixture/guide",
         "models/fixture/crate",
         "lights/fixture/round",
         "guis/fixture/screen"}));
    CHECK(shaders.front().shaderPath.generic_string() == "textures/fixture/mixed_case");
    CHECK(
      findShader(shaders, "textures/fixture/guide").previewImages
      == paths({"textures/fixture/guide.tga"}));
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Duplicate names in one file")
  {
    const auto data = R"(textures/fixture/twice
{
	diffusemap	textures/fixture/first_d.tga
}

Textures/Fixture/Twice.tga
{
	diffusemap	textures/fixture/second_d.tga
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK(shaders.front().previewImages == paths({"textures/fixture/first_d.tga"}));
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 6, column 1: Material 'Textures/Fixture/Twice.tga' was already "
        "defined at line 1; ignoring this definition",
      }));
  }

  SECTION("Nested stencil block")
  {
    const auto data = R"(
textures/fixture/stencil
{
	{
		blend	add
		stencil
		{
			Ref		1
			Comp	Equal
			Pass	Keep
		}
		map			textures/fixture/stencil_d.tga
		alphaTest	0.25
	}
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK_FALSE(shaders.front().engineDefaulted);
    CHECK(
      shaders.front().stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/fixture/stencil_d.tga",
         .blendFunc = {"GL_ONE", "GL_ONE"},
         .alphaFunc = gl::MaterialAlphaFunc{GreaterEqual, 0.25f},
         .lighting = Lighting::None},
      });
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Non-ASCII text")
  {
    // id Tech 4 names are ASCII, and quoted strings are the only carrier of other bytes;
    // a path built from them can throw (0x81 is unmapped in code page 1252)
    const auto data = std::string{
      "textures/fixture/before { diffusemap textures/fixture/before_d.tga }\n"
      "textures/fixture/editor\n"
      "{\n"
      "\tqer_editorimage \"textures/fixture/e\x81.tga\"\n"
      "\t{ blend diffusemap map \"textures/fixture/d\xC3\x89"
      "cole.tga\" }\n"
      "}\n"
      "\"textures/fixture/\xC3\x89"
      "cole\" { diffusemap textures/fixture/name_d.tga }\n"
      "textures/fixture/after { diffusemap textures/fixture/after_d.tga }\n"};

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    CHECK(
      shaderPaths(shaders)
      == paths(
        {"textures/fixture/before",
         "textures/fixture/editor",
         "textures/fixture/__cole",
         "textures/fixture/after"}));

    const auto& editor = findShader(shaders, "textures/fixture/editor");
    CHECK_FALSE(editor.engineDefaulted);
    CHECK(editor.editorImage == std::filesystem::path{"textures/fixture/e_.tga"});
    REQUIRE(editor.stages.size() == 1);
    CHECK(editor.stages[0].map == std::filesystem::path{"textures/fixture/d__cole.tga"});
    CHECK(
      editor.previewImages
      == paths({"textures/fixture/e_.tga", "textures/fixture/d__cole.tga"}));

    CHECK_FALSE(findShader(shaders, "textures/fixture/__cole").engineDefaulted);

    // one warning per material, naming the first field
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 4, column 18: Non-ASCII characters in the editor image of material "
        "'textures/fixture/editor' were replaced with '_'",
        "At line 7, column 1: Non-ASCII characters in the name of material "
        "'textures/fixture/__cole' were replaced with '_'",
      }));
  }

  SECTION("Deep nesting")
  {
    auto heightmaps = std::string{};
    for (auto i = 0; i < 1000; ++i)
    {
      heightmaps += "heightmap( ";
    }
    const auto parentheses = std::string(2000, '(');

    const auto data = "textures/fixture/deep_program { diffusemap " + heightmaps
                      + "textures/fixture/d }\n"
                        "textures/fixture/deep_expression { { blend add map "
                        "textures/fixture/a rgb "
                      + parentheses
                      + " } }\n"
                        "textures/fixture/after { diffusemap textures/fixture/after_d.tga }\n";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    CHECK(
      shaderPaths(shaders)
      == paths(
        {"textures/fixture/deep_program",
         "textures/fixture/deep_expression",
         "textures/fixture/after"}));
    CHECK(findShader(shaders, "textures/fixture/deep_program").engineDefaulted);
    CHECK(findShader(shaders, "textures/fixture/deep_expression").engineDefaulted);
    CHECK_FALSE(findShader(shaders, "textures/fixture/after").engineDefaulted);

    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 1, column 748: Image program nested too deeply (more than 64 levels) "
        "in material 'textures/fixture/deep_program'; the engine replaces this "
        "material with its default material",
        "At line 2, column 139: Expression nested too deeply (more than 64 levels) in "
        "material 'textures/fixture/deep_expression'; the engine replaces this "
        "material with its default material",
      }));
  }

  SECTION("Out-of-range parm index")
  {
    const auto data = R"(
textures/fixture/huge_parm		{ { program fixture.vfp vertexParm 99999999999999999999 1 } }
textures/fixture/after			{ diffusemap textures/fixture/after_d.tga }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 2);
    CHECK(findShader(shaders, "textures/fixture/huge_parm").engineDefaulted);
    CHECK_FALSE(findShader(shaders, "textures/fixture/after").engineDefaulted);
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 2, column 64: Bad vertexParm number '99999999999999999999' in "
        "material 'textures/fixture/huge_parm'; the engine replaces this material "
        "with its default material",
      }));
  }

  SECTION("Lexer")
  {
    const auto data = R"(
textures/fixture/lexer
{
	/* a block comment */ qer_editorimage "textures/fixture/quoted ed.tga" // trailing
	description "a " \
		"concatenated string"
	sort 1.5e1f
	{ blend add map textures/fixture/a-b }
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);

    const auto& lexer = shaders.front();
    CHECK(lexer.editorImage == std::filesystem::path{"textures/fixture/quoted ed.tga"});
    // '-' ends a name: the stage gets the image textures/fixture/a, and the engine
    // reads '-' and 'b' as unknown stage keywords
    CHECK(lexer.engineDefaulted);
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 8, column 36: Unknown stage keyword '-' in material "
        "'textures/fixture/lexer'; the engine replaces this material with its default "
        "material",
      }));
  }

  SECTION("Doom 3 BFG fixture tb_test.mtr")
  {
    const auto shaders = parseFile(
      getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG" / "base" / "materials"
        / "tb_test.mtr",
      status);

    CHECK(
      shaderPaths(shaders)
      == paths(
        {"textures/tb_test/wall",
         "textures/tb_test/wall_plain",
         "textures/tb_test/grate",
         "textures/tb_test/glass",
         "textures/tb_test/bumped",
         "textures/tb_test/dds",
         "textures/tb_test/nonsolid"}));

    for (const auto& shader : shaders)
    {
      CAPTURE(shader.shaderPath);
      CHECK(shader.format == Format::Doom3);
      CHECK_FALSE(shader.engineDefaulted);
    }

    CHECK(
      findShader(shaders, "textures/tb_test/wall").previewImages
      == paths({"textures/tb_test/wall_ed", "textures/tb_test/wall_d"}));
    CHECK(
      findShader(shaders, "textures/tb_test/wall_plain").previewImages
      == paths({"textures/tb_test/wall_d"}));

    const auto& grate = findShader(shaders, "textures/tb_test/grate");
    CHECK(grate.culling == Culling::None);
    CHECK(grate.coverage == Coverage::Perforated);
    CHECK(
      grate.stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/tb_test/grate_d",
         .blendFunc = {"", ""},
         .alphaFunc = gl::MaterialAlphaFunc{GreaterEqual, 0.5f},
         .lighting = Lighting::Diffuse},
      });
    CHECK(grate.previewImages == paths({"textures/tb_test/grate_d"}));

    const auto& glass = findShader(shaders, "textures/tb_test/glass");
    CHECK(glass.coverage == Coverage::Translucent);
    CHECK(
      glass.stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/tb_test/glass_d",
         .blendFunc = {"GL_SRC_ALPHA", "GL_ONE_MINUS_SRC_ALPHA"},
         .alphaFunc = {},
         .lighting = Lighting::None},
      });
    CHECK(glass.previewImages == paths({"textures/tb_test/glass_d"}));

    const auto& bumped = findShader(shaders, "textures/tb_test/bumped");
    CHECK(bumped.coverage == Coverage::Opaque);
    CHECK(
      bumped.stages
      == std::vector<Quake3ShaderStage>{
        {.map = "textures/tb_test/wall_d",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Diffuse},
        {.map = "textures/tb_test/bumped_local",
         .blendFunc = {"", ""},
         .alphaFunc = {},
         .lighting = Lighting::Bump},
        {.map = "textures/tb_test/wall_d",
         .blendFunc = {"GL_ONE", "GL_ONE"},
         .alphaFunc = {},
         .lighting = Lighting::None},
      });
    CHECK(
      bumped.previewImages
      == paths({"textures/tb_test/wall_ed", "textures/tb_test/wall_d"}));

    CHECK(
      findShader(shaders, "textures/tb_test/dds").previewImages
      == paths({"textures/tb_test/dds_d"}));

    const auto& nonsolid = findShader(shaders, "textures/tb_test/nonsolid");
    CHECK(
      nonsolid.surfaceParms == kdl::flat_set<std::string>{"nonsolid", "coverage_opaque"});
    CHECK(
      nonsolid.previewImages
      == paths({"textures/tb_test/wall_ed", "textures/tb_test/wall_d"}));

    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }
}

} // namespace tb::mdl
