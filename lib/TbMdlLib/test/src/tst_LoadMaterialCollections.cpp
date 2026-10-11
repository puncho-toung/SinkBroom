/*
 Copyright (C) 2024 Kristian Duske

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
#include "fs/TestUtils.h"
#include "fs/VirtualFileSystem.h"
#include "fs/WadFileSystem.h"
#include "gl/MaterialCollection.h"
#include "gl/Resource.h"
#include "gl/Texture.h"
#include "mdl/CatchConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/LoadMaterialCollections.h"
#include "mdl/LoadShaders.h"
#include "mdl/Quake3Shader.h"
#include "mdl/TestUtils.h"

#include "kd/ranges/to.h"
#include "kd/reflection_impl.h"
#include "kd/task_manager.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

namespace tb::mdl
{

namespace
{

struct MaterialInfo
{
  std::string name;
  size_t width;
  size_t height;

  kdl_reflect_inline(MaterialInfo, name, width, height);
};

struct MaterialCollectionInfo
{
  std::filesystem::path path;
  std::vector<std::optional<MaterialInfo>> materials;

  kdl_reflect_inline(MaterialCollectionInfo, path, materials);
};

MaterialCollectionInfo makeMaterialCollectionInfo(
  const gl::MaterialCollection& materialCollection)
{
  const auto toMaterialInfo = [](const auto& material) -> std::optional<MaterialInfo> {
    if (const auto* texture = material.texture())
    {
      return MaterialInfo{
        material.name(),
        material.texture()->width(),
        material.texture()->height(),
      };
    }
    return std::nullopt;
  };

  return MaterialCollectionInfo{
    materialCollection.path(),
    materialCollection.materials() | std::views::transform(toMaterialInfo)
      | kdl::ranges::to<std::vector>()};
}

class MaterialCollectionsMatcher
  : public Catch::Matchers::MatcherBase<Result<std::vector<gl::MaterialCollection>>>
{
private:
  std::vector<MaterialCollectionInfo> m_expected;

public:
  explicit MaterialCollectionsMatcher(std::vector<MaterialCollectionInfo> expected)
    : m_expected{std::move(expected)}
  {
  }

  bool match(const Result<std::vector<gl::MaterialCollection>>& result) const override
  {
    return result | kdl::transform([&](const auto& materialCollections) {
             return std::ranges::equal(
               materialCollections | std::views::transform(makeMaterialCollectionInfo),
               m_expected);
           })
           | kdl::transform_error([](auto) { return false; }) | kdl::value();
  }

  std::string describe() const override
  {
    auto str = std::stringstream{};
    str << "\nmatches\n" << kdl::make_streamable(m_expected);
    return str.str();
  }
};

auto MatchesMaterialCollections(std::vector<MaterialCollectionInfo> expected)
{
  return MaterialCollectionsMatcher{std::move(expected)};
}

auto createResource(gl::ResourceLoader<gl::Texture> resourceLoader)
{
  auto resource = std::make_shared<gl::TextureResource>(std::move(resourceLoader));
  resource->loadSync();
  return resource;
}

// Keeps every message; shader files are parsed and log on task threads.
class CollectingLogger : public Logger
{
private:
  mutable std::mutex m_mutex;
  std::vector<std::string> m_messages;

public:
  size_t countMessagesContaining(const std::string_view text) const
  {
    const auto lock = std::lock_guard{m_mutex};
    return size_t(std::ranges::count_if(m_messages, [&](const auto& message) {
      return message.find(text) != std::string::npos;
    }));
  }

private:
  void doLog(const LogLevel, const std::string_view message) override
  {
    const auto lock = std::lock_guard{m_mutex};
    m_messages.emplace_back(message);
  }
};

const gl::Material* findMaterial(
  const std::vector<gl::MaterialCollection>& materialCollections, const std::string& name)
{
  for (const auto& materialCollection : materialCollections)
  {
    if (const auto iMaterial =
          std::ranges::find(materialCollection.materials(), name, &gl::Material::name);
        iMaterial != materialCollection.materials().end())
    {
      return &*iMaterial;
    }
  }
  return nullptr;
}

const Quake3Shader* findShader(
  const std::vector<Quake3Shader>& shaders, const std::filesystem::path& shaderPath)
{
  const auto iShader = std::ranges::find(shaders, shaderPath, &Quake3Shader::shaderPath);
  return iShader != shaders.end() ? &*iShader : nullptr;
}

const auto Doom3FixtureMaterialConfig = MaterialConfig{
  .root = "textures",
  .extensions = {".tga", ".png", ".jpg", ".dds"},
  .palette = "",
  .property = std::nullopt,
  .shaderSearchPath = "materials",
  .excludes = {"*_d", "*_ed", "*_add"},
  .shaderExtensions = {".mtr"},
  .shaderImageSearchPaths = {"_tb"},
};

} // namespace

TEST_CASE("LoadMaterialCollections")
{
  SECTION("loadMaterial")
  {
    auto fs = fs::VirtualFileSystem{};
    auto logger = NullLogger{};

    const auto fixtureRoot = getFixtureRoot();

    auto taskManager = kdl::task_manager{};

    const auto testDir = fixtureRoot / "test/io/LoadMaterial/files";
    fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

    const auto materialConfig = mdl::MaterialConfig{
      .root = "textures",
      .extensions = {".png", ".jpg"},
      .palette = "",
      .property = std::nullopt,
      .shaderSearchPath = "scripts",
      .excludes = {},
    };

    CHECK(
      loadMaterial(fs, materialConfig, "material.jpg", createResource, {}, std::nullopt));

    SECTION("find alternative file extensions")
    {
      CHECK(loadMaterial(
        fs, materialConfig, "material.png", createResource, {}, std::nullopt));
    }

    SECTION(
      "a `{`-prefixed name on a MIP texture extension eagerly sets "
      "Material::effectiveAlphaFunc, without resolving the texture resource")
    {
      const auto mipMaterialConfig = mdl::MaterialConfig{
        .root = "textures",
        .extensions = {".D"},
        .palette = "",
        .property = std::nullopt,
        .shaderSearchPath = "scripts",
        .excludes = {},
      };

      auto lazyCreateResource = [](gl::ResourceLoader<gl::Texture> resourceLoader) {
        return std::make_shared<gl::TextureResource>(std::move(resourceLoader));
      };

      const auto material = loadMaterial(
                              fs,
                              mipMaterialConfig,
                              "textures/{fence.D",
                              lazyCreateResource,
                              {},
                              std::nullopt)
                            | kdl::value();

      // the resource must still be unresolved, or the check below could also be
      // satisfied by Material::effectiveAlphaFunc()'s ImageAlphaDomain fallback
      CHECK(material.textureResource().get() == nullptr);
      CHECK(
        material.effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.5f});
    }

    SECTION("a `{`-prefixed name on a non-MIP texture extension is not masked")
    {
      auto lazyCreateResource = [](gl::ResourceLoader<gl::Texture> resourceLoader) {
        return std::make_shared<gl::TextureResource>(std::move(resourceLoader));
      };

      const auto material = loadMaterial(
                              fs,
                              materialConfig,
                              "textures/{material.jpg",
                              lazyCreateResource,
                              {},
                              std::nullopt)
                            | kdl::value();

      CHECK(!material.effectiveAlphaFunc());
    }

    SECTION("a Quake 3 shader stage's alphaFunc sets Material::effectiveAlphaFunc")
    {
      auto shader = Quake3Shader{};
      shader.shaderPath = "material";
      shader.editorImage = "material";
      shader.addStage().alphaFunc =
        gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::Greater, 0.0f};

      const auto material =
        loadMaterial(
          fs, materialConfig, "material", createResource, {shader}, std::nullopt)
        | kdl::value();
      CHECK(
        material.effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::Greater, 0.0f});
    }

    SECTION("a Quake 3 shader's qer_alphaFunc alone sets Material::effectiveAlphaFunc")
    {
      auto shader = Quake3Shader{};
      shader.shaderPath = "material";
      shader.editorImage = "material";
      shader.qerAlphaFunc =
        gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.25f};

      const auto material =
        loadMaterial(
          fs, materialConfig, "material", createResource, {shader}, std::nullopt)
        | kdl::value();
      CHECK(
        material.effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.25f});
    }

    SECTION("a Doom 3 material is found by its canonical name, whatever its root")
    {
      const auto doom3Dir = fixtureRoot / "test/mdl/LoadMaterialCollections/doom3";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(doom3Dir / "fallback"));
      fs.mount("", std::make_unique<fs::DiskFileSystem>(doom3Dir));

      const auto shaders =
        loadShaders(fs, Doom3FixtureMaterialConfig, taskManager, logger) | kdl::value();

      // entity models name their materials like this; the case is ignored
      for (const auto* name : {"models/test/crate", "Models/Test/Crate"})
      {
        CAPTURE(name);

        const auto material =
          loadMaterial(
            fs, Doom3FixtureMaterialConfig, name, createResource, shaders, std::nullopt)
          | kdl::value();
        REQUIRE(material.texture() != nullptr);
        CHECK(material.texture()->width() == 8);
        CHECK(material.texture()->height() == 24);
        CHECK_FALSE(material.previewImageMissing());
      }
    }

    SECTION("a stage's alphaFunc wins over the shader's qer_alphaFunc")
    {
      auto shader = Quake3Shader{};
      shader.shaderPath = "material";
      shader.editorImage = "material";
      shader.qerAlphaFunc =
        gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.25f};
      shader.addStage().alphaFunc =
        gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::Less, 0.5f};

      const auto material =
        loadMaterial(
          fs, materialConfig, "material", createResource, {shader}, std::nullopt)
        | kdl::value();
      CHECK(
        material.effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::Less, 0.5f});
    }
  }

  SECTION("loadMaterialCollections")
  {
    auto fs = fs::VirtualFileSystem{};
    auto logger = NullLogger{};

    const auto fixtureRoot = getFixtureRoot();

    auto taskManager = kdl::task_manager{};

    SECTION("WAD file")
    {
      const auto wadPath =
        fixtureRoot / "test/mdl/LoadMaterialCollections/wads/cr8_czg.wad";
      fs.mount(
        "", std::make_unique<fs::DiskFileSystem>(fixtureRoot)); // to find the palette
      fs.mount("textures", fs::openFS<fs::WadFileSystem>(wadPath));

      const auto materialConfig = mdl::MaterialConfig{
        .root = "textures",
        .extensions = {".D"},
        .palette = "test/mdl/LoadMaterialCollections/palette.lmp",
        .property = "wad",
        .shaderSearchPath = "",
        .excludes = {},
      };

      CHECK_THAT(
        loadMaterialCollections(fs, materialConfig, createResource, taskManager, logger),
        MatchesMaterialCollections({
          {
            "cr8_czg.wad",
            {
              MaterialInfo{"blowjob_machine", 128, 128},
              MaterialInfo{"bongs2", 128, 128},
              MaterialInfo{"can-o-jam", 64, 64},
              MaterialInfo{"cap4can-o-jam", 64, 64},
              MaterialInfo{"coffin1", 128, 128},
              MaterialInfo{"coffin2", 128, 128},
              MaterialInfo{"cr8_czg_1", 64, 64},
              MaterialInfo{"cr8_czg_2", 64, 64},
              MaterialInfo{"cr8_czg_3", 64, 128},
              MaterialInfo{"cr8_czg_4", 64, 128},
              MaterialInfo{"cr8_czg_5", 64, 128},
              MaterialInfo{"crackpipes", 128, 128},
              MaterialInfo{"czg_backhole", 128, 128},
              MaterialInfo{"czg_fronthole", 128, 128},
              MaterialInfo{"dex_5", 128, 128},
              MaterialInfo{"eat_me", 64, 64},
              MaterialInfo{"for_sux-m-ass", 64, 64},
              MaterialInfo{"lasthopeofhuman", 128, 128},
              MaterialInfo{"polished_turd", 64, 64},
              MaterialInfo{"speedM_1", 128, 128},
              MaterialInfo{"u_get_this", 64, 64},
            },
          },
        }));

      SECTION("Multiple WAD files with name conflicts")
      {
        const auto additionalWadPath =
          fixtureRoot / "test/mdl/LoadMaterialCollections/wads/cr8_a_excerpt.wad";
        fs.mount("textures", fs::openFS<fs::WadFileSystem>(additionalWadPath));

        // Overriding is determined by load order: Wads that are loaded later override
        // materials from other wads that were loaded before. But the material collections
        // are sorted by name and not by load order!
        CHECK_THAT(
          loadMaterialCollections(
            fs, materialConfig, createResource, taskManager, logger),
          MatchesMaterialCollections({
            {
              "cr8_a_excerpt.wad", // sorting does not depend on load order
              {
                MaterialInfo{"added", 128, 128},
                // overrides material from cr8_czg.wad
                MaterialInfo{"cr8_czg_1", 64, 128},
              },
            },
            {
              "cr8_czg.wad",
              {
                MaterialInfo{"blowjob_machine", 128, 128},
                MaterialInfo{"bongs2", 128, 128},
                MaterialInfo{"can-o-jam", 64, 64},
                MaterialInfo{"cap4can-o-jam", 64, 64},
                MaterialInfo{"coffin1", 128, 128},
                MaterialInfo{"coffin2", 128, 128},
                // overridden from cr8_a_excerpt.wad
                // MaterialInfo{"cr8_czg_1", 64, 64},
                MaterialInfo{"cr8_czg_2", 64, 64},
                MaterialInfo{"cr8_czg_3", 64, 128},
                MaterialInfo{"cr8_czg_4", 64, 128},
                MaterialInfo{"cr8_czg_5", 64, 128},
                MaterialInfo{"crackpipes", 128, 128},
                MaterialInfo{"czg_backhole", 128, 128},
                MaterialInfo{"czg_fronthole", 128, 128},
                MaterialInfo{"dex_5", 128, 128},
                MaterialInfo{"eat_me", 64, 64},
                MaterialInfo{"for_sux-m-ass", 64, 64},
                MaterialInfo{"lasthopeofhuman", 128, 128},
                MaterialInfo{"polished_turd", 64, 64},
                MaterialInfo{"speedM_1", 128, 128},
                MaterialInfo{"u_get_this", 64, 64},
              },
            },
          }));
      }
    }

    SECTION("Quake 3 shaders")
    {
      SECTION("Linking shaders with images")
      {
        SECTION("Shader with image")
        {
          const auto testDir =
            fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/shader_with_image";
          fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

          const auto materialConfig = mdl::MaterialConfig{
            .root = "textures",
            .extensions = {".tga", ".png", ".jpg", ".jpeg"},
            .palette = "",
            .property = std::nullopt,
            .shaderSearchPath = "scripts",
            .excludes = {},
          };

          CHECK_THAT(
            loadMaterialCollections(
              fs, materialConfig, createResource, taskManager, logger),
            MatchesMaterialCollections({
              {
                "textures/test",
                {
                  MaterialInfo{"test/editor_image", 128, 128}, // generated for image file
                  MaterialInfo{"test/some_shader", 128, 128},  // loaded from shader file
                },
              },
            }));
        }

        SECTION("Shader overrides image of same name")
        {
          const auto testDir =
          fixtureRoot
          / "test/mdl/LoadMaterialCollections/shaders/"
            "shader_with_image_same_name";
          fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

          const auto materialConfig = mdl::MaterialConfig{
            .root = "textures",
            .extensions = {".tga", ".png", ".jpg", ".jpeg"},
            .palette = "",
            .property = std::nullopt,
            .shaderSearchPath = "scripts",
            .excludes = {},
          };

          CHECK_THAT(
            loadMaterialCollections(
              fs, materialConfig, createResource, taskManager, logger),
            MatchesMaterialCollections({
              {
                "textures/test",
                {
                  MaterialInfo{"test/editor_image", 128, 128}, // generated for image file
                  MaterialInfo{"test/image_exists_with_editor_image", 128, 128},
                  MaterialInfo{"test/image_exists_without_editor_image", 64, 64},
                },
              },
            }));
        }

        SECTION("Shader with missing image file")
        {
          const auto testDir =
            fixtureRoot
            / "test/mdl/LoadMaterialCollections/shaders/shader_with_missing_image";
          const auto fallbackDir = testDir / "fallback";

          // We need to mount the fallback dir so that we can find "__TB_empty.png" which
          // is automatically used when no texture can be found for a shader.
          fs.mount("", std::make_unique<fs::DiskFileSystem>(fallbackDir));
          fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

          const auto materialConfig = mdl::MaterialConfig{
            .root = "textures",
            .extensions = {".tga", ".png", ".jpg", ".jpeg"},
            .palette = "",
            .property = std::nullopt,
            .shaderSearchPath = "scripts",
            .excludes = {},
          };

          CHECK_THAT(
            loadMaterialCollections(
              fs, materialConfig, createResource, taskManager, logger),
            MatchesMaterialCollections({
              {
                "textures",
                {
                  MaterialInfo{"__TB_empty", 32, 32}, // generated for fallback image
                },
              },
              {
                "textures/test",
                {
                  MaterialInfo{"test/some_shader", 32, 32}, // loaded from shader file
                },
              },
            }));

          const auto materialCollections =
            loadMaterialCollections(
              fs, materialConfig, createResource, taskManager, logger)
              .value();
          const auto* shaderMaterial =
            findMaterial(materialCollections, "test/some_shader");
          REQUIRE(shaderMaterial != nullptr);
          CHECK(shaderMaterial->previewImageMissing());

          const auto* imageMaterial = findMaterial(materialCollections, "__TB_empty");
          REQUIRE(imageMaterial != nullptr);
          CHECK_FALSE(imageMaterial->previewImageMissing());
        }
      }

      SECTION("Skip malformed shader files")
      {
        const auto testDir =
          fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/malformed_shader";
        fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

        const auto materialConfig = mdl::MaterialConfig{
          .root = "textures",
          .extensions = {".tga", ".png", ".jpg", ".jpeg"},
          .palette = "",
          .property = std::nullopt,
          .shaderSearchPath = "scripts",
          .excludes = {},
        };

        CHECK_THAT(
          loadMaterialCollections(
            fs, materialConfig, createResource, taskManager, logger),
          MatchesMaterialCollections({
            {
              "textures/test",
              {
                MaterialInfo{"test/editor_image", 128, 128}, // generated for image file
                MaterialInfo{"test/some_shader", 128, 128},  // loaded from shader file
              },
            },
          }));
      }

      SECTION("Find shader image")
      {
        const auto testDir =
          fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/find_shader_image";
        const auto fallbackDir = testDir / "fallback";

        // We need to mount the fallback dir so that we can find "__TB_empty.png" which is
        // automatically used when no texture can be found for a shader.
        fs.mount("", std::make_unique<fs::DiskFileSystem>(fallbackDir));
        fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

        const auto materialConfig = mdl::MaterialConfig{
          .root = "textures",
          .extensions = {".tga", ".png", ".jpg", ".jpeg"},
          .palette = "",
          .property = std::nullopt,
          .shaderSearchPath = "scripts",
          .excludes = {},
        };

        CHECK_THAT(
          loadMaterialCollections(
            fs, materialConfig, createResource, taskManager, logger),
          MatchesMaterialCollections({
            {
              "textures",
              {
                MaterialInfo{"__TB_empty", 32, 32}, // generated for fallback image
              },
            },
            {
              "textures/test",
              {
                MaterialInfo{"test/different_extension", 128, 128},
                MaterialInfo{"test/editor_image", 128, 128},
                MaterialInfo{"test/light_image", 128, 64},
                MaterialInfo{"test/missing_extension", 128, 128},
                MaterialInfo{"test/no_corresponding_image", 32, 32},
                MaterialInfo{"test/stage_map", 64, 128},
                MaterialInfo{"test/with_editor_image", 128, 128},
                MaterialInfo{"test/with_light_image", 128, 64},
                MaterialInfo{"test/with_shader_path", 64, 64},
                MaterialInfo{"test/with_stage_map", 64, 128},
              },
            },
          }));
      }
    }
  }

  SECTION("loadMaterialCollections with Doom 3 materials")
  {
    auto fs = fs::VirtualFileSystem{};
    auto logger = CollectingLogger{};

    const auto fixtureRoot = getFixtureRoot();

    auto taskManager = kdl::task_manager{};

    const auto testDir = fixtureRoot / "test/mdl/LoadMaterialCollections/doom3";

    // "__TB_empty.png" is the placeholder for materials without a preview image
    fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir / "fallback"));
    fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

    const auto result = loadMaterialCollections(
      fs, Doom3FixtureMaterialConfig, createResource, taskManager, logger);

    // Only materials under textures/ are shown, named without the root, in collections
    // named after their directories. The loose images are excluded by pattern or
    // replaced by the material of the same name.
    CHECK_THAT(
      result,
      MatchesMaterialCollections({
        {
          "textures",
          {
            MaterialInfo{"__TB_empty", 32, 32}, // generated for the placeholder image
          },
        },
        {
          "textures/test",
          {
            MaterialInfo{"test/alphatest", 32, 16},
            MaterialInfo{"test/config_order", 16, 16},
            MaterialInfo{"test/dds_named", 20, 12},
            MaterialInfo{"test/dds_only", 20, 12},
            MaterialInfo{"test/duplicate", 32, 16},
            MaterialInfo{"test/first_stage", 48, 8},
            MaterialInfo{"test/generated_only", 32, 32},
            MaterialInfo{"test/invisible", 32, 32},
            MaterialInfo{"test/loose", 24, 8},
            MaterialInfo{"test/missing", 32, 32},
            MaterialInfo{"test/missing_editor", 16, 32},
            MaterialInfo{"test/named_like_image", 32, 32},
            MaterialInfo{"test/nodraw", 8, 8},
            MaterialInfo{"test/opaque_binary_alpha", 8, 4},
            MaterialInfo{"test/precedence", 32, 16},
            MaterialInfo{"test/substituted", 8, 16},
            MaterialInfo{"test/table_declared", 16, 32},
            MaterialInfo{"test/table_undeclared", 32, 16},
            MaterialInfo{"test/tb_diffuse", 40, 8},
            MaterialInfo{"test/tb_editor", 16, 32},
            MaterialInfo{"test/tb_editor_shared", 24, 40},
            MaterialInfo{"test/translucent_lit", 16, 32},
            MaterialInfo{"test/two_sided", 16, 32},
            MaterialInfo{"test/unterminated", 32, 32},
          },
        },
      }));

    REQUIRE(result.is_success());
    const auto& materialCollections = result.value();
    const auto material = [&](const std::string& name) -> const gl::Material& {
      const auto* material = findMaterial(materialCollections, name);
      REQUIRE(material != nullptr);
      return *material;
    };

    SECTION("preview images")
    {
      // the editor image wins over the diffuse map
      CHECK(material("test/precedence").texture()->width() == 32);

      // an editor image found only under _tb/ counts only if some material, here one
      // outside of the material root, uses it in a diffuse stage; otherwise the engine
      // can't load it and uses the diffuse map
      CHECK(material("test/tb_editor").texture()->width() == 16);
      CHECK(material("test/tb_editor_shared").texture()->width() == 24);

      // a missing editor image falls back to the diffuse map, and without an editor
      // image and a diffuse stage, the first stage's image is used
      CHECK(material("test/missing_editor").texture()->width() == 16);
      CHECK(material("test/first_stage").texture()->width() == 48);

      // a .tga in the script finds the .png on disk, and a diffuse map only under _tb/
      // is found
      CHECK(material("test/substituted").texture()->width() == 8);
      CHECK(material("test/tb_diffuse").texture()->width() == 40);

      // a .dds goes through the DDS loader
      CHECK(
        material("test/dds_only").texture()->format()
        == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);

      // the file as named wins, then the order of the configured extensions decides
      CHECK(
        material("test/dds_named").texture()->format()
        == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
      CHECK(material("test/config_order").texture()->width() == 16);
      CHECK(
        material("test/config_order").texture()->format()
        != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);

      // the loose image textures/test/Loose.png and the material textures/test/loose
      // are one material
      CHECK(material("test/loose").texture()->width() == 24);
      CHECK(findMaterial(materialCollections, "test/Loose") == nullptr);

      // the engine defaults this material for looking up an undeclared table; it keeps
      // its editor image
      CHECK(material("test/table_undeclared").texture()->width() == 32);
      CHECK(
        material("test/table_undeclared").surfaceParms().contains("engine_defaulted"));

      // a material without stages and editor image falls back to an image of its own
      // name, but a material that names other images doesn't, and neither does one with
      // stages that show only generated images
      CHECK(material("test/nodraw").texture()->width() == 8);
      CHECK(material("test/named_like_image").texture()->width() == 32);
      CHECK(material("test/generated_only").texture()->width() == 32);
      CHECK(material("test/generated_only").previewImageMissing());
    }

    SECTION("previewImageMissing")
    {
      auto missing = std::vector<std::string>{};
      for (const auto& materialCollection : materialCollections)
      {
        for (const auto& collectionMaterial : materialCollection.materials())
        {
          if (collectionMaterial.previewImageMissing())
          {
            missing.push_back(collectionMaterial.name());
          }
        }
      }

      CHECK(
        missing
        == std::vector<std::string>{
          "test/generated_only",
          "test/invisible",
          "test/missing",
          "test/named_like_image",
          "test/unterminated",
        });
    }

    SECTION("shared texture resources")
    {
      // materials that show the same image share one texture resource
      CHECK(
        &material("test/precedence").textureResource()
        == &material("test/alphatest").textureResource());
      CHECK(
        &material("test/precedence").textureResource()
        == &material("test/duplicate").textureResource());
      CHECK(
        &material("test/missing").textureResource()
        == &material("test/invisible").textureResource());
      CHECK(
        &material("test/precedence").textureResource()
        != &material("test/missing_editor").textureResource());
    }

    SECTION("render state")
    {
      // alpha tested: an explicit alpha function with the stage's threshold
      CHECK(
        material("test/alphatest").effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.25f});

      // translucent and unlit: the stage's blend function
      CHECK(
        material("test/first_stage").effectiveBlendFunc()
        == gl::MaterialBlendFunc{
          gl::MaterialBlendFunc::Enable::UseFactors, GL_DST_COLOR, GL_ZERO});

      // translucent and lit: the image decides
      CHECK(
        material("test/translucent_lit").effectiveBlendFunc().enable
        == gl::MaterialBlendFunc::Enable::UseDefault);

      // opaque: never blended or alpha tested, even with an image with binary alpha
      CHECK(
        material("test/precedence").effectiveBlendFunc().enable
        == gl::MaterialBlendFunc::Enable::DisableBlend);
      CHECK(
        material("test/precedence").effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.0f});
      REQUIRE(material("test/opaque_binary_alpha").texture() != nullptr);
      CHECK(
        material("test/opaque_binary_alpha").texture()->alphaDomain()
        == img::ImageAlphaDomain::Binary);
      CHECK(
        material("test/opaque_binary_alpha").effectiveAlphaFunc()
        == gl::MaterialAlphaFunc{gl::MaterialAlphaFunc::Compare::GreaterEqual, 0.0f});
      CHECK(
        material("test/opaque_binary_alpha").effectiveBlendFunc().enable
        == gl::MaterialBlendFunc::Enable::DisableBlend);

      // culling
      CHECK(material("test/precedence").culling() == gl::MaterialCulling::Back);
      CHECK(material("test/two_sided").culling() == gl::MaterialCulling::None);

      // the tag vocabulary reaches the material
      CHECK(
        material("test/invisible").surfaceParms()
        == kdl::flat_set<std::string>{"coverage_translucent", "nostages", "playerclip"});
    }

    SECTION("summary")
    {
      CHECK(
        logger.countMessagesContaining(
          "Loaded 26 materials from 3 files (2 defaulted, 1 unterminated); 24 under "
          "textures/, 5 without a preview image")
        == 1);
    }
  }

  SECTION("loadShaderFiles")
  {
    auto fs = fs::VirtualFileSystem{};
    auto logger = CollectingLogger{};

    const auto fixtureRoot = getFixtureRoot();

    auto taskManager = kdl::task_manager{};

    SECTION("Quake 3 shaders defined in two files")
    {
      const auto testDir =
        fixtureRoot / "test/mdl/LoadMaterialCollections/shaders/duplicate_shader";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      // shaderExtensions keeps its default
      const auto materialConfig = mdl::MaterialConfig{
        .root = "textures",
        .extensions = {".tga", ".png", ".jpg", ".jpeg"},
        .palette = "",
        .property = std::nullopt,
        .shaderSearchPath = "scripts",
        .excludes = {},
      };

      const auto loadedShaders =
        loadShaderFiles(fs, materialConfig, taskManager, logger) | kdl::value();
      CHECK(loadedShaders.fileCount == 3);

      // the files are read in sorted order, ignoring case, and the first definition wins
      REQUIRE(loadedShaders.shaders.size() == 2);
      const auto* duplicate =
        findShader(loadedShaders.shaders, "textures/test/duplicate");
      REQUIRE(duplicate != nullptr);
      CHECK(duplicate->editorImage == "textures/test/from_a.tga");

      // names that differ only in case are the same name
      const auto* differentCase = findShader(loadedShaders.shaders, "textures/test/Case");
      REQUIRE(differentCase != nullptr);
      CHECK(differentCase->editorImage == "textures/test/case_upper.tga");
      CHECK(findShader(loadedShaders.shaders, "textures/test/case") == nullptr);

      CHECK(logger.countMessagesContaining("it is already defined in") == 2);
    }

    SECTION("Doom 3 materials")
    {
      const auto testDir = fixtureRoot / "test/mdl/LoadMaterialCollections/doom3";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      const auto loadedShaders =
        loadShaderFiles(fs, Doom3FixtureMaterialConfig, taskManager, logger)
        | kdl::value();
      const auto& shaders = loadedShaders.shaders;

      // only files with a configured extension are read, whatever its case
      CHECK(loadedShaders.fileCount == 3);
      CHECK(findShader(shaders, "textures/test/ignored") == nullptr);

      CHECK(shaders.size() == 26);
      CHECK(std::ranges::is_sorted(shaders, std::less{}, &Quake3Shader::shaderPath));
      CHECK(std::ranges::all_of(shaders, [](const auto& shader) {
        return shader.format == Quake3Shader::Format::Doom3;
      }));

      // records outside of the material root are kept for entity models
      CHECK(findShader(shaders, "models/test/crate") != nullptr);

      // the files are read in sorted order, ignoring case, and the first definition wins
      const auto* duplicate = findShader(shaders, "textures/test/duplicate");
      REQUIRE(duplicate != nullptr);
      CHECK(
        duplicate->previewImages
        == std::vector<std::filesystem::path>{"textures/test/editor_ed.tga"});
      CHECK(logger.countMessagesContaining("it is already defined in") == 2);

      // a decl without its closing brace is counted, and the engine defaults it
      CHECK(loadedShaders.unterminatedCount == 1);
      const auto* unterminated = findShader(shaders, "textures/test/unterminated");
      REQUIRE(unterminated != nullptr);
      CHECK(unterminated->engineDefaulted);

      // a table declared in another file is found
      const auto* tableDeclared = findShader(shaders, "textures/test/table_declared");
      REQUIRE(tableDeclared != nullptr);
      CHECK_FALSE(tableDeclared->engineDefaulted);
      CHECK(tableDeclared->referencedTables == std::vector<std::string>{"fixturetable"});

      // the engine defaults a material that looks up a table no file declares; the
      // material keeps its editor image
      const auto* tableUndeclared = findShader(shaders, "textures/test/table_undeclared");
      REQUIRE(tableUndeclared != nullptr);
      CHECK(tableUndeclared->engineDefaulted);
      CHECK(tableUndeclared->surfaceParms.contains("engine_defaulted"));
      CHECK(
        tableUndeclared->previewImages
        == std::vector<std::filesystem::path>{"textures/test/editor_ed.tga"});
      CHECK(
        logger.countMessagesContaining(
          "No material file declares table 'undeclared_table'")
        == 1);
    }

    SECTION("Doom 3 decls without their closing brace")
    {
      const auto testDir = fixtureRoot / "test/mdl/LoadShaders/unterminated";
      fs.mount("", std::make_unique<fs::DiskFileSystem>(testDir));

      const auto loadedShaders =
        loadShaderFiles(fs, Doom3FixtureMaterialConfig, taskManager, logger)
        | kdl::value();

      // a comment left open after the last decl doesn't count, but a decl whose closing
      // brace is inside a comment left open does
      CHECK(loadedShaders.unterminatedCount == 1);

      const auto* closed = findShader(loadedShaders.shaders, "textures/test/closed");
      REQUIRE(closed != nullptr);
      CHECK_FALSE(closed->unterminated);

      const auto* swallowed =
        findShader(loadedShaders.shaders, "textures/test/swallowed");
      REQUIRE(swallowed != nullptr);
      CHECK(swallowed->unterminated);
      CHECK(swallowed->engineDefaulted);
    }
  }
}

} // namespace tb::mdl
