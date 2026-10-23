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

// LP4.2a step 4 (C37, D79, D88, P4.6, P4.18): R_FindClosestEnvironmentProbes
// (tr_frontend_main.cpp:487-668) against hand values.

#include "ViewProbes.h"

#include <map>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// a probe at origin in area, its image keys named after the id, its SH's every
// coefficient ( id, 10 id, 100 id )
ProbeRecord probeAt(const PreviewId id, const EngineVec3& origin, const int area = 0)
{
  auto probe = ProbeRecord{};
  probe.id = id;
  probe.origin = origin;
  probe.areaNum = area;
  probe.globalProbeBounds = {
    EngineVec3{origin[0] - 8, origin[1] - 8, origin[2] - 8},
    EngineVec3{origin[0] + 8, origin[1] + 8, origin[2] + 8}};
  const auto name = std::to_string(id);
  for (std::size_t i = 0; i < 3; ++i)
  {
    probe.images[i].name = name + (i == 0 ? "_amb" : i == 1 ? "_spec" : "_depth");
  }
  for (auto& c : probe.shIrradiance)
  {
    c = EngineVec3{float(id), 10.0f * float(id), 100.0f * float(id)};
  }
  return probe;
}

ProbeTable tableOf(std::vector<ProbeRecord> records)
{
  auto table = ProbeTable{};
  table.version = 1;
  table.records = std::move(records);
  return table;
}

// every image Ready unless named otherwise
struct States
{
  std::map<std::string, ProbeImageState> states;

  ProbeImageStateOf of() const
  {
    return [this](const MaterialImageKey& key) {
      const auto it = states.find(key.name);
      return it != states.end() ? it->second : ProbeImageState::Ready;
    };
  }
};

const auto Connected = std::vector<bool>{true, true};
const auto Origin = EngineVec3{0.0f, 0.0f, 0.0f};

} // namespace

TEST_CASE("ViewProbes: the weights for 1 to 5 candidates (FM:585-634)")
{
  const auto states = States{};
  using Catch::Approx;

  SECTION("one candidate: dCut 1.5 d, normalised to ( 1, 0, 0 )")
  {
    const auto table = tableOf({probeAt(1, {10, 0, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.candidates == 1u);
    CHECK(view.blends[0] == Approx(1.0f));
    CHECK(view.blends[1] == 0.0f);
    CHECK(view.blends[2] == 0.0f);
    CHECK(view.blends[3] == 0.0f);
    CHECK(view.sh[0][0] == Approx(1.0f));
    CHECK(view.sh[8][2] == Approx(100.0f));
    CHECK(view.sh[0][3] == 0.0f);
  }

  SECTION("two: dCut 1.5 · 20 = 30, ( 2/3 )² and ( 1/3 )², normalised 0.8 and 0.2")
  {
    const auto table = tableOf({probeAt(2, {-20, 0, 0}), probeAt(1, {10, 0, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.candidates == 2u);
    CHECK(view.blends[0] == Approx(0.8f));
    CHECK(view.blends[1] == Approx(0.2f));
    CHECK(view.blends[2] == 0.0f);
    // the nearest (id 1) first: 0.8 · 1 + 0.2 · 2
    CHECK(view.sh[0][0] == Approx(1.2f));
    CHECK(view.sh[4][1] == Approx(12.0f));
    // the third position is the nearest's, w 0 (P4.18)
    CHECK(view.probePositions[0] == std::array<float, 4>{10, 0, 0, 1});
    CHECK(view.probePositions[1] == std::array<float, 4>{-20, 0, 0, 1});
    CHECK(view.probePositions[2] == std::array<float, 4>{10, 0, 0, 0});
  }

  SECTION("three: dCut 1.5 · 30 = 45")
  {
    const auto table =
      tableOf({probeAt(1, {10, 0, 0}), probeAt(2, {0, 20, 0}), probeAt(3, {0, 0, 30})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    // ( 35/45 )², ( 25/45 )², ( 15/45 )² = 1225, 625, 225 over 2075
    CHECK(view.blends[0] == Approx(1225.0f / 2075.0f));
    CHECK(view.blends[1] == Approx(625.0f / 2075.0f));
    CHECK(view.blends[2] == Approx(225.0f / 2075.0f));
    CHECK(view.probePositions[2] == std::array<float, 4>{0, 0, 30, 1});
  }

  SECTION("four: dCut the 4th's distance, 40")
  {
    const auto table = tableOf(
      {probeAt(1, {10, 0, 0}),
       probeAt(2, {20, 0, 0}),
       probeAt(3, {30, 0, 0}),
       probeAt(4, {40, 0, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.candidates == 4u);
    // 0.75², 0.5², 0.25² = 9, 4, 1 over 14; the fourth weighs nothing
    CHECK(view.blends[0] == Approx(9.0f / 14.0f));
    CHECK(view.blends[1] == Approx(4.0f / 14.0f));
    CHECK(view.blends[2] == Approx(1.0f / 14.0f));
    CHECK(view.sh[0][0] == Approx((9.0f * 1 + 4.0f * 2 + 1.0f * 3) / 14.0f));
  }

  SECTION("five: the same cutoff, the fifth ignored")
  {
    const auto table = tableOf(
      {probeAt(5, {50, 0, 0}),
       probeAt(1, {10, 0, 0}),
       probeAt(2, {20, 0, 0}),
       probeAt(3, {30, 0, 0}),
       probeAt(4, {40, 0, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.candidates == 5u);
    CHECK(view.blends[0] == Approx(9.0f / 14.0f));
    CHECK(view.blends[2] == Approx(1.0f / 14.0f));
    CHECK(view.radiance[0]->name == "1_spec");
    CHECK(view.radiance[2]->name == "3_spec");
  }

  SECTION("every weight 0: the degenerate ( 1, 0, 0 )")
  {
    // four at the same distance: dCut = d_3 = d_i, so every weight is 0
    const auto table = tableOf(
      {probeAt(1, {10, 0, 0}),
       probeAt(2, {-10, 0, 0}),
       probeAt(3, {0, 10, 0}),
       probeAt(4, {0, -10, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.blends == std::array<float, 4>{1, 0, 0, 0});
    CHECK(view.sh[0][0] == 1.0f);
  }
}

TEST_CASE("ViewProbes: the order, the early out and the connected areas (C37, K32)")
{
  const auto states = States{};

  SECTION("equal distances by id")
  {
    const auto table = tableOf({probeAt(5, {10, 0, 0}), probeAt(3, {-10, 0, 0})});
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.probePositions[0] == std::array<float, 4>{-10, 0, 0, 1});
    CHECK(view.radiance[0]->name == "3_spec");
    CHECK(view.radiance[1]->name == "5_spec");
    CHECK(view.globalProbeBounds == table.records[1].globalProbeBounds);
  }

  SECTION("the early out: view area -1, no tree")
  {
    const auto table = tableOf({probeAt(1, {10, 0, 0})});
    for (const auto& view :
         {chooseViewProbes(&table, Origin, -1, Connected, true, states.of()),
          chooseViewProbes(&table, Origin, 0, Connected, false, states.of()),
          chooseViewProbes(nullptr, Origin, 0, Connected, true, states.of())})
    {
      CHECK(view.candidates == 0u);
      CHECK(engineBoundsIsCleared(view.globalProbeBounds));
      CHECK(view.blends == std::array<float, 4>{1, 0, 0, 0});
      CHECK_FALSE(view.radiance[0]);
      CHECK_FALSE(view.depth);
      CHECK(view.probePositions[0] == std::array<float, 4>{0, 0, 0, 0});
      CHECK(view.sh[0] == std::array<float, 4>{0, 0, 0, 0});
    }
  }

  SECTION("an unconnected probe is dropped, one in area -1 kept")
  {
    const auto table = tableOf(
      {probeAt(1, {10, 0, 0}, 1), probeAt(2, {20, 0, 0}, -1), probeAt(3, {30, 0, 0}, 0)});
    const auto view = chooseViewProbes(
      &table, Origin, 0, std::vector<bool>{true, false}, true, states.of());
    CHECK(view.candidates == 2u);
    CHECK(view.radiance[0]->name == "2_spec");
    CHECK(view.radiance[1]->name == "3_spec");
    // an area past connectedAreas counts as not connected (a guard)
    const auto past = tableOf({probeAt(1, {10, 0, 0}, 7)});
    CHECK(
      chooseViewProbes(&past, Origin, 0, Connected, true, states.of()).candidates == 0u);
  }
}

TEST_CASE("ViewProbes: image states and the bounds (D88, FM:544-583, 638-667)")
{
  const auto table =
    tableOf({probeAt(1, {10, 0, 0}), probeAt(2, {20, 0, 0}), probeAt(3, {30, 0, 0})});

  SECTION("every image loaded: the nearest's bounds, every cube bound")
  {
    const auto states = States{};
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.globalProbeBounds == table.records[0].globalProbeBounds);
    CHECK(view.depth->name == "1_depth");
    CHECK(view.probeDepth[0]->name == "2_depth");
    CHECK(view.probeDepth[1]->name == "3_depth");
    CHECK_FALSE(view.imagesPending);
  }

  SECTION("Pending: the defaults stand and the frame waits; Missing: final")
  {
    auto pending = States{};
    pending.states["2_spec"] = ProbeImageState::Pending;
    pending.states["1_depth"] = ProbeImageState::Pending;
    const auto waiting =
      chooseViewProbes(&table, Origin, 0, Connected, true, pending.of());
    CHECK(waiting.imagesPending);
    CHECK_FALSE(waiting.radiance[1]);
    CHECK_FALSE(waiting.depth);
    CHECK(waiting.probePositions[0][3] == 0.0f);
    // the depth cube is the default: the bounds cleared (no march)
    CHECK(engineBoundsIsCleared(waiting.globalProbeBounds));

    auto missing = States{};
    missing.states["2_spec"] = ProbeImageState::Defaulted;
    missing.states["1_depth"] = ProbeImageState::Defaulted;
    const auto final = chooseViewProbes(&table, Origin, 0, Connected, true, missing.of());
    CHECK_FALSE(final.imagesPending);
    CHECK_FALSE(final.radiance[1]);
    CHECK(final.radiance[0]->name == "1_spec");
    CHECK_FALSE(final.depth);
    CHECK(engineBoundsIsCleared(final.globalProbeBounds));
  }

  SECTION("every radiance cube defaulted: the bounds cleared, depth kept")
  {
    auto states = States{};
    for (const auto* name : {"1_spec", "2_spec", "3_spec"})
    {
      states.states[name] = ProbeImageState::Defaulted;
    }
    const auto view = chooseViewProbes(&table, Origin, 0, Connected, true, states.of());
    CHECK(view.depth->name == "1_depth");
    CHECK(engineBoundsIsCleared(view.globalProbeBounds));
    // the weights do not depend on the images
    CHECK(view.blends[0] > view.blends[1]);
  }

  SECTION("a probe in area -1: its cleared bounds")
  {
    auto outside = tableOf({probeAt(1, {10, 0, 0}, -1)});
    outside.records[0].globalProbeBounds = engineClearedBounds();
    const auto states = States{};
    const auto view = chooseViewProbes(&outside, Origin, 0, Connected, true, states.of());
    CHECK(view.candidates == 1u);
    CHECK(engineBoundsIsCleared(view.globalProbeBounds));
  }
}

} // namespace tb::lit
