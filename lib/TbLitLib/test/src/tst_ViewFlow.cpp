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

// LP4.0 (C32, C33, C36, D81, D94): the view's portal flow on the LP1b fixture's tree and
// on synthetic trees, and the area predicates.

#include "LitMapFixture.h"
#include "LitTestUtils.h"
#include "ViewFlow.h"
#include "lit/AreaData.h"
#include "lit/ProcFile.h"

#include "kd/result.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

struct Fixture
{
  ProcTree tree;
  AreaData areas;
};

// the LP1b fixture's tree: area 0 the east room (x 8..256), area 1 the west room and the
// doorway (x -256..8), one portal on x = 8 (y -32..32, z 0..96), 39 nodes
const Fixture& fixture()
{
  static const auto loaded = [] {
    auto f = Fixture{};
    f.tree = readProcFile(readTextFile(
               litFixtureFolder() / "goldens" / "lp1b_geometry" / "lp1b_geometry.proc"))
             | kdl::if_error([](const auto& e) { FAIL(e.msg); }) | kdl::value();
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
    f.areas = buildAreaData(f.tree);
    return f;
  }();
  return loaded;
}

// a camera looking along direction, 128 × 96, TrenchBroom's fov 90 (fov_x 90, fov_y
// 73.74: tan( fov_y / 2 ) = 0.75)
LitView viewFrom(const EngineVec3& eye, const EngineVec3& direction)
{
  auto camera = CameraState{};
  camera.position = vm::vec3f{eye[0], eye[1], eye[2]};
  camera.direction = vm::vec3f{direction[0], direction[1], direction[2]};
  camera.up = vm::vec3f{0, 0, 1};
  camera.fovDegrees = 90.0f;
  return litViewOf(camera, 128, 96);
}

// a point light's inverseBaseLightProject (R_DeriveLightData)
EngineMat4 pointLight(const EngineVec3& origin, const float radius)
{
  auto light = EngineLightGeometry{};
  light.origin = origin;
  light.lightRadius = {radius, radius, radius};
  light.pointLight = true;
  return engineDeriveLightProjection(light).inverseBaseLightProject;
}

// an entity's inverseBaseModelProject: local bounds of half size about origin
EngineMat4 entityBox(const EngineVec3& origin, const float half)
{
  return engineOffsetScaleForBounds(
    engineMatrixFromOriginAxis(origin, {1, 0, 0, 0, 1, 0, 0, 0, 1}),
    {EngineVec3{-half, -half, -half}, EngineVec3{half, half, half}});
}

ViewFlowInputs inputsOf(const LitView& view, const ProcTree& tree, const AreaData& areas)
{
  auto inputs = ViewFlowInputs{};
  inputs.view = view;
  inputs.tree = &tree;
  inputs.areas = &areas;
  inputs.recordChainsForTesting = true;
  return inputs;
}

const ViewFlowChain* chainInto(const ViewFlowResult& result, const int area)
{
  const auto it = std::find_if(
    result.chainsForTesting.begin(),
    result.chainsForTesting.end(),
    [&](const ViewFlowChain& chain) { return chain.area == area; });
  return it != result.chainsForTesting.end() ? &*it : nullptr;
}

ScreenRect rectOf(const short x1, const short y1, const short x2, const short y2)
{
  auto rect = ScreenRect{};
  rect.x1 = x1;
  rect.y1 = y1;
  rect.x2 = x2;
  rect.y2 = y2;
  return rect;
}

// a vertical portal winding on x = x0, y -32..32, z 0..96, wound as the fixture's (its
// GetPlane normal +x: the positive area is on x > x0)
std::vector<std::array<float, 3>> portalAtX(const float x0, const float height = 96.0f)
{
  return {{x0, -32, 0}, {x0, -32, height}, {x0, 32, height}, {x0, 32, 0}};
}

} // namespace

TEST_CASE("ViewFlow: the flow on the LP1b fixture's tree (C32)")
{
  const auto& f = fixture();
  REQUIRE(f.tree.nodes.size() == 39u);
  REQUIRE(f.areas.areas.size() == 2u);

  SECTION("in area 0 facing the portal: area 1 through a rect smaller than the view")
  {
    // the eye 192 east of the doorway's plane, looking west: the portal (y -32..32, z
    // 0..96) is inside every frustum plane, so it is not clipped
    const auto result =
      flowView(inputsOf(viewFrom({200, 0, 64}, {-1, 0, 0}), f.tree, f.areas));
    CHECK(result.flowed);
    CHECK(result.viewArea == 0);
    CHECK(result.connectedAreas == std::vector<bool>{true, true});
    CHECK(result.areaFlowed == std::vector<bool>{true, true});
    CHECK(result.chains == 2u);
    CHECK(result.portalsVisited == 1u);
    // the view's chain has the 5 frustum planes and the whole scissor
    const auto* root = chainInto(result, 0);
    REQUIRE(root);
    CHECK(root->planes == 5u);
    CHECK(root->rect == rectOf(0, 0, 127, 95));
    // area 1's: the 4 edge planes through the eye and the portal's own (the stack keeps
    // only its new planes; the view's act through the clip)
    const auto* through = chainInto(result, 1);
    REQUIRE(through);
    CHECK(through->planes == 5u);
    // the rect by hand (screen right is +y): y -+32 at 192 ahead, ndc x -+1/6, window x
    // ( ndc · 0.5 + 0.5 ) · 127 = 52.92 and 74.08; z 0 and 96 are -64 and +32 from the
    // eye, ndc y -64 / 144 and 32 / 144, window y · 95 = 26.39 and 58.06; Ftoi, then
    // Expand
    CHECK(through->rect == rectOf(51, 25, 75, 59));
    CHECK(through->rect.area() < 128 * 96);
    // both world entities are seen: area 1's through the portal's rect
    REQUIRE(result.worldRects.size() == 2u);
    CHECK(result.worldRects[0] == rectOf(0, 0, 127, 95));
    CHECK(result.worldRects[1] == rectOf(51, 25, 75, 59));
  }

  SECTION("in area 0 facing away: the portal behind the eye is clipped away")
  {
    // the portal's plane still has the eye 192 in front of it (not the d < -0.1 skip):
    // the view's frustum planes leave nothing of its winding
    const auto result =
      flowView(inputsOf(viewFrom({200, 0, 64}, {1, 0, 0}), f.tree, f.areas));
    CHECK(result.areaFlowed == std::vector<bool>{true, false});
    CHECK(result.chains == 1u);
    CHECK(result.portalsVisited == 0u);
    CHECK_FALSE(result.worldRects[1]);
  }

  SECTION("outside the world: every area with the view's planes and scissor")
  {
    // east of the room, x > 256, is solid: PointInArea -1
    const auto result =
      flowView(inputsOf(viewFrom({400, 0, 64}, {-1, 0, 0}), f.tree, f.areas));
    CHECK(result.viewArea == -1);
    CHECK(result.connectedAreas == std::vector<bool>{true, true});
    CHECK(result.areaFlowed == std::vector<bool>{true, true});
    CHECK(result.chains == 2u);
    CHECK(result.portalsVisited == 0u);
    for (const auto& chain : result.chainsForTesting)
    {
      CHECK(chain.planes == 5u);
      CHECK(chain.rect == rectOf(0, 0, 127, 95));
    }
  }

  SECTION("within 1.0 of the portal's plane: the stack is copied, not clipped")
  {
    // 0.5 east of x = 8, in area 0, looking west: area 1 with the view's 5 planes and
    // its whole rect
    const auto result =
      flowView(inputsOf(viewFrom({8.5f, 0, 48}, {-1, 0, 0}), f.tree, f.areas));
    CHECK(result.viewArea == 0);
    const auto* through = chainInto(result, 1);
    REQUIRE(through);
    CHECK(through->planes == 5u);
    CHECK(through->rect == rectOf(0, 0, 127, 95));
    CHECK(result.portalsVisited == 1u);
  }

  SECTION("a light referencing area 1 alone gets a rect only through the chain")
  {
    // a point light of radius 40 at (-100 0 48): its box x -140..-60, y -40..40, z 8..88
    // lies inside the portal's edge planes seen from the eye
    const auto areas = std::vector<int>{1};
    const auto lights = std::vector<ViewFlowLight>{
      ViewFlowLight{7, 1, areas, pointLight({-100, 0, 48}, 40), false}};
    auto inputs = inputsOf(viewFrom({200, 0, 64}, {-1, 0, 0}), f.tree, f.areas);
    inputs.lights = lights;
    const auto result = flowView(inputs);
    REQUIRE(result.lightRects.contains(7));
    CHECK(result.lightRects.at(7) == chainInto(result, 1)->rect);
    CHECK(result.lightRects.at(7) == rectOf(51, 25, 75, 59));

    // facing away, area 1 is not flowed: no rect
    inputs.view = viewFrom({200, 0, 64}, {1, 0, 0});
    CHECK_FALSE(flowView(inputs).lightRects.contains(7));
  }

  SECTION("the world entity's rect is absent when its area's chain rect is empty (P4.29)")
  {
    auto inputs = inputsOf(viewFrom({200, 0, 64}, {-1, 0, 0}), f.tree, f.areas);
    auto empty = ScreenRect{};
    empty.clear();
    inputs.scissorForTesting = empty;
    const auto result = flowView(inputs);
    // the area is entered (marked) but its world entity is not directly visible
    CHECK(result.areaFlowed[0]);
    CHECK_FALSE(result.worldRects[0]);
    // and with the view's scissor it is
    inputs.scissorForTesting = std::nullopt;
    CHECK(flowView(inputs).worldRects[0] == rectOf(0, 0, 127, 95));
  }

  SECTION("brush entities: culled by the chain's corners or seen through it")
  {
    // behind the eye (all corners beyond the near plane) and astride the eye (some on
    // each side of every plane), both referencing area 0
    const auto areas = std::vector<int>{0};
    const auto entities = std::vector<ViewFlowEntity>{
      ViewFlowEntity{1, areas, entityBox({240, 0, 64}, 16)},
      ViewFlowEntity{2, areas, entityBox({200, 0, 64}, 16)}};
    auto inputs = inputsOf(viewFrom({200, 0, 64}, {-1, 0, 0}), f.tree, f.areas);
    inputs.entities = entities;
    const auto result = flowView(inputs);
    CHECK_FALSE(result.entityRects.contains(1));
    REQUIRE(result.entityRects.contains(2));
    CHECK(result.entityRects.at(2) == rectOf(0, 0, 127, 95));
  }
}

TEST_CASE("ViewFlow: the flow's rules on synthetic trees (C32, P4.23)")
{
  SECTION("a portal whose plane has the eye more than 0.1 behind it is skipped")
  {
    // every point is in area 0 (one node, both children area 0); the portal on x = 100
    // has area 0 on its positive side (x > 100)
    auto tree = ProcTree{};
    tree.numAreas = 2;
    tree.portals.push_back(ProcPortal{0, 1, portalAtX(100)});
    tree.nodes.push_back(ProcNode{{0, 0, 1, -1000}, {-1, -1}});
    const auto areas = buildAreaData(tree);
    // from x 50 its plane is 50 behind the eye: skipped, though the portal is in view
    const auto behind = flowView(inputsOf(viewFrom({50, 0, 48}, {1, 0, 0}), tree, areas));
    CHECK(behind.viewArea == 0);
    CHECK(behind.areaFlowed == std::vector<bool>{true, false});
    // from x 150 it is 50 in front: flowed
    const auto front =
      flowView(inputsOf(viewFrom({150, 0, 48}, {-1, 0, 0}), tree, areas));
    CHECK(front.areaFlowed == std::vector<bool>{true, true});
  }

  SECTION("a ring of three areas: a portal side on the stack is not entered again")
  {
    // three portals on half-planes about the vertical axis through the eye, at 0, 120 and
    // 240 degrees: the eye is on every portal's plane (d < 1.0, copied stacks)
    auto tree = ProcTree{};
    tree.numAreas = 3;
    const auto spoke = [](const float degrees) {
      const auto a = degrees * 3.14159265f / 180.0f;
      const auto x = 100.0f * std::cos(a);
      const auto y = 100.0f * std::sin(a);
      return std::vector<std::array<float, 3>>{
        {0, 0, 0}, {x, y, 0}, {x, y, 96}, {0, 0, 96}};
    };
    tree.portals.push_back(ProcPortal{0, 1, spoke(0)});
    tree.portals.push_back(ProcPortal{1, 2, spoke(120)});
    tree.portals.push_back(ProcPortal{2, 0, spoke(240)});
    tree.nodes.push_back(ProcNode{{0, 0, 1, -1000}, {-1, -1}});
    const auto areas = buildAreaData(tree);
    const auto result = flowView(inputsOf(viewFrom({0, 0, 48}, {1, 0, 0}), tree, areas));
    CHECK(result.areaFlowed == std::vector<bool>{true, true, true});
    // each of the six sides enters a stack at most once: the chains stay few
    CHECK(result.chains < 64u);
    CHECK(result.portalsVisited < 64u);
  }

  SECTION("a 300-area corridor: 299 portals in a chain, the stack on the heap")
  {
    // areas i at x 64 i .. 64 ( i + 1 ), portal i on x = 64 ( i + 1 ) with area i + 1 on
    // its positive side; nodes: x > 64 ( k + 1 ) goes on to node k + 1, else area k
    constexpr auto Areas = 300;
    auto tree = ProcTree{};
    tree.numAreas = Areas;
    for (auto i = 0; i + 1 < Areas; ++i)
    {
      tree.portals.push_back(ProcPortal{i + 1, i, portalAtX(64.0f * float(i + 1), 64)});
      tree.nodes.push_back(ProcNode{
        {1, 0, 0, -64.0f * float(i + 1)},
        {i + 2 < Areas ? i + 1 : -1 - (i + 1), -1 - i}});
    }
    const auto areas = buildAreaData(tree);
    // from the corridor's start, looking down it: each portal is inside the last one's
    // planes
    const auto result = flowView(inputsOf(viewFrom({2, 0, 32}, {1, 0, 0}), tree, areas));
    CHECK(result.viewArea == 0);
    CHECK(
      std::all_of(result.areaFlowed.begin(), result.areaFlowed.end(), [](const bool b) {
        return b;
      }));
    CHECK(result.chains == std::size_t(Areas));
    CHECK(result.portalsVisited == std::size_t(Areas - 1));
    // the depth bound: each portal side enters a stack once
    CHECK(result.portalsVisited <= 2u * tree.portals.size());
  }

  SECTION("light area culling: a non-shadowing light of an unconnected area is dropped")
  {
    // two areas split by x = 0 and no portal: two components; the eye in area 0
    auto tree = ProcTree{};
    tree.numAreas = 2;
    tree.nodes.push_back(ProcNode{{1, 0, 0, 0}, {-1, -2}});
    const auto areas = buildAreaData(tree);
    CHECK(areas.components == std::vector<int>{0, 1});
    // both lights stand in area 1 and reach into area 0 (radius 200 about x -50)
    const auto refs = std::vector<int>{0, 1};
    const auto volume = pointLight({-50, 0, 48}, 200);
    const auto lights = std::vector<ViewFlowLight>{
      ViewFlowLight{1, 1, refs, volume, false}, ViewFlowLight{2, 1, refs, volume, true}};
    auto inputs = inputsOf(viewFrom({100, 0, 48}, {-1, 0, 0}), tree, areas);
    inputs.lights = lights;
    const auto result = flowView(inputs);
    CHECK(result.connectedAreas == std::vector<bool>{true, false});
    CHECK_FALSE(result.lightRects.contains(1));
    REQUIRE(result.lightRects.contains(2));
    CHECK(result.lightRects.at(2) == rectOf(0, 0, 127, 95));
  }

  SECTION("CullLightByPortals: every corner in front of one plane culls")
  {
    auto tree = ProcTree{};
    tree.numAreas = 1;
    tree.nodes.push_back(ProcNode{{1, 0, 0, 0}, {-1, -1}});
    const auto areas = buildAreaData(tree);
    const auto refs = std::vector<int>{0};
    // the eye at x 100 looking west: a light behind it (x 250..350, beyond the near
    // plane) and one about the eye
    const auto lights = std::vector<ViewFlowLight>{
      ViewFlowLight{1, 0, refs, pointLight({300, 0, 48}, 50), true},
      ViewFlowLight{2, 0, refs, pointLight({100, 0, 48}, 50), true}};
    auto inputs = inputsOf(viewFrom({100, 0, 48}, {-1, 0, 0}), tree, areas);
    inputs.lights = lights;
    const auto result = flowView(inputs);
    CHECK_FALSE(result.lightRects.contains(1));
    CHECK(result.lightRects.contains(2));
  }

  SECTION("no tree: nothing is flowed (D94)")
  {
    auto inputs = ViewFlowInputs{};
    inputs.view = viewFrom({0, 0, 0}, {1, 0, 0});
    const auto none = flowView(inputs);
    CHECK_FALSE(none.flowed);
    CHECK(none.viewArea == -1);
    CHECK(none.connectedAreas.empty());
    CHECK(none.areaFlowed.empty());
    CHECK(none.worldRects.empty());
    CHECK(none.entityRects.empty());
    CHECK(none.lightRects.empty());
    // a tree without nodes, or without its AreaData, counts as none
    const auto empty = ProcTree{};
    const auto emptyAreas = AreaData{};
    inputs.tree = &empty;
    inputs.areas = &emptyAreas;
    CHECK_FALSE(flowView(inputs).flowed);
    inputs.tree = &fixture().tree;
    inputs.areas = nullptr;
    CHECK_FALSE(flowView(inputs).flowed);
  }
}

TEST_CASE("ViewFlow: the corner cull and the connected areas")
{
  // the box x 10..12 against x = 5 (outside where x > 5): every corner in front
  const auto corners = engineFrustumCorners(
    engineOffsetScaleForBounds(
      engineIdentityMatrix(), {EngineVec3{10, -1, -1}, EngineVec3{12, 1, 1}}),
    engineUnitCube());
  const auto outside = std::vector<EnginePlane>{{0, 0, 1, -100}, {1, 0, 0, -5}};
  CHECK(engineCullCornersByPortals(corners, outside));
  // against x = 11 it straddles, against x = 20 it is behind: kept
  const auto straddling = std::vector<EnginePlane>{{1, 0, 0, -11}, {1, 0, 0, -20}};
  CHECK_FALSE(engineCullCornersByPortals(corners, straddling));
  CHECK_FALSE(engineCullCornersByPortals(corners, {}));

  const auto& f = fixture();
  CHECK(engineBuildConnectedAreas(f.areas, 0) == std::vector<bool>{true, true});
  CHECK(engineBuildConnectedAreas(f.areas, -1) == std::vector<bool>{true, true});
  auto split = ProcTree{};
  split.numAreas = 3;
  split.portals.push_back(ProcPortal{0, 1, portalAtX(0)});
  const auto splitAreas = buildAreaData(split);
  CHECK(
    engineBuildConnectedAreas(splitAreas, 2) == std::vector<bool>{false, false, true});
  CHECK(engineBuildConnectedAreas(splitAreas, 1) == std::vector<bool>{true, true, false});
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST_CASE("ViewFlow: 100 flows on the debug heap (P4.23)")
{
  const auto& f = fixture();
  const auto refs = std::vector<int>{1};
  const auto lights = std::vector<ViewFlowLight>{
    ViewFlowLight{7, 1, refs, pointLight({-100, 0, 48}, 40), false}};
  auto inputs = inputsOf(viewFrom({200, 0, 64}, {-1, 0, 0}), f.tree, f.areas);
  inputs.lights = lights;
  inputs.recordChainsForTesting = false;
  static_cast<void>(flowView(inputs));
  CHECK_FALSE(heapGrows([&] {
    for (auto i = 0; i < 100; ++i)
    {
      const auto result = flowView(inputs);
      REQUIRE(result.lightRects.size() == 1u);
    }
  }));
}
#endif

} // namespace tb::lit
