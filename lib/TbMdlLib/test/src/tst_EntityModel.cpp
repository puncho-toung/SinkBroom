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

#include "TestLogger.h"
#include "base/PreferenceManager.h"
#include "gl/IndexRangeMap.h"
#include "gl/IndexRangeMapBuilder.h"
#include "gl/Material.h"
#include "gl/MaterialIndexRangeMap.h"
#include "gl/MaterialIndexRangeMapBuilder.h"
#include "gl/MaterialIndexRangeRenderer.h"
#include "gl/PrimType.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "gl/Vertex.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityModel.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/LoadEntityModel.h"
#include "mdl/TestUtils.h"

#include "kd/ranges/to.h"

#include "vm/approx.h"
#include "vm/bbox.h"
#include "vm/intersection.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <filesystem>
#include <ranges>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

gl::Material makeDummyMaterial(std::string name)
{
  auto textureResource = gl::createTextureResource(gl::Texture{1, 1});
  return gl::Material{std::move(name), std::move(textureResource)};
}

gl::IndexRangeMapBuilder<EntityModelVertex::Type> makeDummyBuilder()
{
  auto size = gl::IndexRangeMap::Size{};
  size.inc(gl::PrimType::Triangles, 1);

  auto builder = gl::IndexRangeMapBuilder<EntityModelVertex::Type>{3, size};
  builder.addTriangle(EntityModelVertex{}, EntityModelVertex{}, EntityModelVertex{});

  return builder;
}

} // namespace

TEST_CASE("EntityModel")
{
  SECTION("intersect")
  {
    const auto environmentConfig = EnvironmentConfig{};
    const auto& gameInfo = QuakeGameInfo;

    auto logger = TestLogger{};
    auto fs = GameFileSystem{};
    fs.initialize(
      environmentConfig,
      gameInfo.gameConfig,
      gameInfo.gamePathPreference.defaultValue,
      {},
      logger);

    const auto path = std::filesystem::path{"cube.bsp"};
    const auto loadMaterial = [](auto) -> gl::Material {
      throw std::runtime_error{"should not be called"};
    };

    auto model = loadEntityModelSync(
      fs,
      gameInfo.gameConfig.materialConfig,
      ModelConvention::Default,
      path,
      loadMaterial,
      {},
      logger);

    auto& frame = model.value().data()->frames().at(0);

    const auto box = vm::bbox3f(vm::vec3f::fill(-32), vm::vec3f::fill(32));
    CHECK(box == frame.bounds());

    // test some hitting rays
    for (int x = -45; x <= 45; x += 15)
    {
      for (int y = -45; y <= 45; y += 15)
      {
        for (int z = -45; z <= 45; z += 15)
        {
          // shoot a ray from (x, y, z) to (0, 0, 0), it will hit the box
          const auto startPoint = vm::vec3f(x, y, z);
          if (box.contains(startPoint))
          {
            continue;
          }
          const auto endPoint = vm::vec3f{0, 0, 0};
          const auto ray = vm::ray3f(startPoint, vm::normalize(endPoint - startPoint));

          const auto treeDist = frame.intersect(ray);
          const auto expected = vm::intersect_ray_bbox(ray, box);

          CHECK(expected == vm::optional_approx(treeDist));
        }
      }
    }

    // test a missing ray
    const auto missRay = vm::ray3f(vm::vec3f(0, -33, -33), vm::vec3f{0, 1, 0});
    CHECK(frame.intersect(missRay) == std::nullopt);
    CHECK(vm::intersect_ray_bbox(missRay, box) == std::nullopt);
  }

  SECTION("buildRenderer")
  {
    // Ensure that when rendering a model with multiple surfaces
    // and when each surface has a variable number of skins, we
    // default to a skin index of 0 if the provided index is not
    // present for the surface.

    auto modelData = EntityModelData{PitchType::Normal, Orientation::Oriented};
    auto& frame = modelData.addFrame("test", vm::bbox3f{0, 8});

    SECTION("Out of bounds skin index")
    {
      // Prepare the first surface - it will only have one skin
      auto& surface1 = modelData.addSurface("surface 1", 1);

      auto materials1 = std::vector<gl::Material>{};
      materials1.push_back(makeDummyMaterial("skin1"));
      surface1.setSkins(std::move(materials1));

      auto builder1 = makeDummyBuilder();
      surface1.addMesh(frame, builder1.vertices(), builder1.indices());

      // The second surface will have two skins
      auto& surface2 = modelData.addSurface("surface 2", 1);

      auto materials2 = std::vector<gl::Material>{};
      materials2.push_back(makeDummyMaterial("skin2a"));
      materials2.push_back(makeDummyMaterial("skin2b"));
      surface2.setSkins(std::move(materials2));

      auto builder2 = makeDummyBuilder();
      surface2.addMesh(frame, builder2.vertices(), builder2.indices());

      // even though the model has 2 skins, we should get a valid renderer even if we
      // request to use skin 3
      CHECK(modelData.buildRenderer(0, 0) != nullptr);
      CHECK(modelData.buildRenderer(1, 0) != nullptr);
      CHECK(modelData.buildRenderer(2, 0) != nullptr);
    }

    SECTION("Surface without skins")
    {
      auto& surface = modelData.addSurface("surface 1", 1);

      auto builder = makeDummyBuilder();
      surface.addMesh(frame, builder.vertices(), builder.indices());

      // even if the surface doesn't have any skins, we can still make a renderer
      CHECK(modelData.buildRenderer(0, 0));
    }

    SECTION("Out of bounds frame index")
    {
      auto& surface = modelData.addSurface("surface 1", 1);

      auto materials = std::vector<gl::Material>{};
      materials.push_back(makeDummyMaterial("skin1"));
      surface.setSkins(std::move(materials));

      auto builder = makeDummyBuilder();
      surface.addMesh(frame, builder.vertices(), builder.indices());

      REQUIRE(modelData.buildRenderer(0, 0) != nullptr);
      CHECK(modelData.buildRenderer(0, 1) == nullptr);
    }
  }

  SECTION("forEachPrimitive")
  {
    auto modelData = EntityModelData{PitchType::Normal, Orientation::Oriented};
    auto& frame = modelData.addFrame("test", vm::bbox3f{0, 8});
    auto& surface = modelData.addSurface("surface", 1);

    auto materials = std::vector<gl::Material>{};
    materials.push_back(makeDummyMaterial("skin1"));
    materials.push_back(makeDummyMaterial("skin2"));
    surface.setSkins(std::move(materials));

    using Primitive =
      std::tuple<const gl::Material*, gl::PrimType, std::vector<vm::vec3f>>;
    const auto primitives = [&](const size_t frameIndex) {
      auto result = std::vector<Primitive>{};
      surface.forEachPrimitive(
        frameIndex, [&](const auto* skin, const auto primType, const auto vertices) {
          result.emplace_back(
            skin, primType, vertices | std::views::transform([](const auto& vertex) {
                              return gl::getVertexComponent<0>(vertex);
                            }) | kdl::ranges::to<std::vector>());
        });
      return result;
    };

    const auto v0 = EntityModelVertex{{1, 0, 0}, {0, 0}};
    const auto v1 = EntityModelVertex{{0, 1, 0}, {0, 0}};
    const auto v2 = EntityModelVertex{{0, 0, 1}, {0, 0}};

    SECTION("a surface without a mesh has no primitives")
    {
      CHECK(primitives(0).empty());
      CHECK(primitives(1).empty());
    }

    SECTION("a mesh that is drawn with the selected skin")
    {
      auto size = gl::IndexRangeMap::Size{};
      size.inc(gl::PrimType::Triangles, 1);

      auto builder = gl::IndexRangeMapBuilder<EntityModelVertex::Type>{3, size};
      builder.addTriangle(v0, v1, v2);
      surface.addMesh(frame, builder.vertices(), builder.indices());

      CHECK(
        primitives(0)
        == std::vector<Primitive>{
          {nullptr, gl::PrimType::Triangles, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
        });
      CHECK(primitives(1).empty());
    }

    SECTION("a mesh with a skin for each primitive")
    {
      const auto* skin1 = surface.skin(0);
      const auto* skin2 = surface.skin(1);

      auto size = gl::MaterialIndexRangeMap::Size{};
      size.inc(skin1, gl::PrimType::Triangles, 3);
      size.inc(skin2, gl::PrimType::Triangles, 3);

      auto builder = gl::MaterialIndexRangeMapBuilder<EntityModelVertex::Type>{6, size};
      builder.addTriangle(skin2, v2, v1, v0);
      builder.addTriangle(skin1, v0, v1, v2);
      surface.addMesh(frame, builder.vertices(), builder.indices());

      CHECK_THAT(
        primitives(0),
        Catch::Matchers::UnorderedEquals(std::vector<Primitive>{
          {skin1, gl::PrimType::Triangles, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
          {skin2, gl::PrimType::Triangles, {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}}},
        }));
    }
  }
}


} // namespace tb::mdl
