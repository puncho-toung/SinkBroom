/*
 Copyright (C) 2025 Kristian Duske

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


#include "Matchers.h"
#include "gl/Material.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EditorContext.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/Grid.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/Matchers.h"
#include "mdl/NodeHandles.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/UvCoordSystem.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"
#include "kd/ranges/zip_view.h"

#include "vm/approx.h"
#include "vm/vec_io.h" // IWYU pragma: keep

#include <array>
#include <chrono>
#include <cmath>
#include <optional>
#include <ranges>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
using namespace Catch::Matchers;

namespace
{

bool checkPlanePointsIntegral(const auto* brushNode)
{
  return std::ranges::all_of(brushNode->brush().faces(), [](const auto& face) {
    return std::ranges::all_of(face.points(), pointExactlyIntegral);
  });
}

bool checkVerticesIntegral(const auto* brushNode)
{
  return std::ranges::all_of(brushNode->brush().vertices(), [](const auto& vertex) {
    return pointExactlyIntegral(vertex->position());
  });
}

// The volume of a convex brush, by fans over its faces.
double brushVolume(const Brush& brush)
{
  const auto origin = brush.bounds().center();
  auto sixTimes = 0.0;
  for (const auto& face : brush.faces())
  {
    const auto polygon = face.vertexPositions();
    for (size_t i = 1; i + 1 < polygon.size(); ++i)
    {
      sixTimes += vm::dot(
        polygon[0] - origin, vm::cross(polygon[i] - origin, polygon[i + 1] - origin));
    }
  }
  return std::abs(sixTimes) / 6.0;
}

// True if every face of `piece` lies on a face plane of `original` or on an axial plane
// w = k * grid.
bool keepsPlanes(const Brush& piece, const Brush& original, const double grid)
{
  return std::ranges::all_of(piece.faces(), [&](const auto& face) {
    const auto& plane = face.boundary();
    const auto onOriginal = std::ranges::any_of(original.faces(), [&](const auto& other) {
      return vm::is_equal(plane, other.boundary(), vm::constants<double>::almost_zero());
    });
    const auto axis = vm::find_abs_max_component(plane.normal);
    const auto axial = std::abs(plane.normal[axis]) == 1.0
                       && std::fmod(std::abs(plane.distance), grid) == 0.0;
    return onOriginal || axial;
  });
}

// The brush's faces as sorted vertex lists, the list sorted; rounded to `snapTo` if given.
std::vector<std::vector<vm::vec3d>> facetsOf(
  const Brush& brush, const std::optional<double> snapTo = std::nullopt)
{
  auto result = std::vector<std::vector<vm::vec3d>>{};
  for (const auto& face : brush.faces())
  {
    auto facet = face.vertexPositions();
    if (snapTo)
    {
      for (auto& position : facet)
      {
        position = *snapTo * vm::round(position / *snapTo);
      }
    }
    std::sort(facet.begin(), facet.end());
    result.push_back(std::move(facet));
  }
  std::sort(result.begin(), result.end());
  return result;
}

// True if the stock snap keeps the brush's faces (its rounded facets).
bool stockSnapKeepsFaces(Brush brush, const vm::bbox3d& worldBounds, const double snapTo)
{
  const auto expected = facetsOf(brush, snapTo);
  return brush.canSnapVertices(worldBounds, snapTo)
         && brush.snapVertices(worldBounds, snapTo, false) && facetsOf(brush) == expected;
}

// A terrain of 2 * n * n triangle prisms on a `cell` lattice, with heights off the grid,
// as a triangle-mode terrain brush tool makes it.
std::vector<Brush> makeTrianglePrisms(
  const BrushBuilder& builder, const size_t n, const double cell)
{
  const auto height = [](const size_t i, const size_t j) {
    return 100.0 + 37.3 * std::sin(0.7 * double(i)) + 21.9 * std::cos(1.3 * double(j))
           + 0.37 * double((i * j) % 7);
  };

  auto brushes = std::vector<Brush>{};
  brushes.reserve(2 * n * n);
  for (size_t i = 0; i < n; ++i)
  {
    for (size_t j = 0; j < n; ++j)
    {
      const auto x0 = double(i) * cell;
      const auto y0 = double(j) * cell;
      const auto p00 = vm::vec3d{x0, y0, height(i, j)};
      const auto p10 = vm::vec3d{x0 + cell, y0, height(i + 1, j)};
      const auto p01 = vm::vec3d{x0, y0 + cell, height(i, j + 1)};
      const auto p11 = vm::vec3d{x0 + cell, y0 + cell, height(i + 1, j + 1)};
      for (const auto& top : {std::array{p00, p10, p11}, std::array{p00, p11, p01}})
      {
        auto points = std::vector<vm::vec3d>{};
        for (const auto& point : top)
        {
          points.push_back(point);
          points.push_back(vm::vec3d{point.x(), point.y(), 0.0});
        }
        brushes.push_back(builder.createBrush(points, "material") | kdl::value());
      }
    }
  }
  return brushes;
}

// n * n boxes on a `cell` lattice whose tops are planes tilted by slopes off the grid, as
// after sculpting a square-mode terrain: rounding makes some tops non-planar.
std::vector<Brush> makeTiltedBoxes(
  const BrushBuilder& builder, const size_t n, const double cell)
{
  auto brushes = std::vector<Brush>{};
  brushes.reserve(n * n);
  for (size_t i = 0; i < n; ++i)
  {
    for (size_t j = 0; j < n; ++j)
    {
      const auto x0 = double(i) * cell;
      const auto y0 = double(j) * cell;
      const auto a = 0.0137 * double(1 + (i + 2 * j) % 5);
      const auto b = 0.0091 * double(1 + (2 * i + j) % 3);
      auto points = std::vector<vm::vec3d>{};
      for (const auto dx : {0.0, cell})
      {
        for (const auto dy : {0.0, cell})
        {
          points.push_back(vm::vec3d{x0 + dx, y0 + dy, 0.0});
          points.push_back(vm::vec3d{x0 + dx, y0 + dy, 64.3 + a * dx + b * dy});
        }
      }
      brushes.push_back(builder.createBrush(points, "material") | kdl::value());
    }
  }
  return brushes;
}

// The chunk cuts only brushes whose materials are loaded and opaque, and the test maps load
// none: the chunk tests give their brushes this material. Static: a map refers to it until
// the map is destroyed, after the test.
gl::Material& opaqueTestMaterial()
{
  static auto material =
    gl::Material{"opaque", gl::createTextureResource(gl::Texture{64, 64})};
  return material;
}

// Sets the opaque test material on every face, on the node directly: a command would look the
// material up by name again and find none. The faces keep their material names.
void makeOpaque(BrushNode& brushNode)
{
  auto brush = brushNode.brush();
  for (auto& face : brush.faces())
  {
    face.setMaterial(&opaqueTestMaterial());
  }
  brushNode.setBrush(std::move(brush));
}

bool checkBoundsIntegral(const auto* brush)
{
  return pointExactlyIntegral(brush->logicalBounds().min)
         && pointExactlyIntegral(brush->logicalBounds().max);
}

bool checkBrushIntegral(const auto* brush)
{
  return checkPlanePointsIntegral(brush) && checkVerticesIntegral(brush)
         && checkBoundsIntegral(brush);
}

void checkTransformation(
  const Node& node, const Node& original, const vm::mat4x4d& transformation)
{
  CHECK(node.physicalBounds() == original.physicalBounds().transform(transformation));

  REQUIRE(node.childCount() == original.childCount());
  for (const auto& [nodeChild, originalChild] :
       kdl::views::zip(node.children(), original.children()))
  {
    checkTransformation(*nodeChild, *originalChild, transformation);
  }
}

bool hasEmptyName(const std::vector<std::string>& names)
{
  return std::ranges::any_of(names, [](const auto& s) { return s.empty(); });
}

} // namespace

TEST_CASE("Map_Geometry")
{
  auto fixture = MapFixture{};

  SECTION("transformSelection")
  {
    auto& map = fixture.create();

    GIVEN("A node to transform")
    {
      using CreateNode = std::function<Node*(const Map&)>;

      const auto createNode = GENERATE_COPY(
        CreateNode{[](const auto& m) -> Node* {
          auto* groupNode = new GroupNode{Group{"group"}};
          auto* brushNode = createBrushNode(m);
          auto* patchNode = createPatchNode();
          auto* entityNode = new EntityNode{Entity{}};
          groupNode->addChildren({brushNode, patchNode, entityNode});
          return groupNode;
        }},
        CreateNode{[](const auto&) -> Node* { return new EntityNode{Entity{}}; }},
        CreateNode{[](const auto& m) -> Node* {
          auto* entityNode = new EntityNode{Entity{}};
          auto* brushNode = createBrushNode(m);
          auto* patchNode = createPatchNode();
          entityNode->addChildren({brushNode, patchNode});
          return entityNode;
        }},
        CreateNode{[](const auto& m) -> Node* { return createBrushNode(m); }},
        CreateNode{[](const auto&) -> Node* { return createPatchNode(); }});

      auto* node = createNode(map);
      CAPTURE(node->name());

      addNodes(map, {{&parentForNodes(map), {node}}});

      const auto originalNode =
        std::unique_ptr<Node>{node->cloneRecursively(map.worldBounds())};
      const auto transformation = vm::translation_matrix(vm::vec3d{1, 2, 3});

      WHEN("The node is transformed")
      {
        selectNodes(map, {node});
        transformSelection(map, "Transform Nodes", transformation);

        THEN("The transformation was applied to the node and its children")
        {
          checkTransformation(*node, *originalNode.get(), transformation);
        }

        AND_WHEN("The transformation is undone")
        {
          map.undoCommand();

          THEN("The node is back in its original state")
          {
            checkTransformation(*node, *originalNode.get(), vm::mat4x4d::identity());
          }

          AND_WHEN("The transformation is redone")
          {
            map.redoCommand();

            THEN("The transformation is applied again")
            {
              checkTransformation(*node, *originalNode.get(), transformation);
            }
          }
        }
      }
    }

    GIVEN("A group containing a brush entity")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/1715

      auto* brushNode1 = createBrushNode(map);
      addNodes(map, {{&parentForNodes(map), {brushNode1}}});

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      reparentNodes(map, {{entityNode, {brushNode1}}});

      selectNodes(map, {brushNode1});

      auto* groupNode = groupSelectedNodes(map, "test");
      REQUIRE(groupNode->selected());

      WHEN("The group is transformed")
      {
        REQUIRE(transformSelection(
          map, "Translate", vm::translation_matrix(vm::vec3d{16, 0, 0})));

        THEN("The entity does not have any empty property keys")
        {
          CHECK(!hasEmptyName(entityNode->entity().propertyKeys()));

          AND_WHEN("The transformation is undone")
          {
            map.undoCommand();

            THEN("The entity still doesn't have any empty property keys")
            {
              CHECK(!hasEmptyName(entityNode->entity().propertyKeys()));
            }
          }
        }
      }
    }

    GIVEN("Two linked groups")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/3784

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      const auto box = vm::bbox3d{{0, 0, 0}, {64, 64, 64}};

      auto* brushNode1 =
        new BrushNode{builder.createCuboid(box, "material") | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode1}}});
      selectNodes(map, {brushNode1});

      auto* group = groupSelectedNodes(map, "testGroup");
      selectNodes(map, {group});

      auto* linkedGroup = createLinkedDuplicate(map);
      deselectAll(map);
      selectNodes(map, {linkedGroup});
      REQUIRE_THAT(
        map.selection().nodes, UnorderedEquals(std::vector<Node*>{linkedGroup}));

      auto* linkedBrushNode = dynamic_cast<BrushNode*>(linkedGroup->children().at(0));
      REQUIRE(linkedBrushNode != nullptr);

      WHEN("One of the groups is transformed with aligment lock off")
      {
        map.editorContext().setAlignmentLock(false);

        const auto delta = vm::vec3d{0.125, 0, 0};
        REQUIRE(translateSelection(map, delta));

        THEN("The brushes in both linked groups have alignment lock forced on")
        {
          auto getUvCoords =
            [](auto* brushNode, const vm::vec3d& normal) -> std::vector<vm::vec2f> {
            const BrushFace& face =
              brushNode->brush().face(*brushNode->brush().findFace(normal));
            return face.vertexPositions()
                   | std::views::transform([&](auto x) { return face.uvCoords(x); })
                   | kdl::ranges::to<std::vector>();
          };

          // Brushes in linked groups should have alignment lock forced on
          CHECK(uvListsEqual(
            getUvCoords(brushNode1, vm::vec3d{0, 0, 1}),
            getUvCoords(linkedBrushNode, vm::vec3d{0, 0, 1})));
        }
      }
    }

    GIVEN("A brush in a group, with alignment lock off")
    {
      // alignment is forced on only for a brush in a closed group that is linked
      map.editorContext().setAlignmentLock(false);

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});
      auto* groupNode = groupSelectedNodes(map, "testGroup");
      REQUIRE(groupNode);

      const auto topUvCoords = [&]() {
        const auto& face =
          brushNode->brush().face(*brushNode->brush().findFace(vm::vec3d{0, 0, 1}));
        return face.vertexPositions()
               | std::views::transform([&](auto x) { return face.uvCoords(x); })
               | kdl::ranges::to<std::vector>();
      };
      const auto delta = vm::vec3d{0.125, 0, 0};

      WHEN("The group is closed and linked")
      {
        deselectAll(map);
        selectNodes(map, {groupNode});
        REQUIRE(createLinkedDuplicate(map));
        deselectAll(map);
        selectNodes(map, {groupNode});

        const auto uvCoordsBefore = topUvCoords();
        REQUIRE(translateSelection(map, delta));

        THEN("The brush keeps its alignment")
        {
          CHECK(uvListsEqual(uvCoordsBefore, topUvCoords()));
        }
      }

      WHEN("The group is closed and not linked")
      {
        deselectAll(map);
        selectNodes(map, {groupNode});

        const auto uvCoordsBefore = topUvCoords();
        REQUIRE(translateSelection(map, delta));

        THEN("The brush does not keep its alignment")
        {
          CHECK_FALSE(uvListsEqual(uvCoordsBefore, topUvCoords()));
        }
      }

      WHEN("The group is linked and open")
      {
        deselectAll(map);
        selectNodes(map, {groupNode});
        REQUIRE(createLinkedDuplicate(map));
        deselectAll(map);
        openGroup(map, *groupNode);
        selectNodes(map, {brushNode});

        const auto uvCoordsBefore = topUvCoords();
        REQUIRE(translateSelection(map, delta));

        THEN("The brush does not keep its alignment")
        {
          CHECK_FALSE(uvListsEqual(uvCoordsBefore, topUvCoords()));
        }
      }
    }
  }

  SECTION("translateSelection")
  {
    auto& map = fixture.create();

    GIVEN("An entity")
    {
      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});

      WHEN("The entity is translated")
      {
        translateSelection(map, {1, 2, 3});
        THEN("It is transformed accordingly")
        {
          const auto* originValue =
            entityNode->entity().property(EntityPropertyKeys::Origin);
          REQUIRE(originValue != nullptr);
          CHECK(*originValue == "1 2 3");
        }
      }
    }
  }

  SECTION("rotateSelection")
  {
    auto& map = fixture.create();

    SECTION("objects")
    {
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* brushNode1 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {30, 31, 31}}, "material")
        | kdl::value()};
      auto* brushNode2 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{30, 0, 0}, {31, 31, 31}}, "material")
        | kdl::value()};

      REQUIRE(checkBrushIntegral(brushNode1));
      REQUIRE(checkBrushIntegral(brushNode2));

      SECTION("two brushes")
      {
        addNodes(map, {{&parentForNodes(map), {brushNode1, brushNode2}}});
        selectNodes(map, {brushNode1, brushNode2});

        const auto boundsCenter = map.selectionBounds()->center();
        CHECK(boundsCenter == vm::vec3d{15.5, 15.5, 15.5});

        // 90 degrees CCW about the Z axis through the center of the selection
        rotateSelection(map, boundsCenter, vm::vec3d{0, 0, 1}, vm::to_radians(90.0));

        CHECK(checkBrushIntegral(brushNode1));
        CHECK(checkBrushIntegral(brushNode2));

        const auto brush1ExpectedBounds = vm::bbox3d{{0, 0, 0}, {31, 30, 31}};
        const auto brush2ExpectedBounds = vm::bbox3d{{0, 30, 0}, {31, 31, 31}};

        // these should be exactly integral
        CHECK(brushNode1->logicalBounds() == brush1ExpectedBounds);
        CHECK(brushNode2->logicalBounds() == brush2ExpectedBounds);
      }

      SECTION("brush entity")
      {
        auto* entityNode = new EntityNode{Entity{{
          {"classname", "func_door"},
          {"angle", "45"},
        }}};

        addNodes(map, {{&parentForNodes(map), {entityNode}}});
        addNodes(map, {{entityNode, {brushNode1, brushNode2}}});

        REQUIRE(*entityNode->entity().property("angle") == "45");

        SECTION("Rotating some brushes, but not all")
        {
          selectNodes(map, {brushNode1});
          rotateSelection(
            map,
            map.selectionBounds()->center(),
            vm::vec3d{0, 0, 1},
            vm::to_radians(90.0));

          CHECK(*entityNode->entity().property("angle") == "45");
        }

        SECTION("Rotating all brushes")
        {
          selectNodes(map, {brushNode1, brushNode2});
          rotateSelection(
            map,
            map.selectionBounds()->center(),
            vm::vec3d{0, 0, 1},
            vm::to_radians(90.0));

          CHECK(*entityNode->entity().property("angle") == "135");
        }

        SECTION("Rotating grouped brush entity")
        {
          selectNodes(map, {entityNode});
          auto* groupNode = groupSelectedNodes(map, "some_name");

          deselectAll(map);
          selectNodes(map, {groupNode});
          rotateSelection(
            map,
            map.selectionBounds()->center(),
            vm::vec3d{0, 0, 1},
            vm::to_radians(90.0));

          CHECK(*entityNode->entity().property("angle") == "135");
        }
      }
    }

    SECTION("vertices")
    {
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
        | kdl::value()};

      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      auto& vertexHandles = map.nodeHandles();
      vertexHandles.addHandles<VertexHandle>(*brushNode);
      vertexHandles.selectHandles<VertexHandle>(std::vector<VertexHandle>{
        {{-32, -32, 32}},
        {{-32, 32, 32}},
        {{32, -32, 32}},
        {{32, 32, 32}},
      });

      rotateSelection(map, {0, 0, 0}, {0, 0, 1}, vm::to_radians(45.0));

      const auto e = vm::constants<double>::almost_zero();
      const auto x = 45.254833995939407;

      CHECK(brushNode->brush().hasVertex({-x, 0, +32}, e));
      CHECK(brushNode->brush().hasVertex({+x, 0, +32}, e));
      CHECK(brushNode->brush().hasVertex({0, -x, +32}, e));
      CHECK(brushNode->brush().hasVertex({0, +x, +32}, e));

      CHECK(brushNode->brush().hasVertex({-32, -32, -32}, e));
      CHECK(brushNode->brush().hasVertex({-32, +32, -32}, e));
      CHECK(brushNode->brush().hasVertex({+32, -32, -32}, e));
      CHECK(brushNode->brush().hasVertex({+32, +32, -32}, e));
    }

    SECTION("Rotating a group containing a brush entity")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/1754

      auto* brushNode1 = createBrushNode(map);
      addNodes(map, {{&parentForNodes(map), {brushNode1}}});

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      reparentNodes(map, {{entityNode, {brushNode1}}});

      selectNodes(map, {brushNode1});

      auto* groupNode = groupSelectedNodes(map, "test");
      CHECK(groupNode->selected());

      CHECK(!entityNode->entity().hasProperty("origin"));
      CHECK(rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, 10));
      CHECK(!entityNode->entity().hasProperty("origin"));

      map.undoCommand();

      CHECK(!entityNode->entity().hasProperty("origin"));
    }

    SECTION("Undoing a rotation removes angle key")
    {
      auto* entityNode = new EntityNode{Entity{{
        {EntityPropertyKeys::Classname, "test"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      CHECK(!entityNode->entity().hasProperty("angle"));

      selectNodes(map, {entityNode});
      rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, vm::to_radians(15.0));
      CHECK(entityNode->entity().hasProperty("angle"));
      CHECK(*entityNode->entity().property("angle") == "15");

      map.undoCommand();
      CHECK(!entityNode->entity().hasProperty("angle"));
    }
  }

  SECTION("rotateSelection with the id Tech 4 rotation policy")
  {
    auto fixtureConfig = MapFixtureConfig{};
    fixtureConfig.gameInfo.gameConfig.entityConfig.rotationPolicy =
      EntityRotationPolicy::IdTech4;
    auto& map = fixture.create(fixtureConfig);
    REQUIRE(
      map.worldNode().entityPropertyConfig().rotationPolicy
      == EntityRotationPolicy::IdTech4);

    SECTION("A point entity's rotation is written to angles")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "some_point_entity"},
        {"rotation", "0 1 0 -1 0 0 0 0 1"},
        {"angle", "45"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});

      // 45 degrees clockwise about the Z axis through the entity's origin
      rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, vm::to_radians(-45.0));
      CHECK(*entityNode->entity().property("angles") == "0 45 0");
      CHECK(!entityNode->entity().hasProperty("rotation"));
      CHECK(!entityNode->entity().hasProperty("angle"));

      map.undoCommand();
      CHECK(*entityNode->entity().property("rotation") == "0 1 0 -1 0 0 0 0 1");
      CHECK(*entityNode->entity().property("angle") == "45");
      CHECK(!entityNode->entity().hasProperty("angles"));
    }

    SECTION("An entity with brushes never gets angles")
    {
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 32, 32}}, "material")
        | kdl::value()};

      auto* entityNode = new EntityNode{Entity{{
        {"classname", "func_door"},
        {"angle", "45"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      addNodes(map, {{entityNode, {brushNode}}});
      selectNodes(map, {brushNode});

      rotateSelection(
        map, map.selectionBounds()->center(), vm::vec3d{0, 0, 1}, vm::to_radians(90.0));

      // the angle of a door is the direction in which it opens
      CHECK(*entityNode->entity().property("angle") == "135");
      CHECK(!entityNode->entity().hasProperty("angles"));
      CHECK(!entityNode->entity().hasProperty("rotation"));
    }

    SECTION("The angles of an entity with brushes are not touched")
    {
      auto* brushNode = createBrushNode(map);
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "func_static"},
        {"angles", "0 90 0"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      addNodes(map, {{entityNode, {brushNode}}});
      selectNodes(map, {brushNode});

      rotateSelection(
        map, map.selectionBounds()->center(), vm::vec3d{0, 0, 1}, vm::to_radians(90.0));

      CHECK(*entityNode->entity().property("angles") == "0 90 0");
      CHECK(!entityNode->entity().hasProperty("angle"));
    }

    SECTION("Rotating some brushes of an entity, but not all, leaves its angle alone")
    {
      auto* brushNode1 = createBrushNode(map);
      auto* brushNode2 = createBrushNode(map);
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "func_door"},
        {"angle", "45"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      addNodes(map, {{entityNode, {brushNode1, brushNode2}}});
      selectNodes(map, {brushNode1});

      rotateSelection(
        map, map.selectionBounds()->center(), vm::vec3d{0, 0, 1}, vm::to_radians(90.0));

      CHECK(*entityNode->entity().property("angle") == "45");
      CHECK(!entityNode->entity().hasProperty("angles"));
    }

    SECTION("Rotating a group turns the angle of an entity with brushes in it")
    {
      auto* brushNode = createBrushNode(map);
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "func_door"},
        {"angle", "45"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      addNodes(map, {{entityNode, {brushNode}}});
      selectNodes(map, {entityNode});
      auto* groupNode = groupSelectedNodes(map, "group");
      REQUIRE(groupNode != nullptr);

      deselectAll(map);
      selectNodes(map, {groupNode});
      rotateSelection(
        map, map.selectionBounds()->center(), vm::vec3d{0, 0, 1}, vm::to_radians(90.0));

      CHECK(*entityNode->entity().property("angle") == "135");
      CHECK(!entityNode->entity().hasProperty("angles"));
    }

    SECTION("Rotating a group writes the angles of a light in it")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "light"},
        {"rotation", "0 1 0 -1 0 0 0 0 1"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});
      auto* groupNode = groupSelectedNodes(map, "group");
      REQUIRE(groupNode != nullptr);

      deselectAll(map);
      selectNodes(map, {groupNode});
      rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, vm::to_radians(-45.0));

      CHECK(*entityNode->entity().property("angles") == "0 45 0");
      CHECK(!entityNode->entity().hasProperty("rotation"));
    }

    SECTION("Redo after undo restores the rotated light exactly")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "light"},
        {"light_rotation", "0 1 0 -1 0 0 0 0 1"},
        {"rotation", "1 0 0 0 1 0 0 0 1"},
        {"light_angles", "0 0 0"},
        {"angles", "0 0 0"},
        {"angle", "0"},
      }}};
      const auto original = entityNode->entity();

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});
      rotateSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{0, 0, 1}, vm::to_radians(-45.0));

      const auto rotated = entityNode->entity();
      REQUIRE(
        rotated.properties()
        == std::vector<EntityProperty>{
          {"classname", "light"},
          {"angles", "0 45 0"},
        });

      map.undoCommand();
      CHECK(entityNode->entity().properties() == original.properties());

      map.redoCommand();
      CHECK(entityNode->entity() == rotated);
    }

    SECTION("Flipping a point entity mirrors its angles")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "some_point_entity"},
        {"angles", "0 30 0"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});

      // mirroring X turns the forward axis (cos 30, sin 30, 0) into (-cos 30, sin 30, 0)
      CHECK(flipSelection(map, vm::vec3d{0, 0, 0}, vm::axis::x));
      CHECK(*entityNode->entity().property("angles") == "0 150 0");
    }

    SECTION("Scaling a point entity converts a legacy rotation to angles")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "some_point_entity"},
        {"rotation", "0 1 0 -1 0 0 0 0 1"},
      }}};

      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      selectNodes(map, {entityNode});

      // a scale has a linear part, so the rotation is written again, unchanged
      CHECK(scaleSelection(map, vm::vec3d{0, 0, 0}, vm::vec3d{2, 2, 2}));
      CHECK(*entityNode->entity().property("angles") == "0 90 0");
      CHECK(!entityNode->entity().hasProperty("rotation"));
    }
  }

  SECTION("scaleSelection")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    const auto initialBBox = vm::bbox3d{{-100, -100, -100}, {100, 100, 100}};
    const auto doubleBBox = vm::bbox3d{2.0 * initialBBox.min, 2.0 * initialBBox.max};
    const auto invalidBBox = vm::bbox3d{{0, -100, -100}, {0, 100, 100}};

    auto* brushNode =
      new BrushNode{builder.createCuboid(initialBBox, "material") | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    REQUIRE(brushNode->logicalBounds().size() == vm::vec3d{200, 200, 200});
    REQUIRE(
      brushNode->brush().face(*brushNode->brush().findFace(vm::vec3d{0, 0, 1})).boundary()
      == vm::plane3d{100, vm::vec3d{0, 0, 1}});

    SECTION("single brush")
    {
      // attempting an invalid scale has no effect
      CHECK(!scaleSelection(map, initialBBox, invalidBBox));
      CHECK(brushNode->logicalBounds().size() == vm::vec3d{200, 200, 200});
      CHECK(
        brushNode->brush()
          .face(*brushNode->brush().findFace(vm::vec3d{0, 0, 1}))
          .boundary()
        == vm::plane3d{100, vm::vec3d{0, 0, 1}});

      CHECK(scaleSelection(map, initialBBox, doubleBBox));
      CHECK(brushNode->logicalBounds().size() == vm::vec3d{400, 400, 400});
      CHECK(
        brushNode->brush()
          .face(*brushNode->brush().findFace(vm::vec3d{0, 0, 1}))
          .boundary()
        == vm::plane3d{200, vm::vec3d{0, 0, 1}});
    }

    SECTION("in group")
    {
      [[maybe_unused]] auto* group = groupSelectedNodes(map, "my group");

      // attempting an invalid scale has no effect
      CHECK(!scaleSelection(map, initialBBox, invalidBBox));
      CHECK(brushNode->logicalBounds().size() == vm::vec3d{200, 200, 200});

      CHECK(scaleSelection(map, initialBBox, doubleBBox));
      CHECK(brushNode->logicalBounds().size() == vm::vec3d{400, 400, 400});
    }

    SECTION("with off center origin")
    {
      const auto origin = vm::vec3d{50, 0, 0};
      CHECK(scaleSelection(map, origin, vm::vec3d{2, 1, 1}));
      CHECK(
        brushNode->logicalBounds() == vm::bbox3d{{-250, -100, -100}, {150, 100, 100}});
    }
  }

  SECTION("shearSelection")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    SECTION("cube")
    {
      const auto initialBBox = vm::bbox3d{{100, 100, 100}, {200, 200, 200}};

      auto* brushNode =
        new BrushNode{builder.createCuboid(initialBBox, "material") | kdl::value()};

      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      CHECK_THAT(
        brushNode->brush().vertexPositions(),
        UnorderedEquals(std::vector<vm::vec3d>{
          // bottom face
          {100, 100, 100},
          {200, 100, 100},
          {200, 200, 100},
          {100, 200, 100},
          // top face
          {100, 100, 200},
          {200, 100, 200},
          {200, 200, 200},
          {100, 200, 200},
        }));

      // Shear the -Y face by (50, 0, 0). That means the verts with Y=100 will get
      // sheared.
      CHECK(shearSelection(map, initialBBox, vm::vec3d{0, -1, 0}, vm::vec3d{50, 0, 0}));

      CHECK_THAT(
        brushNode->brush().vertexPositions(),
        UnorderedEquals(std::vector<vm::vec3d>{
          // bottom face
          {150, 100, 100},
          {250, 100, 100},
          {200, 200, 100},
          {100, 200, 100},
          // top face
          {150, 100, 200},
          {250, 100, 200},
          {200, 200, 200},
          {100, 200, 200},
        }));
    }

    SECTION("pillar")
    {
      const auto initialBBox = vm::bbox3d{{0, 0, 0}, {100, 100, 400}};

      auto* brushNode =
        new BrushNode{builder.createCuboid(initialBBox, "material") | kdl::value()};

      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      CHECK_THAT(
        brushNode->brush().vertexPositions(),
        UnorderedEquals(std::vector<vm::vec3d>{
          // bottom face
          {0, 0, 0},
          {100, 0, 0},
          {100, 100, 0},
          {0, 100, 0},
          // top face
          {0, 0, 400},
          {100, 0, 400},
          {100, 100, 400},
          {0, 100, 400},
        }));

      // Shear the +Z face by (50, 0, 0). That means the verts with Z=400 will get
      // sheared.
      CHECK(shearSelection(map, initialBBox, vm::vec3d{0, 0, 1}, vm::vec3d{50, 0, 0}));

      CHECK_THAT(
        brushNode->brush().vertexPositions(),
        UnorderedEquals(std::vector<vm::vec3d>{
          // bottom face
          {0, 0, 0},
          {100, 0, 0},
          {100, 100, 0},
          {0, 100, 0},
          // top face
          {50, 0, 400},
          {150, 0, 400},
          {150, 100, 400},
          {50, 100, 400},
        }));
    }
  }

  SECTION("flipSelection")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode1 = new BrushNode{
      builder.createCuboid(vm::bbox3d{{0, 0, 0}, {30, 31, 31}}, "material")
      | kdl::value()};
    auto* brushNode2 = new BrushNode{
      builder.createCuboid(vm::bbox3d{{30, 0, 0}, {31, 31, 31}}, "material")
      | kdl::value()};

    CHECK(checkBrushIntegral(brushNode1));
    CHECK(checkBrushIntegral(brushNode2));

    addNodes(map, {{&parentForNodes(map), {brushNode1}}});
    addNodes(map, {{&parentForNodes(map), {brushNode2}}});

    selectNodes(map, {brushNode1, brushNode2});

    const auto boundsCenter = map.selectionBounds()->center();
    CHECK(boundsCenter == vm::approx{vm::vec3d{15.5, 15.5, 15.5}});

    flipSelection(map, boundsCenter, vm::axis::x);

    CHECK(checkBrushIntegral(brushNode1));
    CHECK(checkBrushIntegral(brushNode2));

    CHECK(brushNode1->logicalBounds() == vm::bbox3d{{1, 0, 0}, {31, 31, 31}});
    CHECK(brushNode2->logicalBounds() == vm::bbox3d{{0, 0, 0}, {1, 31, 31}});
  }

  SECTION("transformVertices")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    SECTION("no vertex gets deleted")
    {
      REQUIRE(
        transformVertices(
          map,
          {
            {-32, -32, 32},
            {-32, 32, 32},
            {32, -32, 32},
            {32, 32, 32},
          },
          vm::translation_matrix(vm::vec3d{0, 0, 32}))
        == TransformVerticesResult{true, true});

      CHECK(brushNode->brush().hasVertex({-32, -32, 64}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 64}));
      CHECK(brushNode->brush().hasVertex({32, -32, 64}));
      CHECK(brushNode->brush().hasVertex({32, 32, 64}));

      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));

      SECTION("Undo and redo")
      {
        map.undoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));

        map.redoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 64}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 64}));
        CHECK(brushNode->brush().hasVertex({32, -32, 64}));
        CHECK(brushNode->brush().hasVertex({32, 32, 64}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));
      }
    }

    SECTION("all vertices get deleted")
    {
      REQUIRE(
        transformVertices(
          map, {{32, 32, 32}}, vm::translation_matrix(vm::vec3d{-32, -32, -32}))
        == TransformVerticesResult{true, false});

      REQUIRE(brushNode->brush().vertexCount() == 7);
      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
    }
  }

  SECTION("transformEdges")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    SECTION("Edge transform is valid")
    {
      REQUIRE(transformEdges(
        map,
        {{{32, -32, 32}, {32, 32, 32}}},
        vm::translation_matrix(vm::vec3d{0, 0, 32})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 64}));
      CHECK(brushNode->brush().hasVertex({32, 32, 64}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));

      SECTION("undo and redo")
      {
        map.undoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));

        map.redoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({32, -32, 64}));
        CHECK(brushNode->brush().hasVertex({32, 32, 64}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));
      }
    }

    SECTION("Edge transform is invalid")
    {
      REQUIRE(!transformEdges(
        map,
        {{{32, -32, 32}, {32, 32, 32}}},
        vm::translation_matrix(vm::vec3d{-32, 0, -32})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
    }

    SECTION("Edge not found")
    {
      REQUIRE(transformEdges(
        map, {{{1, 2, 3}, {4, 5, 6}}}, vm::translation_matrix(vm::vec3d{-32, 0, -32})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
    }
  }

  SECTION("transformFaces")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    SECTION("Face transform is valid")
    {
      REQUIRE(transformFaces(
        map,
        {{
          {-32, -32, 32},
          {32, -32, 32},
          {32, 32, 32},
          {-32, 32, 32},
        }},
        vm::translation_matrix(vm::vec3d{0, 0, 32})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 64}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 64}));
      CHECK(brushNode->brush().hasVertex({32, -32, 64}));
      CHECK(brushNode->brush().hasVertex({32, 32, 64}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));

      SECTION("undo and redo")
      {
        map.undoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({32, -32, 32}));
        CHECK(brushNode->brush().hasVertex({32, 32, 32}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));

        map.redoCommand();

        CHECK(brushNode->brush().hasVertex({-32, -32, 64}));
        CHECK(brushNode->brush().hasVertex({-32, 32, 64}));
        CHECK(brushNode->brush().hasVertex({32, -32, 64}));
        CHECK(brushNode->brush().hasVertex({32, 32, 64}));
        CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
        CHECK(brushNode->brush().hasVertex({32, -32, -32}));
        CHECK(brushNode->brush().hasVertex({32, 32, -32}));
      }
    }

    SECTION("Face transform is invalid")
    {
      REQUIRE(!transformFaces(
        map,
        {{
          {-32, -32, 32},
          {32, -32, 32},
          {32, 32, 32},
          {-32, 32, 32},
        }},
        vm::translation_matrix(vm::vec3d{0, 0, -128})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
    }

    SECTION("Face not found")
    {
      REQUIRE(transformFaces(
        map,
        {{{1, 2, 3}, {4, 5, 6}, {7, 8, 9}}},
        vm::translation_matrix(vm::vec3d{-32, 0, -32})));

      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({32, -32, 32}));
      CHECK(brushNode->brush().hasVertex({32, 32, 32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
    }
  }

  SECTION("addVertex")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    SECTION("Vertex can be added")
    {
      REQUIRE(addVertex(map, {0, 0, 64}));

      CHECK(brushNode->brush().vertexCount() == 9);
      CHECK(brushNode->brush().hasVertex({0, 0, 64}));

      SECTION("Undo and redo")
      {
        map.undoCommand();

        CHECK(brushNode->brush().vertexCount() == 8);
        CHECK(!brushNode->brush().hasVertex({0, 0, 64}));

        map.redoCommand();

        CHECK(brushNode->brush().vertexCount() == 9);
        CHECK(brushNode->brush().hasVertex({0, 0, 64}));
      }
    }

    SECTION("Vertex cannot be added")
    {
      REQUIRE(!addVertex(map, {0, 0, 0}));

      CHECK(brushNode->brush().vertexCount() == 8);
    }
  }

  SECTION("removeVertices")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-32, -32, -32}, {32, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});
    selectNodes(map, {brushNode});

    SECTION("Remove single vertex")
    {
      REQUIRE(removeVertices(map, "Remove vertex", {{32, 32, 32}}));

      CHECK(brushNode->brush().vertexCount() == 7);
      CHECK(!brushNode->brush().hasVertex({32, 32, 32}));

      SECTION("Undo and redo")
      {
        map.undoCommand();

        CHECK(brushNode->brush().vertexCount() == 8);
        CHECK(brushNode->brush().hasVertex({32, 32, 32}));

        map.redoCommand();

        CHECK(brushNode->brush().vertexCount() == 7);
        CHECK(!brushNode->brush().hasVertex({32, 32, 32}));
      }
    }

    SECTION("Remove multiple vertices")
    {
      REQUIRE(removeVertices(
        map,
        "Remove vertex",
        {
          {32, 32, 32},
          {32, -32, 32},
          {-32, 32, 32},
        }));

      CHECK(brushNode->brush().vertexCount() == 5);
      CHECK(brushNode->brush().hasVertex({32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, 32, -32}));
      CHECK(brushNode->brush().hasVertex({-32, -32, 32}));
    }

    SECTION("Vertices cannot be removed")
    {
      REQUIRE(!removeVertices(
        map,
        "Remove vertex",
        {
          {32, 32, 32},
          {32, -32, 32},
          {-32, 32, 32},
          {-32, -32, 32},
        }));

      CHECK(brushNode->brush().vertexCount() == 8);
    }
  }

  SECTION("snapVertices")
  {
    // also run these at the hard world bounds of Doom 3 BFG
    const auto worldBounds = GENERATE(vm::bbox3d{8129.0}, vm::bbox3d{131072.0});
    auto& map = fixture.create({.worldBounds = worldBounds});

    SECTION("Don't crash when snapping vertices")
    {
      // see https://github.com/TrenchBroom/TrenchBroom/issues/2244
      selectAllNodes(map);
      removeSelectedNodes(map);

      const auto brush = R"(
// Game: Quake
// Format: Standard
// entity 0
{
"classname" "worldspawn"
// brush 0
{
( -96 -0 116 ) ( -96 -64 116 ) ( -96 -64 172 ) karch1 -0 -0 -0 1 1
( -96 -0 172 ) ( -96 -64 172 ) ( -116 -64 144 ) karch1 -84 176 -0 1 1
( -116 -64 144 ) ( -96 -64 116 ) ( -96 -0 116 ) karch_sup6 2 -64 -0 1 1
( -96 -0 116 ) ( -96 -0 172 ) ( -116 -0 144 ) karch1 -0 -0 -0 1 1
( -96 -64 172 ) ( -96 -64 116 ) ( -116 -64 144 ) karch1 -0 -0 -0 1 1
}
})";
      paste(map, brush);
      selectAllNodes(map);

      CHECK(map.selection().brushes.size() == 1u);
      CHECK_NOTHROW(snapVertices(map, map.grid().actualSize()));
    }

    SECTION("Linked groups")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/3768

      auto* brushNode = createBrushNode(map);
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      auto* groupNode = groupSelectedNodes(map, "test");
      REQUIRE(groupNode != nullptr);

      auto* linkedGroupNode = createLinkedDuplicate(map);
      REQUIRE(linkedGroupNode != nullptr);

      deselectAll(map);

      SECTION("Can't snap to grid with both groups selected")
      {
        selectNodes(map, {groupNode, linkedGroupNode});

        CHECK(
          transformSelection(map, "", vm::translation_matrix(vm::vec3d{0.5, 0.5, 0})));

        // This could generate conflicts, because what snaps one group could misalign
        // another group in the link set. So, just reject the change.
        CHECK(!snapVertices(map, 16));
      }
    }
  }

  SECTION("safeSnapVertices")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    const auto addBrush = [&](Brush brush) {
      auto* brushNode = new BrushNode{std::move(brush)};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      return brushNode;
    };

    SECTION("A brush on the grid is left as it is")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {128, 256, 64}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 16);
      CHECK(!report.refused());
      CHECK(!report.committed);
      CHECK(report.unchangedCount == 1u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("An off-grid box snaps as the stock snap does, in one undo step")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(
          vm::bbox3d{{0.3, 0.2, 0.1}, {64.4, 63.7, 32.2}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      auto stock = original;
      REQUIRE(stock.snapVertices(map.worldBounds(), 1.0));
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(report.snappedCount == 1u);
      CHECK(checkVerticesIntegral(brushNode));
      CHECK(brushNode->brush().faceCount() == 6u);
      CHECK(brushNode->brush().bounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 32}});
      CHECK_THAT(
        brushNode->brush().vertexPositions(),
        UnorderedEquals(stock.vertexPositions()));

      map.undoCommand();
      CHECK(brushNode->brush() == original);
      map.redoCommand();
      CHECK(checkVerticesIntegral(brushNode));
    }

    SECTION("A snap to a grid of 16 moves every vertex to a multiple of 16")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{3.3, 9, 0.5}, {70, 120.4, 40}}, "material")
        | kdl::value());
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 16);
      CHECK(report.committed);
      CHECK(brushNode->brush().faceCount() == 6u);
      CHECK(brushNode->brush().bounds() == vm::bbox3d{{0, 16, 0}, {64, 128, 48}});
    }

    SECTION("Half-way coordinates round away from zero, as the stock snap does")
    {
      // -2.5 rounds to -3; rounding half up would give -2, a box of zero thickness, and
      // refuse it. (The test needs the vertex at exactly -2.5: a plane's vertex can come out
      // a hair off a half, and then both roundings agree.)
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{-2.5, 0, 0}, {-1.6, 4, 4}}, "material")
        | kdl::value());
      REQUIRE(std::ranges::any_of(
        brushNode->brush().vertexPositions(), [](const auto& v) { return v.x() == -2.5; }));
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(brushNode->brush().bounds() == vm::bbox3d{{-3, 0, 0}, {-2, 4, 4}});
    }

    SECTION("A quad that would go non-planar refuses the whole snap")
    {
      // The top face is planar, but its rounded corners are not: the stock snap would
      // split it into two triangles.
      auto* tilted = addBrush(
        builder.createBrush(
          std::vector<vm::vec3d>{
            {0, 0, 0},
            {64, 0, 0},
            {64, 64, 0},
            {0, 64, 0},
            {0, 0, 32.4},
            {64, 0, 32.4 + 8.2},
            {0, 64, 32.4 + 4.3},
            {64, 64, 32.4 + 8.2 + 4.3},
          },
          "material")
        | kdl::value());
      auto* offGrid = addBrush(
        builder.createCuboid(
          vm::bbox3d{{200.3, 0.2, 0.1}, {264.4, 63.7, 32.2}}, "material")
        | kdl::value());
      REQUIRE(tilted->brush().faceCount() == 6u);
      const auto originalTilted = tilted->brush();
      const auto originalOffGrid = offGrid->brush();
      auto stock = originalTilted;
      REQUIRE(stock.snapVertices(map.worldBounds(), 1.0));
      CHECK(stock.faceCount() != 6u);
      selectNodes(map, {tilted, offGrid});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(!report.committed);
      CHECK(report.faceSetCount == 1u);
      CHECK(report.refusedBrushes == std::vector<BrushNode*>{tilted});
      CHECK(tilted->brush() == originalTilted);
      CHECK(offGrid->brush() == originalOffGrid);
    }

    SECTION("A brush thinner than the grid is refused")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 0.3}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.mergeCount == 1u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("A contact with an unselected brush that would open refuses the snap")
    {
      auto* lower = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64.4}}, "material")
        | kdl::value());
      auto* upper = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 64.4}, {64, 64, 128}}, "material")
        | kdl::value());
      const auto original = lower->brush();
      const auto originalUpper = upper->brush();
      selectNodes(map, {lower});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.refusedBrushes.empty());
      CHECK(report.openedContactCount == 1u);
      CHECK(report.openedContactBrushes == std::vector<BrushNode*>{lower});
      CHECK(lower->brush() == original);
      CHECK(upper->brush() == originalUpper);
    }

    SECTION("A contact between two selected brushes that stays closed lets both snap")
    {
      auto* lower = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64.4}}, "material")
        | kdl::value());
      auto* upper = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 64.4}, {64, 64, 128}}, "material")
        | kdl::value());
      selectNodes(map, {lower, upper});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(report.snappedCount == 2u);
      CHECK(lower->brush().bounds().max.z() == 64.0);
      CHECK(upper->brush().bounds().min.z() == 64.0);
    }

    SECTION("A terrain of triangle prisms snaps whole, as the stock snap does")
    {
      const auto brushes = makeTrianglePrisms(builder, 8, 64.0);
      auto brushNodes = std::vector<Node*>{};
      for (const auto& brush : brushes)
      {
        brushNodes.push_back(addBrush(brush));
      }
      selectNodes(map, brushNodes);

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(!report.refused());
      CHECK(report.snappedCount + report.unchangedCount == brushes.size());

      for (size_t i = 0; i < brushes.size(); ++i)
      {
        auto stock = brushes[i];
        REQUIRE(stock.snapVertices(map.worldBounds(), 1.0));
        const auto* brushNode = static_cast<const BrushNode*>(brushNodes[i]);
        CHECK(checkVerticesIntegral(brushNode));
        CHECK_THAT(
          brushNode->brush().vertexPositions(),
          UnorderedEquals(stock.vertexPositions()));
      }

      map.undoCommand();
      for (size_t i = 0; i < brushes.size(); ++i)
      {
        CHECK(static_cast<const BrushNode*>(brushNodes[i])->brush() == brushes[i]);
      }
    }

    SECTION("Sculpted quad tops: exactly the brushes stock would change are refused")
    {
      const auto brushes = makeTiltedBoxes(builder, 6, 64.0);
      auto brushNodes = std::vector<Node*>{};
      auto stockChanges = std::vector<BrushNode*>{};
      for (const auto& brush : brushes)
      {
        auto* brushNode = addBrush(brush);
        brushNodes.push_back(brushNode);
        if (!stockSnapKeepsFaces(brush, map.worldBounds(), 1.0))
        {
          stockChanges.push_back(brushNode);
        }
      }
      REQUIRE(!stockChanges.empty());
      REQUIRE(stockChanges.size() < brushes.size());
      selectNodes(map, brushNodes);

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(!report.committed);
      CHECK_THAT(report.refusedBrushes, UnorderedEquals(stockChanges));
      for (size_t i = 0; i < brushes.size(); ++i)
      {
        CHECK(static_cast<const BrushNode*>(brushNodes[i])->brush() == brushes[i]);
      }
    }

    SECTION("A contact with an unselected brush that crosses the origin planes is found")
    {
      // the octree keeps brushes crossing x, y or z = 0 in its root; the gather must still
      // pair them with their neighbours
      auto* lower = addBrush(
        builder.createCuboid(vm::bbox3d{{-64, -64, -64}, {64, 64, 64.4}}, "material")
        | kdl::value());
      auto* upper = addBrush(
        builder.createCuboid(vm::bbox3d{{-64, -64, 64.4}, {64, 64, 128}}, "material")
        | kdl::value());
      const auto originalUpper = upper->brush();
      selectNodes(map, {upper});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount == 1u);
      CHECK(upper->brush() == originalUpper);
      CHECK(lower->brush().bounds().max.z() == vm::approx{64.4});
    }

    SECTION("A T-junction contact with two unselected brushes survives")
    {
      auto* lower = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64.3, 64, 64}}, "material")
        | kdl::value());
      addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 64}, {32, 64, 128}}, "material")
        | kdl::value());
      addBrush(
        builder.createCuboid(vm::bbox3d{{32, 0, 64}, {64, 64, 128}}, "material")
        | kdl::value());
      selectNodes(map, {lower});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(report.openedContactCount == 0u);
      CHECK(lower->brush().bounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("A contact the snap would partly lose is refused")
    {
      // the upper brush ends at 64.3 on the lower one; rounding the lower one to 64 would
      // leave 0.3 of the upper brush's bottom over nothing
      auto* lower = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64.3, 64, 64}}, "material")
        | kdl::value());
      addBrush(
        builder.createCuboid(vm::bbox3d{{32, 0, 64}, {64.3, 64, 128}}, "material")
        | kdl::value());
      const auto original = lower->brush();
      selectNodes(map, {lower});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount == 1u);
      CHECK(lower->brush() == original);
    }

    SECTION("An overlap the snap would partly lose is refused")
    {
      // B's left face runs from x = 256.2 at y = 0 to 255.6 at y = 512 into A, which ends at
      // 256.3; rounding A to 256 would open a gap where y is below about 171
      auto* a = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {256.3, 512, 64}}, "material")
        | kdl::value());
      addBrush(
        builder.createBrush(
          std::vector<vm::vec3d>{
            {256.2, 0, 0},
            {255.6, 512, 0},
            {512, 0, 0},
            {512, 512, 0},
            {256.2, 0, 64},
            {255.6, 512, 64},
            {512, 0, 64},
            {512, 512, 64},
          },
          "material")
        | kdl::value());
      const auto original = a->brush();
      selectNodes(map, {a});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount == 1u);
      CHECK(a->brush() == original);
    }

    SECTION("Faces 0.005 apart count as a contact (the compiler seals them)")
    {
      auto* a = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {256.4, 64, 16}}, "material")
        | kdl::value());
      addBrush(
        builder.createCuboid(vm::bbox3d{{256.405, 0, 0}, {512, 64, 16}}, "material")
        | kdl::value());
      const auto original = a->brush();
      selectNodes(map, {a});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount == 1u);
      CHECK(a->brush() == original);
    }

    SECTION("Part of a terrain cannot be snapped where it meets the rest")
    {
      const auto brushes = makeTrianglePrisms(builder, 4, 64.0);
      auto half = std::vector<Node*>{};
      for (size_t i = 0; i < brushes.size(); ++i)
      {
        auto* brushNode = addBrush(brushes[i]);
        if (i < brushes.size() / 2)
        {
          half.push_back(brushNode);
        }
      }
      selectNodes(map, half);

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount > 0u);
    }

    SECTION("Two brushes that seal by an overlap must not come apart")
    {
      // A ends at 256.3 and B begins at 256.2: rounding A to 256 would open a 0.2 gap.
      auto* a = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {256.3, 64, 16}}, "material")
        | kdl::value());
      auto* b = addBrush(
        builder.createCuboid(vm::bbox3d{{256.2, 0, 0}, {512, 64, 16}}, "material")
        | kdl::value());
      const auto originalA = a->brush();
      selectNodes(map, {a});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.openedContactCount == 1u);
      CHECK(report.openedContactBrushes == std::vector<BrushNode*>{a});
      CHECK(a->brush() == originalA);

      SECTION("A seal thinner than 0.01 units is protected too")
      {
        // the shared piece's edges are shorter than the brush geometry's clean-up keeps
        deselectAll(map);
        auto* c = addBrush(
          builder.createCuboid(vm::bbox3d{{0, 200, 0}, {64.4, 264, 16}}, "material")
          | kdl::value());
        addBrush(
          builder.createCuboid(vm::bbox3d{{64.396, 200, 0}, {128, 264, 16}}, "material")
          | kdl::value());
        const auto originalC = c->brush();
        selectNodes(map, {c});

        const auto thin = safeSnapVertices(map, 1);
        CHECK(thin.refused());
        CHECK(thin.openedContactCount == 1u);
        CHECK(c->brush() == originalC);
      }

      SECTION("An overlap that survives the snap does not refuse it")
      {
        deselectAll(map);
        selectNodes(map, {b});
        // B begins at 256.2 and rounds to 256: it still overlaps A (0 to 256.3)
        const auto second = safeSnapVertices(map, 1);
        CHECK(second.committed);
        CHECK(b->brush().bounds().min.x() == 256.0);
      }
    }

    SECTION("Two brushes touching only along a line are not a contact")
    {
      // float noise in the vertices (z = 64.4 on both) must not make the line a contact
      auto* a = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 64.4}}, "material")
        | kdl::value());
      addBrush(
        builder.createCuboid(vm::bbox3d{{64, 0, 64.4}, {128, 64, 128}}, "material")
        | kdl::value());
      selectNodes(map, {a});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(report.openedContactCount == 0u);
    }

    SECTION("A brush inside a group with linked copies is refused")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(
          vm::bbox3d{{0.3, 0.2, 0.1}, {64.4, 63.7, 32.2}}, "material")
        | kdl::value());
      selectNodes(map, {brushNode});
      auto* groupNode = groupSelectedNodes(map, "test");
      REQUIRE(groupNode != nullptr);
      auto* linkedGroupNode = createLinkedDuplicate(map);
      REQUIRE(linkedGroupNode != nullptr);
      deselectAll(map);

      const auto original = brushNode->brush();
      selectNodes(map, {groupNode, linkedGroupNode});
      const auto report = safeSnapVertices(map, 1);
      CHECK(report.refused());
      CHECK(report.linkedCount == 2u);
      CHECK(brushNode->brush() == original);

      deselectAll(map);
      selectNodes(map, {groupNode});
      const auto single = safeSnapVertices(map, 1);
      CHECK(single.refused());
      CHECK(single.linkedCount == 1u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("A brush inside a group with no linked copy snaps")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(
          vm::bbox3d{{0.3, 0.2, 0.1}, {64.4, 63.7, 32.2}}, "material")
        | kdl::value());
      selectNodes(map, {brushNode});
      auto* groupNode = groupSelectedNodes(map, "test");
      REQUIRE(groupNode != nullptr);
      deselectAll(map);
      selectNodes(map, {groupNode});

      const auto report = safeSnapVertices(map, 1);
      CHECK(report.committed);
      CHECK(report.linkedCount == 0u);
      CHECK(brushNode->brush().bounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 32}});
    }

    SECTION("A grid below one unit is refused")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(
          vm::bbox3d{{0.3, 0.2, 0.1}, {64.4, 63.7, 32.2}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = safeSnapVertices(map, 0.5);
      CHECK(!report.error.empty());
      CHECK(report.refused());
      CHECK(brushNode->brush() == original);
    }
  }

  SECTION("chunkToGrid")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
    const auto addBrush = [&](Brush brush) {
      auto* brushNode = new BrushNode{std::move(brush)};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      makeOpaque(*brushNode);
      return brushNode;
    };
    const auto selectedBrushes = [&]() {
      return map.selection().brushes
             | std::views::transform([](const auto* node) { return node->brush(); })
             | kdl::ranges::to<std::vector>();
    };

    SECTION("A 1024 box becomes four pieces that keep its volume and planes")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(
          vm::bbox3d{{0, 0, 0}, {1024, 1024, 64}},
          "left",
          "right",
          "front",
          "back",
          "top",
          "bottom")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(report.committed);
      CHECK(report.chunkedCount == 1u);
      CHECK(report.pieceCount == 4u);
      CHECK(report.cutCount == 3u);
      CHECK(report.skippedNearCount == 0u);
      CHECK(report.skippedVolumeCount == 0u);
      CHECK(report.skippedFailedCount == 0u);

      const auto pieces = selectedBrushes();
      REQUIRE(pieces.size() == 4u);
      auto volume = 0.0;
      for (const auto& piece : pieces)
      {
        volume += brushVolume(piece);
        CHECK(keepsPlanes(piece, original, 512.0));
        CHECK(piece.bounds().size() == vm::vec3d{512, 512, 64});

        // A cut face takes the material of the brush face closest to it in direction.
        for (const auto& face : piece.faces())
        {
          if (face.normal() == vm::vec3d{1, 0, 0})
          {
            CHECK(face.materialName() == "right");
          }
          if (face.normal() == vm::vec3d{-1, 0, 0})
          {
            CHECK(face.materialName() == "left");
          }
          if (face.normal() == vm::vec3d{0, 1, 0})
          {
            CHECK(face.materialName() == "back");
          }
          if (face.normal() == vm::vec3d{0, -1, 0})
          {
            CHECK(face.materialName() == "front");
          }
        }
      }
      CHECK(volume == vm::approx{brushVolume(original), 1e-6 * brushVolume(original)});

      SECTION("A second chunk cuts nothing")
      {
        const auto second = chunkToGrid(map, 512);
        CHECK(!second.committed);
        CHECK(second.cutCount == 0u);
        CHECK(selectedBrushes().size() == 4u);
      }

      SECTION("Undo restores the brush in one step")
      {
        // the brush's own node holds the first piece; the other three are new nodes
        const auto pieceNodes = map.selection().brushes;
        REQUIRE(std::ranges::find(pieceNodes, brushNode) != pieceNodes.end());
        map.undoCommand();
        CHECK(brushNode->parent() != nullptr);
        CHECK(facetsOf(brushNode->brush()) == facetsOf(original));
        for (const auto* pieceNode : pieceNodes)
        {
          CHECK((pieceNode == brushNode || pieceNode->parent() == nullptr));
        }
      }
    }

    SECTION("A sloped brush keeps its planes and volume")
    {
      auto* brushNode = addBrush(
        builder.createBrush(
          std::vector<vm::vec3d>{
            {-300, -40, 0},
            {700, -40, 0},
            {-300, 600, 0},
            {700, 600, 0},
            {-300, -40, 100},
            {700, -40, 420},
            {-300, 600, 260},
            {700, 600, 580},
          },
          "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      // x = 0 and 512, y = 0 and 512 cross it: three columns by three rows.
      const auto report = chunkToGrid(map, 512);
      CHECK(report.committed);
      CHECK(report.pieceCount == 9u);
      CHECK(report.cutCount == 8u);
      // a cut vertex rebuilt a hair off its plane would show here as a near skip
      CHECK(report.skippedNearCount == 0u);
      CHECK(report.skippedFailedCount == 0u);

      const auto pieces = selectedBrushes();
      auto volume = 0.0;
      for (const auto& piece : pieces)
      {
        volume += brushVolume(piece);
        CHECK(keepsPlanes(piece, original, 512.0));
      }
      CHECK(volume == vm::approx{brushVolume(original), 1e-6 * brushVolume(original)});
    }

    SECTION("A brush on the grid lines is left as it is")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{512, 0, 0}, {1024, 512, 64}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.cutCount == 0u);
      CHECK(report.skippedNearCount == 0u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("A brush crossing only z = 512 is not cut")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {64, 64, 600}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.cutCount == 0u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("A cut that leaves a piece under one cubic unit is left out")
    {
      // the vertex at x = 512.75 is far enough from the plane, but the piece beyond it is
      // 0.75 by 1 by 1 units
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {512.75, 1, 1}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.skippedVolumeCount == 1u);
      CHECK(brushNode->brush() == original);
    }

    SECTION("A mixed selection: the brushes not cut stay selected beside the pieces")
    {
      auto* big = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {1024, 512, 64}}, "material")
        | kdl::value());
      auto* small = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 600, 0}, {64, 664, 64}}, "material")
        | kdl::value());
      const auto originalSmall = small->brush();
      selectNodes(map, {big, small});

      const auto report = chunkToGrid(map, 512);
      CHECK(report.committed);
      CHECK(report.chunkedCount == 1u);
      CHECK(report.pieceCount == 2u);
      CHECK(big->brush().bounds().size().x() == 512.0);
      CHECK(small->brush() == originalSmall);
      CHECK(map.selection().brushes.size() == 3u);
      CHECK(std::ranges::find(map.selection().brushes, small) != map.selection().brushes.end());
    }

    SECTION("A brush with a translucent face is left whole")
    {
      // static: the map refers to it until the map is destroyed, after this section. The
      // brush is set on the node directly; a command would look the material up again.
      static auto glass =
        gl::Material{"glass", gl::createTextureResource(gl::Texture{64, 64})};
      glass.setSurfaceParms({"coverage_translucent"});
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {1024, 1024, 64}}, "material")
        | kdl::value());
      auto glassBrush = brushNode->brush();
      glassBrush.face(0).setMaterial(&glass);
      brushNode->setBrush(std::move(glassBrush));
      REQUIRE(brushNode->brush().face(0).material() == &glass);
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.notOpaqueCount == 1u);
      CHECK(brushNode->parent() != nullptr);
    }

    SECTION("A brush with a missing material is left whole")
    {
      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {1024, 1024, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      REQUIRE(brushNode->brush().face(0).material() == nullptr);
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.notOpaqueCount == 1u);
    }

    SECTION("The pieces of a brush inside an entity stay in the entity")
    {
      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});
      auto* brushNode = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {1024, 1024, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{entityNode, {brushNode}}});
      makeOpaque(*brushNode);
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(report.committed);
      CHECK(entityNode->childCount() == 4u);
      for (const auto* piece : map.selection().brushes)
      {
        CHECK(piece->parent() == entityNode);
      }
    }

    SECTION("A cut within half a unit of a vertex is left out")
    {
      auto* brushNode = addBrush(
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {512.25, 64, 64}}, "material")
        | kdl::value());
      const auto original = brushNode->brush();
      selectNodes(map, {brushNode});

      const auto report = chunkToGrid(map, 512);
      CHECK(!report.committed);
      CHECK(report.skippedNearCount == 1u);
      CHECK(brushNode->brush() == original);
    }
  }

  SECTION("csgConvexMerge")
  {
    SECTION("Merge two brushes")
    {
      auto& map = fixture.create();
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      auto* brushNode1 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 64, 64}}, "material")
        | kdl::value()};
      auto* brushNode2 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{32, 0, 0}, {64, 64, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{entityNode, {brushNode1}}});
      addNodes(map, {{&parentForNodes(map), {brushNode2}}});
      CHECK(entityNode->children().size() == 1u);

      selectNodes(map, {brushNode1, brushNode2});
      CHECK(csgConvexMerge(map));
      REQUIRE(entityNode->children().size() == 1u);

      auto* brushNode3 = entityNode->children().front();
      CHECK(brushNode3->logicalBounds() == vm::bbox3d{{0, 0, 0}, {64, 64, 64}});
    }

    SECTION("Merge two faces")
    {
      auto& map = fixture.create();
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      auto* brushNode1 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 64, 64}}, "material")
        | kdl::value()};
      auto* brushNode2 = new BrushNode{
        builder.createCuboid(vm::bbox3d{{32, 0, 0}, {64, 64, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{entityNode, {brushNode1}}});
      addNodes(map, {{&parentForNodes(map), {brushNode2}}});
      REQUIRE(entityNode->children().size() == 1u);

      const auto oFace1Index = brushNode1->brush().findFace(vm::vec3d{-1, 0, 0});
      const auto oFace2Index = brushNode2->brush().findFace(vm::vec3d{+1, 0, 0});

      REQUIRE(oFace1Index);
      REQUIRE(oFace2Index);

      selectBrushFaces(map, {{brushNode1, *oFace1Index}, {brushNode2, *oFace2Index}});
      CHECK(csgConvexMerge(map));

      // added to the parent of the first brush, original brush is not deleted
      REQUIRE(entityNode->children().size() == 2u);

      auto* brushNode3 = entityNode->children().back();

      // check our assumption about the order of the entities' children
      REQUIRE(brushNode3 != brushNode1);
      REQUIRE(brushNode3 != brushNode2);

      const auto face1Verts = brushNode1->brush().face(*oFace1Index).vertexPositions();
      const auto face2Verts = brushNode2->brush().face(*oFace2Index).vertexPositions();

      const auto bounds =
        vm::merge(*vm::bbox3d::build(face1Verts), *vm::bbox3d::build(face2Verts));

      CHECK(brushNode3->logicalBounds() == bounds);
    }

    SECTION("Texture alignment")
    {
      auto& map = fixture.create({.mapFormat = MapFormat::Valve});
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      const auto texAlignmentSnapshot =
        UvCoordSystemSnapshot{vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};

      auto brush1 = builder.createCuboid(vm::bbox3d{{0, 0, 0}, {32, 64, 64}}, "material")
                    | kdl::value();
      brush1.face(*brush1.findFace(vm::vec3d{0, 0, 1}))
        .restoreUvCoordSystemSnapshot(texAlignmentSnapshot);

      auto brush2 = builder.createCuboid(vm::bbox3d{{32, 0, 0}, {64, 64, 64}}, "material")
                    | kdl::value();
      brush2.face(*brush2.findFace(vm::vec3d{0, 0, 1}))
        .restoreUvCoordSystemSnapshot(texAlignmentSnapshot);

      auto* brushNode1 = new BrushNode{std::move(brush1)};
      auto* brushNode2 = new BrushNode{std::move(brush2)};

      addNodes(map, {{entityNode, {brushNode1}}});
      addNodes(map, {{entityNode, {brushNode2}}});
      CHECK(entityNode->children().size() == 2u);

      selectNodes(map, {brushNode1, brushNode2});
      CHECK(csgConvexMerge(map));
      CHECK(entityNode->children().size() == 1u);

      auto* brushNode3 = static_cast<BrushNode*>(entityNode->children()[0]);
      const auto& brush3 = brushNode3->brush();

      const auto& top = brush3.face(*brush3.findFace(vm::vec3d{0, 0, 1}));
      CHECK(top.uAxis() == vm::vec3d{1, 0, 0});
      CHECK(top.vAxis() == vm::vec3d{0, 1, 0});
    }

    SECTION("Regression tests")
    {
      // A merge of these brushes previously crashed due to floating point imprecision
      // when computing the convex hull of their combined vertices.
      auto& map = fixture.load(
        "test/mdl/Map/csgConvexMergeCrash.map",
        {.mapFormat = MapFormat::Valve, .worldBounds = vm::bbox3d{32768.0}});

      REQUIRE(map.editorContext().currentLayer()->childCount() == 119);

      selectAllNodes(map);
      CHECK(csgConvexMerge(map));

      REQUIRE(map.editorContext().currentLayer()->childCount() == 1);
      auto* result =
        dynamic_cast<BrushNode*>(map.editorContext().currentLayer()->children().at(0));
      REQUIRE(result);
      CHECK(result->brush().fullySpecified());
    }
  }

  SECTION("csgSubtract")
  {
    SECTION("Subtract multiple brushes")
    {
      auto& map = fixture.create();
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      auto* minuendNode = new BrushNode{
        builder.createCuboid(
          vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{64, 64, 64}}, "material")
        | kdl::value()};
      auto* subtrahendNode1 = new BrushNode{
        builder.createCuboid(
          vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{32, 32, 64}}, "material")
        | kdl::value()};
      auto* subtrahendNode2 = new BrushNode{
        builder.createCuboid(
          vm::bbox3d{vm::vec3d{32, 32, 0}, vm::vec3d{64, 64, 64}}, "material")
        | kdl::value()};

      addNodes(map, {{entityNode, {minuendNode, subtrahendNode1, subtrahendNode2}}});
      CHECK(entityNode->children().size() == 3u);

      // we want to compute minuend - {subtrahendNode1, subtrahendNode2}
      selectNodes(map, {subtrahendNode1, subtrahendNode2});
      CHECK(csgSubtract(map));
      CHECK(entityNode->children().size() == 2u);

      auto* remainderNode1 = dynamic_cast<BrushNode*>(entityNode->children()[0]);
      auto* remainderNode2 = dynamic_cast<BrushNode*>(entityNode->children()[1]);
      CHECK(remainderNode1 != nullptr);
      CHECK(remainderNode2 != nullptr);

      const auto expectedBBox1 = vm::bbox3d{vm::vec3d{0, 32, 0}, vm::vec3d{32, 64, 64}};
      const auto expectedBBox2 = vm::bbox3d{vm::vec3d{32, 0, 0}, vm::vec3d{64, 32, 64}};

      if (remainderNode1->logicalBounds() != expectedBBox1)
      {
        std::swap(remainderNode1, remainderNode2);
      }

      CHECK(remainderNode1->logicalBounds() == expectedBBox1);
      CHECK(remainderNode2->logicalBounds() == expectedBBox2);
    }

    SECTION("Undo restores selection")
    {
      auto& map = fixture.create();
      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      auto* subtrahend1 = new BrushNode{
        builder.createCuboid(
          vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{64, 64, 64}}, "material")
        | kdl::value()};
      addNodes(map, {{entityNode, {subtrahend1}}});

      selectNodes(map, {subtrahend1});
      CHECK(csgSubtract(map));
      CHECK(entityNode->children().size() == 0u);
      CHECK(!map.selection().hasNodes());

      // check that the selection is restored after undo
      map.undoCommand();

      CHECK(map.selection().hasOnlyBrushes());
      CHECK_THAT(map.selection().brushes, Equals(std::vector<BrushNode*>{subtrahend1}));
    }

    SECTION("Texture alignment")
    {
      auto& map = fixture.create({.mapFormat = MapFormat::Valve});

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      const auto texAlignmentSnapshot =
        UvCoordSystemSnapshot{vm::vec3d{1, 0, 0}, vm::vec3d{0, 1, 0}};

      auto brush1 = builder.createCuboid(
                      vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{64, 64, 64}}, "material")
                    | kdl::value();
      auto brush2 = builder.createCuboid(
                      vm::bbox3d{vm::vec3d{0, 0, 0}, vm::vec3d{64, 64, 32}}, "material")
                    | kdl::value();
      brush2.face(*brush2.findFace(vm::vec3d{0, 0, 1}))
        .restoreUvCoordSystemSnapshot(texAlignmentSnapshot);

      auto* brushNode1 = new BrushNode{std::move(brush1)};
      auto* brushNode2 = new BrushNode{std::move(brush2)};

      addNodes(map, {{entityNode, {brushNode1}}});
      addNodes(map, {{entityNode, {brushNode2}}});
      CHECK(entityNode->children().size() == 2u);

      // we want to compute brush1 - brush2
      selectNodes(map, {brushNode2});
      CHECK(csgSubtract(map));
      CHECK(entityNode->children().size() == 1u);

      auto* brushNode3 = static_cast<BrushNode*>(entityNode->children()[0]);
      const auto& brush3 = brushNode3->brush();

      CHECK(
        brushNode3->logicalBounds()
        == vm::bbox3d{vm::vec3d{0, 0, 32}, vm::vec3d{64, 64, 64}});

      // the material alignment from the top of brush2 should have transferred
      // to the bottom face of brush3
      const auto& top = brush3.face(*brush3.findFace(vm::vec3d{0, 0, -1}));
      CHECK(top.uAxis() == vm::vec3d{1, 0, 0});
      CHECK(top.vAxis() == vm::vec3d{0, 1, 0});
    }

    SECTION("Regression tests")
    {
      auto& map = fixture.load(
        "test/mdl/Map/csgSubtractFailure.map", {.mapFormat = MapFormat::Valve});

      REQUIRE(map.editorContext().currentLayer()->childCount() == 2);
      auto* subtrahendNode =
        dynamic_cast<BrushNode*>(map.editorContext().currentLayer()->children().at(1));
      REQUIRE(subtrahendNode);
      REQUIRE(subtrahendNode->brush().findFace("clip").has_value());

      // select the second object in the default layer (a clip brush) and subtract
      selectNodes(map, {subtrahendNode});
      CHECK(csgSubtract(map));

      REQUIRE(map.editorContext().currentLayer()->childCount() == 1);
      auto* result =
        dynamic_cast<BrushNode*>(map.editorContext().currentLayer()->children().at(0));

      CHECK_THAT(
        result->brush().vertexPositions(),
        UnorderedApproxVecMatches(
          std::vector<vm::vec3d>{
            {-2852, 372, 248},
            {-2854, 372, 256},
            {-2854, 364, 256},
            {-2852, 364, 248},
            {-2840, 372, 248},
            {-2843.2, 372, 256},
            {-2843.2, 364, 256},
            {-2840, 364, 248}},
          0.001));
    }
  }

  SECTION("csgHollow")
  {
    auto& map =
      fixture.load("test/mdl/Map/csgHollow.map", {.mapFormat = MapFormat::Valve});

    REQUIRE(map.editorContext().currentLayer()->childCount() == 2);
    REQUIRE(!map.modified());

    SECTION("A brush too small to be hollowed doesn't block the command")
    {
      selectAllNodes(map);
      CHECK(csgHollow(map));

      // One cube is too small to hollow, so it's left untouched.
      // The other is hollowed into 6 brushes.
      CHECK(map.editorContext().currentLayer()->childCount() == 7);
      CHECK(map.modified());
    }

    SECTION("If no brushes are hollowed, the transaction isn't committed")
    {
      auto* smallBrushNode = map.editorContext().currentLayer()->children().at(0);
      selectNodes(map, {smallBrushNode});

      CHECK(!csgHollow(map));
      CHECK(map.editorContext().currentLayer()->childCount() == 2);
      CHECK(!map.modified());
    }
  }

  SECTION("extrudeBrushes")
  {
    auto& map = fixture.create();
    const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};

    auto* brushNode1 = new BrushNode{
      builder.createCuboid(vm::bbox3d{{-64, -32, -32}, {0, 32, 32}}, "material")
      | kdl::value()};

    auto* brushNode2 = new BrushNode{
      builder.createCuboid(vm::bbox3d{{0, -32, -32}, {64, 32, 32}}, "material")
      | kdl::value()};

    addNodes(map, {{&parentForNodes(map), {brushNode1, brushNode2}}});
    selectNodes(map, {brushNode1, brushNode2});

    SECTION("Extrude one brush")
    {
      REQUIRE(extrudeBrushes(
        map,
        {
          {
            {-64, -32, +32},
            {0, -32, +32},
            {0, +32, +32},
            {-64, +32, +32},
          },
        },
        {0, 0, 32}));

      CHECK(brushNode1->physicalBounds() == vm::bbox3d{{-64, -32, -32}, {0, +32, +64}});
      CHECK(brushNode2->physicalBounds() == vm::bbox3d{{0, -32, -32}, {+64, +32, +32}});

      SECTION("Undo and redo")
      {
        map.undoCommand();
        CHECK(brushNode1->physicalBounds() == vm::bbox3d{{-64, -32, -32}, {0, 32, 32}});
        CHECK(brushNode2->physicalBounds() == vm::bbox3d{{0, -32, -32}, {+64, +32, +32}});

        map.redoCommand();
        CHECK(brushNode1->physicalBounds() == vm::bbox3d{{-64, -32, -32}, {0, +32, +64}});
        CHECK(brushNode2->physicalBounds() == vm::bbox3d{{0, -32, -32}, {+64, +32, +32}});
      }
    }

    SECTION("Extrude two brushes")
    {
      REQUIRE(extrudeBrushes(
        map,
        {
          {
            {-64, -32, +32},
            {0, -32, +32},
            {0, +32, +32},
            {-64, +32, +32},
          },
          {
            {0, -32, +32},
            {+64, -32, +32},
            {+64, +32, +32},
            {0, +32, +32},
          },
        },
        {0, 0, 32}));

      CHECK(brushNode1->physicalBounds() == vm::bbox3d{{-64, -32, -32}, {0, +32, +64}});
      CHECK(brushNode2->physicalBounds() == vm::bbox3d{{0, -32, -32}, {+64, +32, +64}});
    }
  }
}

TEST_CASE("Map_Geometry.safeSnapTiming", "[.][safe-snap-timing]")
{
  // An 8192-unit terrain of 8,192 triangle prisms (64 by 64 cells of 128 units): the safe
  // snap against the stock snap on the same selection.
  auto fixture = MapFixture{};
  auto& map = fixture.create({.worldBounds = vm::bbox3d{131072.0}});
  const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
  auto brushNodes = std::vector<Node*>{};
  for (auto& brush : makeTrianglePrisms(builder, 64, 128.0))
  {
    brushNodes.push_back(new BrushNode{std::move(brush)});
  }
  addNodes(map, {{&parentForNodes(map), brushNodes}});
  selectNodes(map, brushNodes);

  const auto time = [](const auto& f) {
    const auto start = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
  };

  auto report = SafeSnapReport{};
  const auto safeMs = time([&]() { report = safeSnapVertices(map, 1); });
  CHECK(report.committed);
  map.undoCommand();
  auto stockDone = false;
  const auto stockMs = time([&]() { stockDone = snapVertices(map, 1); });
  CHECK(stockDone);
  WARN(
    "safe snap " << safeMs << " ms, stock snap " << stockMs << " ms, "
                 << brushNodes.size() << " prisms");
}

} // namespace tb::mdl
