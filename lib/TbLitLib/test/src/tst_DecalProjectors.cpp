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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/geometry/DecalProjection.cpp,
// idlib/geometry/Winding.cpp, idlib/math/Plane.h, idlib/math/Angles.cpp and
// tools/compilers/dmap/dress_decal.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2026 Phillip Melnichenko
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/DecalProjectors.h"
#include "lit/EngineDecal.h"
#include "lit/ProcFile.h"

#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using Vec = std::array<double, 3>;

Vec add(const Vec& a, const Vec& b)
{
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vec sub(const Vec& a, const Vec& b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec mul(const double s, const Vec& a)
{
  return {s * a[0], s * a[1], s * a[2]};
}

double dot(const Vec& a, const Vec& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec cross(const Vec& a, const Vec& b)
{
  return {
    a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Vec unit(const Vec& a)
{
  return mul(1.0 / std::sqrt(dot(a, a)), a);
}

struct RefPlane
{
  Vec n = {0, 0, 0};
  double d = 0;
};

// idPlane::FromPoints( p1, p2, p3 ) (Plane.h:325-334): ( p1 - p2 ) x ( p3 - p2 ), through
// p2
RefPlane fromPoints(const Vec& p1, const Vec& p2, const Vec& p3)
{
  const auto n = unit(cross(sub(p1, p2), sub(p3, p2)));
  return {n, -dot(n, p2)};
}

/**
 * The engine's arithmetic, transcribed in double for the test: dress_decal.cpp:249-259's
 * far cap and DecalProjection.cpp:43-138's CreateParms (idWinding::GetPlane, Winding.cpp
 * :792-809: ( p1 - c ) x ( p0 - c ) through p0)
 */
struct Reference
{
  std::array<RefPlane, 6> planes;
  std::array<RefPlane, 2> fade;
  std::array<RefPlane, 2> texture;
  Vec projectionOrigin = {0, 0, 0};
  std::array<Vec, 2> bounds;
};

Reference reference(
  const Vec& origin,
  const std::array<Vec, 3>& axis,
  const double sizeX,
  const double sizeY,
  const double depth,
  const double fadeDepth,
  const bool parallel)
{
  const auto half = depth * 0.5;
  const auto capScale = parallel ? 0.5 : 1.0;
  const auto farCenter = sub(origin, mul(half, axis[0]));
  const auto right = mul(sizeX * capScale, axis[1]);
  const auto up = mul(sizeY * capScale, axis[2]);
  auto ref = Reference{};
  ref.projectionOrigin = add(origin, mul(half, axis[0]));
  const auto w = std::array<Vec, 4>{
    add(sub(farCenter, right), up),
    add(add(farCenter, right), up),
    sub(add(farCenter, right), up),
    sub(sub(farCenter, right), up)};
  const auto windingNormal = unit(cross(sub(w[1], farCenter), sub(w[0], farCenter)));
  const auto winding = RefPlane{windingNormal, -dot(windingNormal, w[0])};
  const auto distance = dot(winding.n, ref.projectionOrigin) + winding.d;
  ref.bounds = {w[0], w[0]};
  for (const auto& p : w)
  {
    for (auto c = std::size_t{0}; c < 3; ++c)
    {
      ref.bounds[0][c] = std::min(ref.bounds[0][c], p[c]);
      ref.bounds[1][c] = std::max(ref.bounds[1][c], p[c]);
    }
  }
  for (auto c = std::size_t{0}; c < 3; ++c)
  {
    if (parallel)
    {
      ref.bounds[0][c] -= distance;
      ref.bounds[1][c] += distance;
    }
    else
    {
      ref.bounds[0][c] = std::min(ref.bounds[0][c], ref.projectionOrigin[c]);
      ref.bounds[1][c] = std::max(ref.bounds[1][c], ref.projectionOrigin[c]);
    }
  }
  for (auto i = std::size_t{0}; i < 4; ++i)
  {
    const auto& a = w[i];
    const auto& b = w[(i + 1) % 4];
    if (parallel)
    {
      const auto n = unit(cross(winding.n, sub(b, a)));
      ref.planes[i] = RefPlane{n, -dot(n, a)};
    }
    else
    {
      ref.planes[i] = fromPoints(ref.projectionOrigin, a, b);
    }
  }
  ref.planes[4] = RefPlane{winding.n, winding.d - distance};
  ref.planes[5] = RefPlane{mul(-1.0, winding.n), -winding.d};
  ref.fade[0] = RefPlane{winding.n, winding.d - fadeDepth};
  ref.fade[1] = RefPlane{mul(-1.0, winding.n), -winding.d + distance - fadeDepth};
  // st ( 0 0 ) ( 1 0 ) ( 1 1 ) at w0, w1, w2: s along w1 - w0, t along w2 - w1, each
  // axis's length 1 over the edge's
  const auto s = sub(w[1], w[0]);
  const auto t = sub(w[2], w[1]);
  const auto sAxis = mul(1.0 / dot(s, s), s);
  const auto tAxis = mul(1.0 / dot(t, t), t);
  ref.texture[0] = RefPlane{sAxis, -dot(sAxis, w[0])};
  ref.texture[1] = RefPlane{tAxis, -dot(tAxis, w[0])};
  return ref;
}

void checkPlane(const EnginePlane& plane, const RefPlane& ref, const double tolerance)
{
  CHECK(std::abs(plane.a - ref.n[0]) < tolerance);
  CHECK(std::abs(plane.b - ref.n[1]) < tolerance);
  CHECK(std::abs(plane.c - ref.n[2]) < tolerance);
  CHECK(std::abs(plane.d - ref.d) < tolerance * 100);
}

void checkParms(const EngineDecalParms& parms, const Reference& ref)
{
  for (auto i = std::size_t{0}; i < 6; ++i)
  {
    CAPTURE(i);
    checkPlane(parms.boundingPlanes[i], ref.planes[i], 1e-5);
  }
  for (auto i = std::size_t{0}; i < 2; ++i)
  {
    CAPTURE(i);
    checkPlane(parms.fadePlanes[i], ref.fade[i], 1e-5);
    checkPlane(parms.textureAxis[i], ref.texture[i], 1e-6);
  }
  for (auto c = std::size_t{0}; c < 3; ++c)
  {
    CAPTURE(c);
    CHECK(std::abs(parms.projectionOrigin[c] - ref.projectionOrigin[c]) < 1e-4);
    CHECK(std::abs(parms.projectionBounds[0][c] - ref.bounds[0][c]) < 1e-3);
    CHECK(std::abs(parms.projectionBounds[1][c] - ref.bounds[1][c]) < 1e-3);
  }
}

// idAngles( pitch, yaw, roll ).ToMat3() (Angles.cpp), in double
std::array<Vec, 3> anglesAxis(const double pitch, const double yaw, const double roll)
{
  const auto rad = std::acos(-1.0) / 180.0;
  const auto sp = std::sin(pitch * rad), cp = std::cos(pitch * rad);
  const auto sy = std::sin(yaw * rad), cy = std::cos(yaw * rad);
  const auto sr = std::sin(roll * rad), cr = std::cos(roll * rad);
  return {
    Vec{cp * cy, cp * sy, -sp},
    Vec{sr * sp * cy + cr * -sy, sr * sp * sy + cr * cy, sr * cp},
    Vec{cr * sp * cy + -sr * -sy, cr * sp * sy + -sr * cy, cr * cp}};
}

std::array<Vec, 3> axisOf(const EngineMat3& m)
{
  return {Vec{m[0], m[1], m[2]}, Vec{m[3], m[4], m[5]}, Vec{m[6], m[7], m[8]}};
}

// renderer/Material.h:438 SURF_DISCRETE
constexpr auto SurfDiscrete = 1 << 10;

struct Materials
{
  MaterialEntry decal;
  MaterialEntry wall;
  std::map<std::string, EntityKeys> entities;

  Materials()
  {
    decal.name = "textures/lp73/decal";
    decal.surfaceFlags = SurfDiscrete;
    wall.name = "textures/lp73/wall";
  }

  DecalProjectorInputs inputs() const
  {
    return DecalProjectorInputs{
      [this](const std::string& name) -> std::optional<const MaterialEntry*> {
        if (name == "textures/lp73/decal")
        {
          return &decal;
        }
        if (name == "textures/lp73/wall")
        {
          return &wall;
        }
        if (name == "textures/lp73/pending")
        {
          return std::nullopt;
        }
        return nullptr;
      },
      [this](const std::string_view name) -> const EntityKeys* {
        const auto it = entities.find(std::string{name});
        return it != entities.end() ? &it->second : nullptr;
      }};
  }
};

EntityKeys decalKeys(EntityKeys extra)
{
  auto keys = EntityKeys{
    {"classname", "func_decal"},
    {"origin", "100 20 50"},
    {"material", "textures/lp73/decal"}};
  keys.insert(keys.end(), extra.begin(), extra.end());
  return keys;
}

} // namespace

/**
 * LP7.3 (C54, SC627-SC633): ParseDecalProjector transcribed onto the entity getters and
 * the copied idDecalProjection::CreateParms: the parms of hand projectors against the
 * engine's arithmetic transcribed above (the planes, the bounds for parallel and
 * perspective, the fade planes, the texture axes), every default, the clamp of fade, the
 * target's axis and its fallbacks, the refusals and their warnings, a moved target.
 */
TEST_CASE("DecalProjectors (LP7.3, C54)")
{
  const auto materials = Materials{};

  SECTION("engineMaterialIsDiscrete: Material.h:671-675")
  {
    auto material = MaterialEntry{};
    CHECK_FALSE(engineMaterialIsDiscrete(material));
    material.surfaceFlags = SurfDiscrete;
    CHECK(engineMaterialIsDiscrete(material));
    material = MaterialEntry{};
    material.deform = 1;
    CHECK(engineMaterialIsDiscrete(material));
    material = MaterialEntry{};
    material.sort = -3.0f;
    CHECK(engineMaterialIsDiscrete(material));
    material = MaterialEntry{};
    material.entityGui = 1;
    CHECK(engineMaterialIsDiscrete(material));
    material = MaterialEntry{};
    material.gui = "guis/test.gui";
    CHECK(engineMaterialIsDiscrete(material));
  }

  SECTION("the defaults: 64 64, depth 32, fade 8, perspective, angle 0")
  {
    const auto parse = parseDecalProjector("d", decalKeys({}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK(parse.warnings.empty());
    CHECK(parse.projector.material == "textures/lp73/decal");
    CHECK(parse.projector.materialKey == "textures/lp73/decal");
    CHECK_FALSE(parse.projector.aimed);
    const auto& parms = parse.projector.parms;
    CHECK_FALSE(parms.parallel);
    CHECK(parms.fadeDepth == 8.0f);
    checkParms(
      parms, reference({100, 20, 50}, anglesAxis(0, 0, 0), 64, 64, 32, 8, false));
    // perspective: the far cap twice decalSize, the projection origin 16 in front
    CHECK(parms.projectionBounds[0] == EngineVec3{84.0f, -44.0f, -14.0f});
    CHECK(parms.projectionBounds[1] == EngineVec3{116.0f, 84.0f, 114.0f});
    CHECK(parms.projectionOrigin == EngineVec3{116.0f, 20.0f, 50.0f});
  }

  SECTION("parallel: the edges' planes and the bounds grown by the depth")
  {
    const auto parse = parseDecalProjector(
      "d",
      decalKeys(
        {{"parallel", "1"}, {"decalSize", "40 24"}, {"depth", "20"}, {"angle", "90"}}),
      materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    const auto& parms = parse.projector.parms;
    CHECK(parms.parallel);
    checkParms(
      parms, reference({100, 20, 50}, anglesAxis(0, 90, 0), 40, 24, 20, 8, true));
  }

  SECTION("the axis: rotation over angles over angle, no modelscale")
  {
    auto parse = parseDecalProjector(
      "d",
      decalKeys(
        {{"rotation", "0 1 0 -1 0 0 0 0 1"}, {"angles", "30 0 0"}, {"angle", "45"}}),
      materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    checkParms(
      parse.projector.parms,
      reference(
        {100, 20, 50},
        {Vec{0, 1, 0}, Vec{-1, 0, 0}, Vec{0, 0, 1}},
        64,
        64,
        32,
        8,
        false));
    parse = parseDecalProjector(
      "d",
      decalKeys({{"angles", "-30 120 10"}, {"angle", "45"}, {"modelscale", "3"}}),
      materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    checkParms(
      parse.projector.parms,
      reference({100, 20, 50}, anglesAxis(-30, 120, 10), 64, 64, 32, 8, false));
    parse = parseDecalProjector(
      "d", decalKeys({{"angle", "210"}, {"modelscale", "3"}}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    checkParms(
      parse.projector.parms,
      reference({100, 20, 50}, anglesAxis(0, 210, 0), 64, 64, 32, 8, false));
  }

  SECTION("fade: clamped to 0 .. depth / 2, with the engine's warning")
  {
    auto parse =
      parseDecalProjector("d", decalKeys({{"fade", "20"}}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK(parse.projector.parms.fadeDepth == 16.0f);
    CHECK(
      parse.warnings
      == std::vector<std::string>{
        "func_decal 'd': fade 20 clamped to 16 (0 to depth/2 = 16)"});
    parse = parseDecalProjector(
      "d", decalKeys({{"fade", "-3"}, {"depth", "10"}}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK(parse.projector.parms.fadeDepth == 0.0f);
    CHECK(
      parse.warnings
      == std::vector<std::string>{
        "func_decal 'd': fade -3 clamped to 0 (0 to depth/2 = 5)"});
    parse = parseDecalProjector(
      "d", decalKeys({{"fade", "5"}, {"depth", "10"}}), materials.inputs());
    CHECK(parse.projector.parms.fadeDepth == 5.0f);
    CHECK(parse.warnings.empty());
  }

  SECTION("the target: its raw origin aims the projector, roll 0")
  {
    auto withTarget = materials;
    withTarget.entities["aim"] =
      EntityKeys{{"classname", "target_null"}, {"origin", "108 24 53"}};
    const auto keys = decalKeys({{"target", "aim"}, {"angle", "90"}});
    const auto parse = parseDecalProjector("d", keys, withTarget.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK(parse.projector.aimed);
    CHECK(parse.warnings.empty());
    // dress_decal.cpp:211-224: axis[0] = -dir, NormalVectors' left and -down
    const auto dir = unit(Vec{8, 4, 3});
    const auto a0 = mul(-1.0, dir);
    const auto d = std::sqrt(a0[0] * a0[0] + a0[1] * a0[1]);
    const auto left = Vec{-a0[1] / d, a0[0] / d, 0};
    const auto down = cross(left, a0);
    const auto axis = std::array<Vec, 3>{a0, left, mul(-1.0, down)};
    checkParms(
      parse.projector.parms, reference({100, 20, 50}, axis, 64, 64, 32, 8, false));
    const auto aimed = engineDecalTargetAxis({100, 20, 50}, {108, 24, 53});
    REQUIRE(aimed);
    const auto rows = axisOf(*aimed);
    for (auto r = std::size_t{0}; r < 3; ++r)
    {
      for (auto c = std::size_t{0}; c < 3; ++c)
      {
        CHECK(std::abs(rows[r][c] - axis[r][c]) < 1e-6);
      }
    }

    // a moved target: another projector
    auto moved = withTarget;
    moved.entities["aim"] =
      EntityKeys{{"classname", "target_null"}, {"origin", "108 12 47"}};
    const auto again = parseDecalProjector("d", keys, moved.inputs());
    REQUIRE(again.outcome == DecalProjectorOutcome::Ready);
    CHECK(
      again.projector.parms.projectionBounds != parse.projector.parms.projectionBounds);
  }

  SECTION("the target's fallbacks: not found, or at the origin: the angles, warned")
  {
    auto parse = parseDecalProjector(
      "d", decalKeys({{"target", "nowhere"}, {"angle", "90"}}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK_FALSE(parse.projector.aimed);
    CHECK(
      parse.warnings
      == std::vector<std::string>{
        "func_decal 'd': target 'nowhere' not found, using its angles"});
    checkParms(
      parse.projector.parms,
      reference({100, 20, 50}, anglesAxis(0, 90, 0), 64, 64, 32, 8, false));

    auto atOrigin = materials;
    atOrigin.entities["here"] = EntityKeys{{"origin", "100 20 50"}};
    parse = parseDecalProjector(
      "d", decalKeys({{"target", "here"}, {"angle", "90"}}), atOrigin.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    CHECK_FALSE(parse.projector.aimed);
    CHECK(
      parse.warnings
      == std::vector<std::string>{
        "func_decal 'd': target 'here' is at its origin, using its angles"});
    CHECK_FALSE(engineDecalTargetAxis({100, 20, 50}, {100, 20, 50}));
  }

  SECTION("the refusals: no projector, the engine's warning")
  {
    auto keys = decalKeys({});
    keys[2].second = "";
    auto parse = parseDecalProjector("d", keys, materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::NoMaterial);
    CHECK(parse.warnings == std::vector<std::string>{"func_decal 'd': no material"});

    keys[2].second = "textures/lp73/wall";
    parse = parseDecalProjector("d", keys, materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::NotDiscrete);
    CHECK(
      parse.warnings
      == std::vector<std::string>{"func_decal 'd': material 'textures/lp73/wall' is not "
                                  "discrete (use DECAL_MACRO), "
                                  "skipped"});

    // a name the table answers without an entry: FindMaterial's default, not discrete
    keys[2].second = "textures/lp73/broken";
    parse = parseDecalProjector("d", keys, materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::NotDiscrete);

    keys[2].second = "textures/lp73/pending";
    parse = parseDecalProjector("d", keys, materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::Pending);
    CHECK(parse.pendingMaterial == "textures/lp73/pending");
    CHECK(parse.warnings.empty());

    parse =
      parseDecalProjector("d", decalKeys({{"decalSize", "64 0"}}), materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::BadSize);
    CHECK(
      parse.warnings
      == std::vector<std::string>{"func_decal 'd': size 64 0 is not positive, skipped"});
    parse = parseDecalProjector("d", decalKeys({{"depth", "-1"}}), materials.inputs());
    CHECK(parse.outcome == DecalProjectorOutcome::BadDepth);
    CHECK(
      parse.warnings
      == std::vector<std::string>{"func_decal 'd': depth -1 is not positive, skipped"});
  }

  SECTION("engineDecalFadeColor: dress_decal.cpp:465-467, alpha only")
  {
    const auto parse =
      parseDecalProjector("d", decalKeys({{"angle", "180"}}), materials.inputs());
    REQUIRE(parse.outcome == DecalProjectorOutcome::Ready);
    const auto& parms = parse.projector.parms;
    // axis[0] = -x: the far cap at x 116, the fade plane at 108; 2 past it alpha 0.75
    CHECK(engineDecalFadeColor(parms, {100, 20, 50}) == 0xFFFFFFFFu);
    CHECK(engineDecalFadeColor(parms, {107, 20, 50}) == 0xFFFFFFFFu);
    CHECK(engineDecalFadeColor(parms, {110, 20, 50}) >> 24 == 191u);
    CHECK((engineDecalFadeColor(parms, {110, 20, 50}) & 0xFFFFFFu) == 0xFFFFFFu);
    CHECK(engineDecalFadeColor(parms, {116, 20, 50}) >> 24 == 0u);
  }

  SECTION("procSurfaceWithin: every vertex within the bounds grown")
  {
    auto surface = ProcSurface{};
    surface.numVerts = 2;
    surface.verts = {0, 0, 0, 0, 0, 0, 0, 1, 10.1f, 5, 5, 0, 0, 0, 0, 1};
    const auto bounds = EngineBounds{EngineVec3{0, 0, 0}, EngineVec3{10, 10, 10}};
    CHECK(procSurfaceWithin(surface, bounds, DressBoundsGrowth));
    CHECK_FALSE(procSurfaceWithin(surface, bounds, InlineBoundsGrowth));
    surface.verts[8] = 10.3f;
    CHECK_FALSE(procSurfaceWithin(surface, bounds, DressBoundsGrowth));
    surface.verts.clear();
    CHECK_FALSE(procSurfaceWithin(surface, bounds, DressBoundsGrowth));
    const auto nan = std::nanf("");
    surface.verts = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1};
    CHECK_FALSE(
      procSurfaceWithin(surface, EngineBounds{EngineVec3{nan, 0, 0}, bounds[1]}, 1.0f));
  }
}

} // namespace tb::lit
