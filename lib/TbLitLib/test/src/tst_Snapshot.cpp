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

#include "lit/Snapshot.h"

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

// C23: every record is plain data that copies and moves as bytes, holding indices, never
// a pointer into the document (D39); the snapshot itself moves without copying its arrays
template <typename T>
constexpr bool isPlainRecord =
  std::is_trivially_copyable_v<T> && std::is_trivially_move_constructible_v<T>
  && std::is_trivially_destructible_v<T> && std::is_standard_layout_v<T>;

static_assert(isPlainRecord<SnapshotRange>);
static_assert(isPlainRecord<SnapshotString>);
static_assert(isPlainRecord<SnapshotNode>);
static_assert(isPlainRecord<SnapshotFace>);
static_assert(isPlainRecord<SnapshotPatch>);
static_assert(isPlainRecord<SnapshotKeyValue>);
static_assert(isPlainRecord<SnapshotEntity>);
static_assert(std::is_same_v<PreviewId, std::uint64_t>);
static_assert(std::is_nothrow_move_constructible_v<Snapshot>);
static_assert(std::is_nothrow_move_assignable_v<Snapshot>);

// a string appended to the pool
SnapshotString addString(Snapshot& snapshot, const std::string& text)
{
  const auto offset = std::uint32_t(snapshot.strings.size());
  snapshot.strings += text;
  return SnapshotString{offset, std::uint32_t(text.size())};
}

} // namespace

TEST_CASE("Snapshot")
{
  auto snapshot = Snapshot{};
  snapshot.generation = 3;
  snapshot.sequence = 1;

  // a world brush with one triangle face, and its entity with two keys
  const auto classname = addString(snapshot, "worldspawn");
  const auto material = addString(snapshot, "common/caulk");
  snapshot.vertices = {vm::vec3d{0, 0, 0}, vm::vec3d{64, 0, 0}, vm::vec3d{0, 64, 0}};
  auto face = SnapshotFace{};
  face.polygon = SnapshotRange{0, 3};
  face.material = material;
  face.uAxis = vm::vec3f{1, 0, 0};
  face.vAxis = vm::vec3f{0, -1, 0};
  face.scale = vm::vec2f{0.5f, 0.5f};
  snapshot.faces.push_back(face);
  snapshot.keyValues.push_back(SnapshotKeyValue{
    addString(snapshot, "classname"), addString(snapshot, "worldspawn")});
  snapshot.keyValues.push_back(
    SnapshotKeyValue{addString(snapshot, "_color"), addString(snapshot, "1 1 1")});
  snapshot.entities.push_back(SnapshotEntity{1, classname, SnapshotRange{0, 2}});
  snapshot.nodes.push_back(
    SnapshotNode{2, 1, SnapshotNodeKind::Brush, true, SnapshotRange{0, 1}});
  snapshot.removed.push_back(7);

  SECTION("strings are views of the pool")
  {
    CHECK(snapshot.string(snapshot.entities[0].classname) == "worldspawn");
    CHECK(snapshot.string(snapshot.faces[0].material) == "common/caulk");
    const auto& keys = snapshot.entities[0].keys;
    CHECK(snapshot.string(snapshot.keyValues[keys.first + 1].key) == "_color");
    CHECK(snapshot.string(SnapshotString{}) == "");
  }

  SECTION("a record copies as bytes")
  {
    auto copy = SnapshotFace{};
    std::memcpy(&copy, &snapshot.faces[0], sizeof(SnapshotFace));
    CHECK(copy.polygon.count == 3u);
    CHECK(copy.uAxis == vm::vec3f{1, 0, 0});
    CHECK(copy.scale == vm::vec2f{0.5f, 0.5f});
    CHECK(snapshot.string(copy.material) == "common/caulk");
  }

  SECTION("handing it over moves its arrays, not their elements")
  {
    const auto* faces = snapshot.faces.data();
    const auto* vertices = snapshot.vertices.data();
    const auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
    CHECK(shared->faces.data() == faces);
    CHECK(shared->vertices.data() == vertices);
    CHECK(shared->generation == 3u);
    CHECK(shared->nodes[0].kind == SnapshotNodeKind::Brush);
    CHECK(shared->nodes[0].entity == shared->entities[0].id);
    CHECK(shared->removed == std::vector<PreviewId>{7});
  }
}

} // namespace tb::lit
