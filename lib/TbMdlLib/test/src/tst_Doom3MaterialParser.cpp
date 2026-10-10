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
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

std::string readFile(const std::filesystem::path& path)
{
  auto file = fs::Disk::openFile(path) | kdl::value();
  auto reader = file->reader().buffer();
  return std::string{reader.stringView()};
}

std::vector<Quake3Shader> parseFile(
  const std::filesystem::path& path, TestParserStatus& status)
{
  return Doom3MaterialParser{readFile(path)}.parse(status);
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
    .referencedTables = {},
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
          .referencedTables = {},
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
          .referencedTables = {},
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
          .referencedTables = {},
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
          .referencedTables = {},
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
          .referencedTables = {},
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

  SECTION("Table references")
  {
    const auto data = R"(
textures/fixture/tables
{
	{
		blend	add
		map		textures/fixture/a
		rgb		Fixture_Table[ time ] * fixture_table[ parm0 ]
		alpha	fixture.other[ fixture_nested[ time ] ]
		rotate	time
	}
}

textures/fixture/defaulted_tables
{
	{
		blend	add
		map		textures/fixture/a
		rgb		fixture_table[ time ]
		nomipmaps
	}
}
)";

    auto parser = Doom3MaterialParser{data};
    const auto shaders = parser.parse(status);
    REQUIRE(shaders.size() == 2);

    // canonical decl names, each once, in the order of use
    CHECK(
      findShader(shaders, "textures/fixture/tables").referencedTables
      == std::vector<std::string>{
        "fixture_table", "fixture", "fixture_nested", "sintable", "costable"});
    CHECK_FALSE(findShader(shaders, "textures/fixture/tables").engineDefaulted);

    // the engine's default material looks up no tables
    CHECK(
      findShader(shaders, "textures/fixture/defaulted_tables").referencedTables.empty());

    CHECK(parser.declaredTables().empty());
  }

  SECTION("makeEngineDefaulted")
  {
    auto shaders = Doom3MaterialParser{R"(
textures/fixture/fine
{
	qer_editorimage	textures/fixture/fine_ed.tga
	twoSided
	{
		blend	add
		map		textures/fixture/a
		rgb		fixture_missing[ time ]
	}
}
)"}
                     .parse(status);
    REQUIRE(shaders.size() == 1);

    // what package 02b does when no file declares fixture_missing
    makeEngineDefaulted(shaders.front());
    CHECK(
      shaders.front()
      == defaultedShader("textures/fixture/fine", "textures/fixture/fine_ed.tga"));
  }

  SECTION("Tables and other typed decls")
  {
    const auto text = readFile(fixturePath / "typed_decls.mtr");
    auto parser = Doom3MaterialParser{text};
    const auto shaders = parser.parse(status);
    CHECK(
      parser.declaredTables()
      == std::vector<std::string>{"fixture_steps", "fixture_snap", "fixture_wave"});
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
textures/fixture/cov_opaque_last	{ translucent forceOpaque }
textures/fixture/cov_translucent_last	{ forceOpaque translucent }
textures/fixture/cov_current		{ forceOpaque { blend filter map _currentRender } }
textures/fixture/cov_portalsky		{ forceOpaque sort portalSky
	{ blend filter map _currentRender } }
textures/fixture/cov_fragment		{ { program fixture.vfp fragmentMap 0 _currentRender } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 17);

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
    CHECK(coverageOf("textures/fixture/cov_threshold") == Coverage::Perforated);
    CHECK(
      alphaFuncOf("textures/fixture/cov_threshold")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.25f});
    // a threshold that is not one number reads as 0.5
    CHECK(coverageOf("textures/fixture/cov_parm") == Coverage::Perforated);
    CHECK(
      alphaFuncOf("textures/fixture/cov_parm")
      == gl::MaterialAlphaFunc{GreaterEqual, 0.5f});
    CHECK(coverageOf("textures/fixture/cov_expression") == Coverage::Perforated);
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
    CHECK(coverageOf("textures/fixture/cov_opaque_last") == Coverage::Opaque);
    CHECK(coverageOf("textures/fixture/cov_translucent_last") == Coverage::Translucent);

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
textures/fixture/cull_last_two	{ backSided twoSided diffusemap textures/fixture/a }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 7);

    CHECK(findShader(shaders, "textures/fixture/cull_default").culling == Culling::Front);
    CHECK(findShader(shaders, "textures/fixture/cull_two").culling == Culling::None);
    CHECK(findShader(shaders, "textures/fixture/cull_back").culling == Culling::Back);
    CHECK(findShader(shaders, "textures/fixture/cull_sprite").culling == Culling::None);
    CHECK(findShader(shaders, "textures/fixture/cull_flare").culling == Culling::None);
    // the last one wins
    CHECK(findShader(shaders, "textures/fixture/cull_last").culling == Culling::Back);
    CHECK(findShader(shaders, "textures/fixture/cull_last_two").culling == Culling::None);

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

      const auto& before = findShader(shaders, "textures/fixture/before");
      CHECK_FALSE(before.engineDefaulted);
      CHECK(before.previewImages == paths({"textures/fixture/before_d.tga"}));
      const auto& after = findShader(shaders, "textures/fixture/after");
      CHECK_FALSE(after.engineDefaulted);
      CHECK(after.previewImages == paths({"textures/fixture/after_d.tga"}));

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
      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 6, column 2: Unknown keyword 'nodrop' in material "
          "'textures/fixture/late'; the engine replaces this material with its "
          "default material",
        }));
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
      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 2, column 69: Bad term 'time/2' in an expression in material "
          "'textures/fixture/bad_term'; the engine replaces this material with its "
          "default material",
        }));
    }

    SECTION("surfaceparm is no keyword")
    {
      const auto data = R"(
textures/fixture/quake3_style
{
	surfaceparm nonsolid
	diffusemap	textures/fixture/d.tga
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 1);

      // surface flags are bare keywords in this engine
      CHECK(shaders.front() == defaultedShader("textures/fixture/quake3_style"));
      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 4, column 2: Unknown keyword 'surfaceparm' in material "
          "'textures/fixture/quake3_style'; the engine replaces this material with "
          "its default material",
        }));
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
    REQUIRE(!shaders.empty());
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

textures/phook/guide
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
         "textures/phook/guide",
         "models/fixture/crate",
         "lights/fixture/round",
         "guis/fixture/screen"}));
    REQUIRE(!shaders.empty());
    CHECK(shaders.front().shaderPath.generic_string() == "textures/fixture/mixed_case");
    // there is no guide decl in this engine
    CHECK(
      findShader(shaders, "textures/phook/guide").previewImages
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

  SECTION("Comments")
  {
    const auto data = R"(
textures/fixture/comments
{
	/* a block comment */ qer_editorimage textures/fixture/comments_ed.tga // a comment
	/* a block comment
	   over two lines */ diffusemap textures/fixture/comments_d.tga
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK_FALSE(shaders.front().engineDefaulted);
    CHECK(
      shaders.front().previewImages
      == paths({"textures/fixture/comments_ed.tga", "textures/fixture/comments_d.tga"}));
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Quoted strings")
  {
    const auto data = R"(
textures/fixture/quoted { qer_editorimage "textures/fixture/quoted ed.tga"
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK(
      shaders.front().editorImage
      == std::filesystem::path{"textures/fixture/quoted ed.tga"});
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Backslash string concatenation")
  {
    const auto data = R"(
textures/fixture/concat
{
	qer_editorimage "textures/fixture/a" \ "_ed.tga"
	description "a " \
		"concatenated string"
	diffusemap textures/fixture/d
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK(
      shaders.front().editorImage == std::filesystem::path{"textures/fixture/a_ed.tga"});
    // the keyword after a string continued on the next line is still read
    CHECK(
      shaders.front().previewImages
      == paths({"textures/fixture/a_ed.tga", "textures/fixture/d"}));
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("No escapes in strings")
  {
    const auto data = R"(
textures/fixture/escapes
{
	description "ends with a backslash \"
	qer_editorimage "textures\fixture\back.tga"
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK_FALSE(shaders.front().engineDefaulted);
    CHECK(
      shaders.front().editorImage == std::filesystem::path{"textures/fixture/back.tga"});
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Minus ends a name")
  {
    const auto data = R"(
textures/fixture/minus
{
	{ blend add map textures/fixture/a-b }
}
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);

    // the stage's image is textures/fixture/a, and '-' is an unknown stage keyword
    CHECK(shaders.front() == defaultedShader("textures/fixture/minus"));
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 4, column 36: Unknown stage keyword '-' in material "
        "'textures/fixture/minus'; the engine replaces this material with its default "
        "material",
      }));
  }

  SECTION("Numbers")
  {
    const auto data = R"(
textures/fixture/num_dot_first	{ { blend diffusemap map textures/fixture/a alphaTest .5 } }
textures/fixture/num_dot_last	{ { blend diffusemap map textures/fixture/a alphaTest 5. } }
textures/fixture/num_negative	{ { blend diffusemap map textures/fixture/a alphaTest -1 } }
textures/fixture/num_hex		{ { blend diffusemap map textures/fixture/a alphaTest 0x10 } }
textures/fixture/num_octal		{ { blend diffusemap map textures/fixture/a alphaTest 010 } }
textures/fixture/num_exponent	{ { blend diffusemap map textures/fixture/a alphaTest 1.5e-1f } }
textures/fixture/num_signed		{ { blend add map textures/fixture/a scale -5, 5. } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 7);

    const auto thresholdOf = [&](const auto& shaderPath) {
      const auto& shader = findShader(shaders, shaderPath);
      REQUIRE(shader.stages.size() == 1);
      REQUIRE(shader.stages.front().alphaFunc);
      return shader.stages.front().alphaFunc->threshold;
    };

    CHECK(thresholdOf("textures/fixture/num_dot_first") == 0.5f);
    CHECK(thresholdOf("textures/fixture/num_dot_last") == 5.0f);
    CHECK(thresholdOf("textures/fixture/num_negative") == -1.0f);
    // idLexer reads hexadecimal and octal integers
    CHECK(thresholdOf("textures/fixture/num_hex") == 16.0f);
    CHECK(thresholdOf("textures/fixture/num_octal") == 8.0f);
    CHECK(thresholdOf("textures/fixture/num_exponent") == 0.15f);
    CHECK_FALSE(findShader(shaders, "textures/fixture/num_signed").engineDefaulted);
    CHECK(status.countStatus(LogLevel::Warn) == 0);
  }

  SECTION("Blend without a comma")
  {
    const auto data = R"(
textures/fixture/no_comma	{ { blend GL_ONE GL_ONE map textures/fixture/a } }
)";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK(shaders.front() == defaultedShader("textures/fixture/no_comma"));
    CHECK_THAT(
      status.messages(LogLevel::Warn),
      Catch::Matchers::Equals(std::vector<std::string>{
        "At line 2, column 44: Expected ',' but found 'GL_ONE' in material "
        "'textures/fixture/no_comma'; the engine replaces this material with its "
        "default material",
      }));
  }

  SECTION("Missing closing brace")
  {
    SECTION("A material without its closing brace swallows the rest of the file")
    {
      const auto data = R"(
textures/fixture/a
{
	diffusemap	textures/fixture/a_d.tga
}

textures/fixture/b
{
	diffusemap	textures/fixture/b_d.tga

textures/fixture/c
{
	diffusemap	textures/fixture/c_d.tga
}

textures/fixture/d
{
	diffusemap	textures/fixture/d_d.tga
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      CHECK(shaderPaths(shaders) == paths({"textures/fixture/a", "textures/fixture/b"}));
      CHECK(
        findShader(shaders, "textures/fixture/a").previewImages
        == paths({"textures/fixture/a_d.tga"}));
      CHECK(
        findShader(shaders, "textures/fixture/b")
        == defaultedShader("textures/fixture/b"));

      CHECK_THAT(
        status.messages(LogLevel::Warn),
        Catch::Matchers::Equals(std::vector<std::string>{
          "At line 7, column 1: Material 'textures/fixture/b' has no matching '}' and "
          "runs to the end of the file",
          "At line 11, column 1: Possible missing '}' before line 11: material "
          "'textures/fixture/b' now contains 'textures/fixture/c'",
          "At line 16, column 1: Possible missing '}' before line 16: material "
          "'textures/fixture/b' now contains 'textures/fixture/d'",
          "At line 11, column 1: Unknown keyword 'textures/fixture/c' in material "
          "'textures/fixture/b'; the engine replaces this material with its default "
          "material",
        }));
    }

    SECTION("An image name on its own line is no missing brace")
    {
      const auto data = R"(
textures/fixture/argument
{
	diffusemap
		textures/fixture/d.tga
	{ blend add map textures/fixture/g }
}
)";

      const auto shaders = Doom3MaterialParser{data}.parse(status);
      REQUIRE(shaders.size() == 1);
      CHECK_FALSE(shaders.front().engineDefaulted);
      CHECK(shaders.front().stages.size() == 2);
      CHECK(status.countStatus(LogLevel::Warn) == 0);
    }
  }

  SECTION("Truncated input never throws")
  {
    // every prefix of every fixture, cut at every byte
    for (const auto& path : {
           fixturePath / "broken.mtr",
           fixturePath / "tool_materials.mtr",
           fixturePath / "typed_decls.mtr",
           getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG" / "base" / "materials"
             / "tb_test.mtr",
         })
    {
      CAPTURE(path);

      // one assertion per file keeps this fast enough for a debug build
      const auto text = readFile(path);
      auto throwingLengths = std::vector<size_t>{};
      for (size_t length = 0; length <= text.size(); ++length)
      {
        auto prefixStatus = TestParserStatus{};
        try
        {
          Doom3MaterialParser{std::string_view{text}.substr(0, length)}.parse(
            prefixStatus);
        }
        catch (...)
        {
          throwingLengths.push_back(length);
        }
      }
      CHECK(throwingLengths.empty());
    }
  }

  SECTION("Keyword arities")
  {
    using T = std::tuple<bool, std::string>;
    constexpr auto Material = false;
    constexpr auto Stage = true;

    // clang-format off
    const auto [inStage, row] = GENERATE(values<T>({
      {Material, "qer_editorimage textures/fixture/ed.tga"},
      {Material, "qer_editorimage textures/fixture/ed.tga the rest of the line"},
      {Material, "description \"a fixture\""},
      {Material, "solid"}, {Material, "water"}, {Material, "playerclip"},
      {Material, "monsterclip"}, {Material, "moveableclip"}, {Material, "ikclip"},
      {Material, "blood"}, {Material, "trigger"}, {Material, "aassolid"},
      {Material, "aasobstacle"}, {Material, "flashlight_trigger"}, {Material, "nonsolid"},
      {Material, "nullNormal"}, {Material, "areaportal"}, {Material, "qer_nocarve"},
      {Material, "discrete"}, {Material, "noFragment"}, {Material, "occlusion"},
      {Material, "slick"}, {Material, "collision"}, {Material, "noimpact"},
      {Material, "nodamage"}, {Material, "ladder"}, {Material, "nosteps"},
      {Material, "metal"}, {Material, "stone"}, {Material, "flesh"}, {Material, "wood"},
      {Material, "cardboard"}, {Material, "liquid"}, {Material, "glass"},
      {Material, "plastic"}, {Material, "ricochet"}, {Material, "surftype10"},
      {Material, "surftype11"}, {Material, "surftype12"}, {Material, "surftype13"},
      {Material, "surftype14"}, {Material, "surftype15"},
      {Material, "polygonOffset"}, {Material, "polygonOffset 2"},
      {Material, "noShadows"}, {Material, "suppressInSubview"}, {Material, "portalSky"},
      {Material, "noSelfShadow"}, {Material, "noPortalFog"}, {Material, "forceShadows"},
      {Material, "noOverlays"}, {Material, "forceOverlays"}, {Material, "translucent"},
      {Material, "zeroclamp"}, {Material, "clamp"}, {Material, "alphazeroclamp"},
      {Material, "forceOpaque"}, {Material, "twoSided"}, {Material, "backSided"},
      {Material, "fogLight"}, {Material, "blendLight"}, {Material, "ambientLight"},
      {Material, "mirror"}, {Material, "directPortal"}, {Material, "noFog"},
      {Material, "unsmoothedTangents"}, {Material, "origin"}, {Material, "mikktspace"},
      {Material, "unlit"}, {Material, "DECAL_MACRO"}, {Material, "lod1"},
      {Material, "lod2"}, {Material, "lod3"}, {Material, "lod4"},
      {Material, "persistentLOD"},
      {Material, "lightFalloffImage makeIntensity( lights/fixture/falloff.tga )"},
      {Material, "guisurf entity"}, {Material, "guisurf entity2"},
      {Material, "guisurf entity3"}, {Material, "guisurf guis/fixture/screen.gui"},
      {Material, "sort subview"}, {Material, "sort opaque"}, {Material, "sort decal"},
      {Material, "sort far"}, {Material, "sort medium"}, {Material, "sort close"},
      {Material, "sort almostNearest"}, {Material, "sort nearest"},
      {Material, "sort postProcess"}, {Material, "sort portalSky"}, {Material, "sort 1.5"},
      {Material, "stereoeye left"}, {Material, "stereoeye right"},
      {Material, "spectrum 1"},
      {Material, "deform sprite"}, {Material, "deform tube"},
      {Material, "deform flare 1.5"}, {Material, "deform expand 0.5 * time"},
      {Material, "deform move time"},
      {Material, "deform turbulent fixture_table 0.0175 ( time * 0.15 ) 10"},
      {Material, "deform eyeBall"}, {Material, "deform particle fixture_particle"},
      {Material, "deform particle2 fixture_particle"},
      {Material, "decalInfo 10 2 ( 1 1 1 1 ) ( 0 -1 0 0 )"},
      {Material, "renderbump -size 256 256 -aa 2 models/fixture/hi.lwo"},
      {Material, "diffusemap textures/fixture/x"}, {Material, "basecolormap textures/fixture/x"},
      {Material, "specularmap textures/fixture/x"}, {Material, "rmaomap textures/fixture/x"},
      {Material, "reflectionmap textures/fixture/x"}, {Material, "pbrmap textures/fixture/x"},
      {Material, "bumpmap textures/fixture/x"}, {Material, "normalmap textures/fixture/x"},
      {Material, "{ blend add map textures/fixture/x }"},
      {Stage, "name a fixture stage"},
      {Stage, "blend add"}, {Stage, "blend blend"}, {Stage, "blend filter"},
      {Stage, "blend modulate"}, {Stage, "blend none"}, {Stage, "blend bumpmap"},
      {Stage, "blend normalmap"}, {Stage, "blend diffusemap"}, {Stage, "blend basecolormap"},
      {Stage, "blend specularmap"}, {Stage, "blend rmaomap"},
      {Stage, "blend GL_ONE, GL_ZERO"}, {Stage, "blend gl_dst_alpha, gl_one_minus_dst_alpha"},
      {Stage, "blend GL_SRC_ALPHA_SATURATE, GL_ONE"},
      {Stage, "map textures/fixture/x"}, {Stage, "map _white"},
      {Stage, "remoteRenderMap 256 256"}, {Stage, "mirrorRenderMap 256 256"},
      {Stage, "xrayRenderMap 256 256"}, {Stage, "guiRenderMap 256 256"},
      {Stage, "screen"}, {Stage, "screen2"}, {Stage, "glassWarp"},
      {Stage, "videomap video/fixture.roq"}, {Stage, "videoMap loop video/fixture.roq"},
      {Stage, "soundmap waveform"}, {Stage, "soundMap waveform"},
      {Stage, "cubeMap env/fixture"}, {Stage, "cubeMapSingle env/fixture"},
      {Stage, "panoramaMap env/fixture"}, {Stage, "hdriMap env/fixture"},
      {Stage, "cameraCubeMap env/fixture"}, {Stage, "quakeCubeMap env/fixture"},
      {Stage, "cubeMapSize 256"},
      {Stage, "ignoreAlphaTest"}, {Stage, "nearest"}, {Stage, "linear"}, {Stage, "clamp"},
      {Stage, "noclamp"}, {Stage, "zeroclamp"}, {Stage, "alphazeroclamp"},
      {Stage, "forceHighQuality"}, {Stage, "highquality"}, {Stage, "uncompressedCubeMap"},
      {Stage, "nopicmip"}, {Stage, "vertexColor"}, {Stage, "inverseVertexColor"},
      {Stage, "maskRed"}, {Stage, "maskGreen"}, {Stage, "maskBlue"}, {Stage, "maskAlpha"},
      {Stage, "maskColor"}, {Stage, "maskDepth"}, {Stage, "colored"},
      {Stage, "privatePolygonOffset"}, {Stage, "privatePolygonOffset 2"},
      {Stage, "privatePolygonOffset -1"},
      {Stage, "texGen normal"}, {Stage, "texGen reflect"}, {Stage, "texGen reflect2"},
      {Stage, "texGen skybox"}, {Stage, "texGen wobbleSky 0.1 0.2 time"},
      {Stage, "scroll time * 0.1, 0"}, {Stage, "translate 0.5, -0.5"},
      {Stage, "scale 2, 2"}, {Stage, "centerScale 2, 2"}, {Stage, "shear 0.1, -0.1"},
      {Stage, "rotate time * 0.1"},
      {Stage, "alphaTest 0.5"},
      {Stage, "color 1, 0.5, 0.25, 1"},
      {Stage, "red parm0"}, {Stage, "green parm1"}, {Stage, "blue parm2"},
      {Stage, "alpha parm3"}, {Stage, "rgb fixture_table[ time ]"}, {Stage, "rgba 0.5"},
      {Stage, "rgb sound * 2"}, {Stage, "rgb global0 + fragmentPrograms"},
      {Stage, "if parm7 == 1"}, {Stage, "if ( time > 1 ) && ( parm7 != 0 )"},
      {Stage, "program fixture.vfp"}, {Stage, "fragmentProgram fixture.vfp"},
      {Stage, "vertexProgram fixture.vfp"},
      {Stage, "vertexParm 0 1"}, {Stage, "vertexParm 1 time, 2"},
      {Stage, "vertexParm 2 1, 2, 3"}, {Stage, "vertexParm 3 1, 2, 3, 4"},
      {Stage, "vertexParm2 0 1, 2, 3, 4"},
      {Stage, "fragmentMap 0 textures/fixture/x"},
      {Stage, "fragmentMap 1 cubeMap nearest clamp env/fixture"},
      {Stage, "fragmentMap 7 _currentRender"},
      {Stage, "stencil { Ref 1 Comp Always Pass Replace Fail Keep ZFail Keep }"},
    }));
    // clang-format on

    CAPTURE(inStage, row);

    const auto data = inStage ? "textures/fixture/keyword\n{\n\t{\n\t\t" + row
                                  + "\n\t}\n\tdiffusemap textures/fixture/d\n}\n"
                              : "textures/fixture/keyword\n{\n\t" + row
                                  + "\n\tdiffusemap textures/fixture/d\n}\n";

    const auto shaders = Doom3MaterialParser{data}.parse(status);
    REQUIRE(shaders.size() == 1);
    CHECK_FALSE(shaders.front().engineDefaulted);
    CHECK(status.countStatus(LogLevel::Warn) == 0);

    // the keyword consumed exactly its arguments
    CHECK(std::ranges::any_of(shaders.front().stages, [](const auto& stage) {
      return stage.lighting == Lighting::Diffuse
             && stage.map == std::filesystem::path{"textures/fixture/d"};
    }));
  }

  SECTION("CRLF line endings")
  {
    // a warning in the middle of a line, then the broken fixture
    auto lf =
      "textures/fixture/early { { blend add map textures/fixture/a nomipmaps } }\n"
      + readFile(fixturePath / "broken.mtr");
    std::erase(lf, '\r');
    auto crlf = std::string{};
    for (const auto c : lf)
    {
      if (c == '\n')
      {
        crlf += '\r';
      }
      crlf += c;
    }

    auto lfStatus = TestParserStatus{};
    const auto lfShaders = Doom3MaterialParser{lf}.parse(lfStatus);
    const auto crlfShaders = Doom3MaterialParser{crlf}.parse(status);

    CHECK(crlfShaders == lfShaders);
    CHECK(status.messages(LogLevel::Warn) == lfStatus.messages(LogLevel::Warn));
    CHECK(status.countStatus(LogLevel::Warn) == 4);
  }

  SECTION("Doom 3 BFG fixture tb_test.mtr")
  {
    const auto text = readFile(
      getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG" / "base" / "materials"
      / "tb_test.mtr");
    auto parser = Doom3MaterialParser{text};
    const auto shaders = parser.parse(status);
    CHECK(parser.declaredTables() == std::vector<std::string>{"tb_test_pulse"});

    // another package owns this fixture, so materials are looked up by name
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
    CHECK(bumped.referencedTables == std::vector<std::string>{"tb_test_pulse"});
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
