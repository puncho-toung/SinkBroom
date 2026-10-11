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

#include "mdl/BrushBuilder.h"
#include "mdl/BrushNode.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_CopyPaste.h"
#include "mdl/Map_Entities.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Groups.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "mdl/PasteType.h"
#include "mdl/PatchNode.h"
#include "mdl/TestFactory.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

namespace tb::mdl
{
namespace
{

MapFixtureConfig makeNamingFixtureConfig()
{
  auto fixtureConfig = MapFixtureConfig{};
  fixtureConfig.gameInfo.gameConfig.entityConfig.naming = EntityNamingConfig{
    .nameKey = "name",
    .brushEntityModelKey = "model",
  };
  return fixtureConfig;
}

void setLinkDefinitions(Map& map)
{
  map.entityDefinitionManager().setDefinitions({
    {"light",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
       PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
     },
     PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
    {"trigger_once",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
       PropertyDefinition{"target", PropertyValueTypes::LinkSource{}, "", ""},
     }},
  });
}

std::vector<std::pair<std::string, std::string>> entityNamesAndTargets(const Map& map)
{
  auto result = std::vector<std::pair<std::string, std::string>>{};
  for (const auto* node : map.worldNode().defaultLayer()->children())
  {
    if (const auto* entityNode = dynamic_cast<const EntityNode*>(node))
    {
      const auto* name = entityNode->entity().property("name");
      const auto* target = entityNode->entity().property("target");
      result.emplace_back(name ? *name : "", target ? *target : "");
    }
  }
  return result;
}

const auto LightAndTrigger = std::string{R"(
{
"classname" "light"
"name" "light_0"
"origin" "0 0 0"
}
{
"classname" "trigger_once"
"name" "trigger_once_0"
"target" "light_0"
{
( -0 -0 -16 ) ( -0 -0  -0 ) ( 64 -0 -16 ) tex1 0 0 0 1 1
( -0 -0 -16 ) ( -0 64 -16 ) ( -0 -0  -0 ) tex2 0 0 0 1 1
( -0 -0 -16 ) ( 64 -0 -16 ) ( -0 64 -16 ) tex3 0 0 0 1 1
( 64 64  -0 ) ( -0 64  -0 ) ( 64 64 -16 ) tex4 0 0 0 1 1
( 64 64  -0 ) ( 64 64 -16 ) ( 64 -0  -0 ) tex5 0 0 0 1 1
( 64 64  -0 ) ( 64 -0  -0 ) ( -0 64  -0 ) tex6 0 0 0 1 1
}
}
)"};

const auto TriggerAlone = std::string{R"(
{
"classname" "trigger_once"
"name" "trigger_once_0"
"target" "light_0"
}
)"};

} // namespace

TEST_CASE("Map_CopyPaste")
{
  auto fixture = MapFixture{};

  SECTION("serializeSelectedNodes")
  {
    auto& map = fixture.create();

    const auto builder = BrushBuilder{
      map.worldNode().mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto* brushNode = new BrushNode{builder.createCube(64.0, "some_material").value()};
    auto* entityNode = new EntityNode{Entity{{{"some_key", "some_value"}}}};

    addNodes(map, {{&parentForNodes(map), {brushNode, entityNode}}});

    SECTION("nothing is selected")
    {
      CHECK(serializeSelectedNodes(map) == R"()");
    }

    SECTION("entity is selected")
    {
      selectNodes(map, {entityNode});

      CHECK(serializeSelectedNodes(map) == R"(// entity 0
{
"some_key" "some_value"
}
)");
    }

    SECTION("two nodes are selected")
    {
      selectNodes(map, {entityNode, brushNode});

      CHECK(serializeSelectedNodes(map) == R"(// entity 0
{
"classname" "worldspawn"
// brush 0
{
( -32 -32 -32 ) ( -32 -31 -32 ) ( -32 -32 -31 ) some_material 0 0 0 1 1
( -32 -32 -32 ) ( -32 -32 -31 ) ( -31 -32 -32 ) some_material 0 0 0 1 1
( -32 -32 -32 ) ( -31 -32 -32 ) ( -32 -31 -32 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 32 33 32 ) ( 33 32 32 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 33 32 32 ) ( 32 32 33 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 32 32 33 ) ( 32 33 32 ) some_material 0 0 0 1 1
}
}
// entity 1
{
"some_key" "some_value"
}
)");
    }

    SECTION("patch is selected")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/5367

      auto* patchNode = createPatchNode();
      addNodes(map, {{&parentForNodes(map), {patchNode}}});
      selectNodes(map, {patchNode});

      CHECK(serializeSelectedNodes(map) != R"()");
    }
  }

  SECTION("serializeSelectedBrushFaces")
  {
    auto& map = fixture.create();

    const auto builder = BrushBuilder{
      map.worldNode().mapFormat(),
      map.worldBounds(),
      map.gameInfo().gameConfig.faceAttribsConfig.defaultUvAttributes,
      map.gameInfo().gameConfig.faceAttribsConfig.defaultSurfaceAttributes};

    auto* brushNode = new BrushNode{builder.createCube(64.0, "some_material").value()};

    addNodes(map, {{&parentForNodes(map), {brushNode}}});

    SECTION("nothing is selected")
    {
      CHECK(serializeSelectedBrushFaces(map) == R"()");
    }

    SECTION("one face is selected")
    {
      selectBrushFaces(map, {toHandles(brushNode).front()});

      CHECK(
        serializeSelectedBrushFaces(map)
        == R"(( -32 -32 -32 ) ( -32 -31 -32 ) ( -32 -32 -31 ) some_material 0 0 0 1 1
)");
    }

    SECTION("all faces are selected")
    {
      selectBrushFaces(map, toHandles(brushNode));

      CHECK(
        serializeSelectedBrushFaces(map)
        == R"(( -32 -32 -32 ) ( -32 -31 -32 ) ( -32 -32 -31 ) some_material 0 0 0 1 1
( -32 -32 -32 ) ( -32 -32 -31 ) ( -31 -32 -32 ) some_material 0 0 0 1 1
( -32 -32 -32 ) ( -31 -32 -32 ) ( -32 -31 -32 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 32 33 32 ) ( 33 32 32 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 33 32 32 ) ( 32 32 33 ) some_material 0 0 0 1 1
( 32 32 32 ) ( 32 32 33 ) ( 32 33 32 ) some_material 0 0 0 1 1
)");
    }
  }

  SECTION("paste")
  {
    SECTION("Paste worldspawn with single brush in layer")
    {
      const auto data = R"(
{
"classname" "worldspawn"
"to_be_ignored" "somevalue"
}
{
"classname" "func_group"
"_tb_type" "_tb_layer"
"_tb_name" "My Layer"
"_tb_id" "1"
{
( -800 288 1024 ) ( -736 288 1024 ) ( -736 224 1024 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 288 1024 ) ( -800 224 1024 ) ( -800 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 224 1024 ) ( -736 288 1024 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 288 1024 ) ( -800 288 1024 ) ( -800 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 1024 ) ( -736 224 1024 ) ( -736 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 576 ) ( -736 224 576 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
}
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();
      REQUIRE_FALSE(worldNode.entity().hasProperty("to_be_ignored"));

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);
      REQUIRE(worldNode.customLayers().empty());

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(!worldNode.entity().hasProperty("to_be_ignored"));
      CHECK(worldNode.customLayers().empty());
      CHECK(defaultLayerNode.childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(defaultLayerNode.children().front()) != nullptr);
    }

    SECTION("Paste worldspawn with single brush in group")
    {
      const auto data = R"(
{
"classname" "worldspawn"
"to_be_ignored" "somevalue"
}
{
"classname" "func_group"
"_tb_type" "_tb_group"
"_tb_name" "My Group"
"_tb_id" "2"
{
( -800 288 1024 ) ( -736 288 1024 ) ( -736 224 1024 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 288 1024 ) ( -800 224 1024 ) ( -800 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 224 1024 ) ( -736 288 1024 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 288 1024 ) ( -800 288 1024 ) ( -800 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 1024 ) ( -736 224 1024 ) ( -736 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 576 ) ( -736 224 576 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
}
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();
      REQUIRE_FALSE(worldNode.entity().hasProperty("to_be_ignored"));

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(!worldNode.entity().hasProperty("to_be_ignored"));
      CHECK(defaultLayerNode.childCount() == 1u);

      const auto* groupNode =
        dynamic_cast<GroupNode*>(defaultLayerNode.children().front());
      CHECK(groupNode != nullptr);
      CHECK(groupNode->group().name() == "My Group");
      CHECK(groupNode->childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(groupNode->children().front()) != nullptr);
    }

    SECTION("Paste worldspawn with single brush in entity")
    {
      const auto data = R"(
{
"classname" "worldspawn"
"to_be_ignored" "somevalue"
}
{
"classname" "func_door"
{
( -800 288 1024 ) ( -736 288 1024 ) ( -736 224 1024 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 288 1024 ) ( -800 224 1024 ) ( -800 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 224 1024 ) ( -736 288 1024 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -736 288 1024 ) ( -800 288 1024 ) ( -800 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 1024 ) ( -736 224 1024 ) ( -736 224 576 ) rtz/c_mf_v3c 56 -32 0 1 1
( -800 224 576 ) ( -736 224 576 ) ( -736 288 576 ) rtz/c_mf_v3c 56 -32 0 1 1
}
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();
      REQUIRE_FALSE(worldNode.entity().hasProperty("to_be_ignored"));

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(!worldNode.entity().hasProperty("to_be_ignored"));
      CHECK(defaultLayerNode.childCount() == 1u);

      const auto* entityNode =
        dynamic_cast<EntityNode*>(defaultLayerNode.children().front());
      CHECK(entityNode != nullptr);
      CHECK(entityNode->entity().classname() == "func_door");
      CHECK(entityNode->childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(entityNode->children().front()) != nullptr);
    }

    SECTION("Paste worldspawn with single brush")
    {
      const auto data = R"(
{
"classname" "worldspawn"
"to_be_ignored" "somevalue"
{
( -0 -0 -16 ) ( -0 -0  -0 ) ( 64 -0 -16 ) tex1 1 2 3 4 5
( -0 -0 -16 ) ( -0 64 -16 ) ( -0 -0  -0 ) tex2 0 0 0 1 1
( -0 -0 -16 ) ( 64 -0 -16 ) ( -0 64 -16 ) tex3 0 0 0 1 1
( 64 64  -0 ) ( -0 64  -0 ) ( 64 64 -16 ) tex4 0 0 0 1 1
( 64 64  -0 ) ( 64 64 -16 ) ( 64 -0  -0 ) tex5 0 0 0 1 1
( 64 64  -0 ) ( 64 -0  -0 ) ( -0 64  -0 ) tex6 0 0 0 1 1
}
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();
      REQUIRE_FALSE(worldNode.entity().hasProperty("to_be_ignored"));

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(!worldNode.entity().hasProperty("to_be_ignored"));
      CHECK(defaultLayerNode.childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(defaultLayerNode.children().front()) != nullptr);
    }

    SECTION("Paste single brush")
    {
      const auto data = R"(
{
( -0 -0 -16 ) ( -0 -0  -0 ) ( 64 -0 -16 ) tex1 1 2 3 4 5
( -0 -0 -16 ) ( -0 64 -16 ) ( -0 -0  -0 ) tex2 0 0 0 1 1
( -0 -0 -16 ) ( 64 -0 -16 ) ( -0 64 -16 ) tex3 0 0 0 1 1
( 64 64  -0 ) ( -0 64  -0 ) ( 64 64 -16 ) tex4 0 0 0 1 1
( 64 64  -0 ) ( 64 64 -16 ) ( 64 -0  -0 ) tex5 0 0 0 1 1
( 64 64  -0 ) ( 64 -0  -0 ) ( -0 64  -0 ) tex6 0 0 0 1 1
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(defaultLayerNode.childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(defaultLayerNode.children().front()) != nullptr);
    }

    SECTION("Paste single patch")
    {
      const auto data = R"(
{
patchDef2
{
common/caulk
( 5 3 0 0 0 )
(
( (-64 -64 4 0   0 ) (-64 0 4 0   -0.25 ) (-64 64 4 0   -0.5 ) )
( (  0 -64 4 0.2 0 ) (  0 0 4 0.2 -0.25 ) (  0 64 4 0.2 -0.5 ) )
( ( 64 -64 4 0.4 0 ) ( 64 0 4 0.4 -0.25 ) ( 64 64 4 0.4 -0.5 ) )
( (128 -64 4 0.6 0 ) (128 0 4 0.6 -0.25 ) (128 64 4 0.6 -0.5 ) )
( (192 -64 4 0.8 0 ) (192 0 4 0.8 -0.25 ) (192 64 4 0.8 -0.5 ) )
)
}
})";

      auto& map = fixture.create({.mapFormat = MapFormat::Quake3});
      const auto& worldNode = map.worldNode();

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(defaultLayerNode.childCount() == 0u);

      REQUIRE(paste(map, data) == PasteType::Node);
      REQUIRE(defaultLayerNode.childCount() == 1u);
      CHECK(dynamic_cast<PatchNode*>(defaultLayerNode.children().front()) != nullptr);
    }

    SECTION("Copy and paste a single patch")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/5367

      auto& map = fixture.create({.mapFormat = MapFormat::Quake3});

      auto* patchNode = createPatchNode();
      addNodes(map, {{&parentForNodes(map), {patchNode}}});
      selectNodes(map, {patchNode});

      const auto copied = serializeSelectedNodes(map);
      REQUIRE(copied != R"()");

      const auto originalPatch = patchNode->patch();
      deselectAll(map);
      removeNodes(map, {patchNode});

      CHECK(paste(map, copied) == PasteType::Node);
      CHECK(map.selection().hasOnlyPatches());
      REQUIRE(map.selection().patches.size() == 1u);
      CHECK(map.selection().patches.front()->patch() == originalPatch);
    }

    SECTION("Paste and translate a group")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/2776

      auto& map = fixture.create();

      const auto builder = BrushBuilder{map.worldNode().mapFormat(), map.worldBounds()};
      const auto box = vm::bbox3d{{0, 0, 0}, {64, 64, 64}};

      auto* brushNode1 =
        new BrushNode{builder.createCuboid(box, "material") | kdl::value()};
      addNodes(map, {{&parentForNodes(map), {brushNode1}}});
      selectNodes(map, {brushNode1});

      const auto groupName = std::string{"testGroup"};

      auto* groupNode = groupSelectedNodes(map, groupName);
      CHECK(groupNode != nullptr);
      selectNodes(map, {groupNode});

      const auto copied = serializeSelectedNodes(map);

      const auto delta = vm::vec3d{16, 16, 16};
      CHECK(paste(map, copied) == PasteType::Node);
      CHECK(map.selection().groups.size() == 1u);
      CHECK(map.selection().groups.at(0)->name() == groupName);
      CHECK(translateSelection(map, delta));
      CHECK(map.selectionBounds() == box.translate(delta));
    }

    SECTION("Paste into open group")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/1734

      const auto data = std::string{
        R"({
"classname" "light"
"origin" "0 0 0"
})"};

      auto& map = fixture.create();
      auto* brushNode = createBrushNode(map);

      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      auto* groupNode = groupSelectedNodes(map, "test");
      openGroup(map, *groupNode);

      CHECK(paste(map, data) == PasteType::Node);
      CHECK(map.selection().hasOnlyEntities());
      CHECK(map.selection().entities.size() == 1u);

      auto* light = map.selection().entities.front();
      CHECK(light->parent() == groupNode);
    }

    SECTION("Undo and redo")
    {
      // https://github.com/TrenchBroom/TrenchBroom/issues/4174

      const auto data = R"(
{
( -0 -0 -16 ) ( -0 -0  -0 ) ( 64 -0 -16 ) tex1 1 2 3 4 5
( -0 -0 -16 ) ( -0 64 -16 ) ( -0 -0  -0 ) tex2 0 0 0 1 1
( -0 -0 -16 ) ( 64 -0 -16 ) ( -0 64 -16 ) tex3 0 0 0 1 1
( 64 64  -0 ) ( -0 64  -0 ) ( 64 64 -16 ) tex4 0 0 0 1 1
( 64 64  -0 ) ( 64 64 -16 ) ( 64 -0  -0 ) tex5 0 0 0 1 1
( 64 64  -0 ) ( 64 -0  -0 ) ( -0 64  -0 ) tex6 0 0 0 1 1
})";

      auto& map = fixture.create();
      const auto& worldNode = map.worldNode();

      const auto& defaultLayerNode = *worldNode.defaultLayer();
      REQUIRE(map.selection().brushes.size() == 0u);
      REQUIRE(defaultLayerNode.childCount() == 0u);

      REQUIRE(paste(map, data) == PasteType::Node);
      REQUIRE(defaultLayerNode.childCount() == 1u);
      REQUIRE(dynamic_cast<BrushNode*>(defaultLayerNode.children().front()) != nullptr);
      REQUIRE(map.selection().brushes.size() == 1u);

      CHECK(map.canUndoCommand());
      map.undoCommand();
      CHECK(defaultLayerNode.childCount() == 0u);
      CHECK(map.selection().brushes.size() == 0u);

      map.redoCommand();
      CHECK(defaultLayerNode.childCount() == 1u);
      CHECK(dynamic_cast<BrushNode*>(defaultLayerNode.children().front()) != nullptr);
      CHECK(map.selection().brushes.size() == 1u);
    }

    SECTION("Paste resets duplicate group IDs")
    {
      auto& map = fixture.create();

      auto* entityNode = new EntityNode{Entity{}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      selectNodes(map, {entityNode});
      auto* groupNode = groupSelectedNodes(map, "test");

      const auto persistentGroupId = groupNode->persistentId();
      REQUIRE(persistentGroupId.has_value());

      deselectAll(map);
      selectNodes(map, {groupNode});

      const auto str = serializeSelectedNodes(map);

      SECTION("Copy and paste resets persistent group ID")
      {
        deselectAll(map);
        REQUIRE(paste(map, str) == PasteType::Node);

        auto* pastedGroupNode =
          dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
        REQUIRE(pastedGroupNode != nullptr);
        REQUIRE(pastedGroupNode != groupNode);

        CHECK(pastedGroupNode->persistentId() != persistentGroupId);
      }

      SECTION("Cut and paste retains persistent group ID")
      {
        removeSelectedNodes(map);
        deselectAll(map);
        REQUIRE(paste(map, str) == PasteType::Node);

        auto* pastedGroupNode =
          dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
        REQUIRE(pastedGroupNode != nullptr);
        REQUIRE(pastedGroupNode != groupNode);

        CHECK(pastedGroupNode->persistentId() == persistentGroupId);
      }
    }

    SECTION("Paste resets duplicate link IDs")
    {
      auto& map = fixture.create();

      auto* brushNode = createBrushNode(map);
      addNodes(map, {{&parentForNodes(map), {brushNode}}});
      selectNodes(map, {brushNode});

      auto* groupNode = groupSelectedNodes(map, "test");

      deselectAll(map);
      selectNodes(map, {groupNode});
      auto* linkedGroup = createLinkedDuplicate(map);

      const auto originalGroupLinkId = linkedGroup->linkId();
      REQUIRE(originalGroupLinkId == groupNode->linkId());

      auto* linkedBrushNode = dynamic_cast<BrushNode*>(linkedGroup->children().front());
      REQUIRE(linkedBrushNode);

      const auto originalBrushLinkId = linkedBrushNode->linkId();
      REQUIRE(originalBrushLinkId == brushNode->linkId());

      deselectAll(map);

      SECTION("Pasting one linked brush")
      {
        deselectAll(map);
        openGroup(map, *groupNode);

        selectNodes(map, {brushNode});
        const auto data = serializeSelectedNodes(map);

        deselectAll(map);

        CHECK(paste(map, data) == PasteType::Node);
        CHECK(groupNode->childCount() == 2);

        const auto* pastedBrushNode =
          dynamic_cast<BrushNode*>(groupNode->children().back());
        REQUIRE(pastedBrushNode);

        CHECK(pastedBrushNode->linkId() != originalBrushLinkId);
      }

      SECTION("Pasting one linked group")
      {
        selectNodes(map, {linkedGroup});
        const auto data = serializeSelectedNodes(map);

        deselectAll(map);

        SECTION("Pasting unknown linked group ID")
        {
          selectAllNodes(map);
          removeSelectedNodes(map);

          CHECK(paste(map, data) == PasteType::Node);
          CHECK(map.worldNode().defaultLayer()->childCount() == 1);

          const auto* pastedGroupNode =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
          REQUIRE(pastedGroupNode);

          CHECK(pastedGroupNode->linkId() == originalGroupLinkId);
        }

        SECTION("If only one linked group exists")
        {
          selectNodes(map, {linkedGroup});
          removeSelectedNodes(map);

          CHECK(paste(map, data) == PasteType::Node);
          CHECK(map.worldNode().defaultLayer()->childCount() == 2);

          const auto* pastedGroupNode =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
          REQUIRE(pastedGroupNode);

          CHECK(pastedGroupNode->linkId() != originalGroupLinkId);

          const auto* pastedBrushNode =
            dynamic_cast<BrushNode*>(pastedGroupNode->children().front());
          REQUIRE(pastedBrushNode);

          CHECK(pastedBrushNode->linkId() != originalBrushLinkId);
        }

        SECTION("If more than one linked group exists")
        {
          CHECK(paste(map, data) == PasteType::Node);
          CHECK(map.worldNode().defaultLayer()->childCount() == 3);

          const auto* pastedGroupNode =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
          REQUIRE(pastedGroupNode);

          CHECK(pastedGroupNode->linkId() == originalGroupLinkId);

          const auto* pastedBrushNode =
            dynamic_cast<BrushNode*>(pastedGroupNode->children().front());
          REQUIRE(pastedBrushNode);

          CHECK(pastedBrushNode->linkId() == originalBrushLinkId);
        }

        SECTION("Pasting recursive linked group")
        {
          openGroup(map, *groupNode);

          CHECK(paste(map, data) == PasteType::Node);
          CHECK(groupNode->childCount() == 2);
          CHECK(linkedGroup->childCount() == 2);

          auto* pastedGroup = dynamic_cast<GroupNode*>(groupNode->children().back());
          REQUIRE(pastedGroup);

          CHECK(pastedGroup->linkId() != originalGroupLinkId);

          const auto* pastedBrushNode =
            dynamic_cast<BrushNode*>(pastedGroup->children().front());
          REQUIRE(pastedBrushNode);

          CHECK(pastedBrushNode->linkId() != originalBrushLinkId);

          auto* linkedPastedGroupNode =
            dynamic_cast<GroupNode*>(linkedGroup->children().back());
          REQUIRE(linkedPastedGroupNode);

          CHECK(linkedPastedGroupNode->linkId() == pastedGroup->linkId());

          const auto* linkedPastedBrushNode =
            dynamic_cast<BrushNode*>(linkedPastedGroupNode->children().front());
          REQUIRE(pastedBrushNode);

          CHECK(linkedPastedBrushNode->linkId() == pastedBrushNode->linkId());
        }
      }

      SECTION("Pasting two linked groups")
      {
        selectNodes(map, {groupNode, linkedGroup});
        const auto data = serializeSelectedNodes(map);

        deselectAll(map);

        SECTION("If only one original group exists")
        {
          selectNodes(map, {linkedGroup});
          removeSelectedNodes(map);

          CHECK(paste(map, data) == PasteType::Node);
          CHECK(map.worldNode().defaultLayer()->childCount() == 3);

          const auto* pastedGroupNode1 =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children()[1]);
          REQUIRE(pastedGroupNode1);

          const auto* pastedGroupNode2 =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children()[2]);
          REQUIRE(pastedGroupNode2);

          CHECK(pastedGroupNode1->linkId() != originalGroupLinkId);
          CHECK(pastedGroupNode2->linkId() != originalGroupLinkId);
          CHECK(pastedGroupNode1->linkId() == pastedGroupNode2->linkId());

          const auto* pastedBrushNode1 =
            dynamic_cast<BrushNode*>(pastedGroupNode1->children().front());
          REQUIRE(pastedBrushNode1);

          CHECK(pastedBrushNode1->linkId() != originalBrushLinkId);

          const auto* pastedBrushNode2 =
            dynamic_cast<BrushNode*>(pastedGroupNode2->children().front());
          REQUIRE(pastedBrushNode2);

          CHECK(pastedBrushNode2->linkId() != originalBrushLinkId);

          CHECK(pastedBrushNode1->linkId() == pastedBrushNode2->linkId());
        }

        SECTION("If both original groups exist")
        {
          CHECK(paste(map, data) == PasteType::Node);
          CHECK(map.worldNode().defaultLayer()->childCount() == 4);

          const auto* pastedGroupNode1 =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children()[2]);
          REQUIRE(pastedGroupNode1);

          const auto* pastedGroupNode2 =
            dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children()[3]);
          REQUIRE(pastedGroupNode2);

          CHECK(pastedGroupNode1->linkId() == originalGroupLinkId);
          CHECK(pastedGroupNode2->linkId() == originalGroupLinkId);

          const auto* pastedBrushNode1 =
            dynamic_cast<BrushNode*>(pastedGroupNode1->children().front());
          REQUIRE(pastedBrushNode1);

          CHECK(pastedBrushNode1->linkId() == originalBrushLinkId);

          const auto* pastedBrushNode2 =
            dynamic_cast<BrushNode*>(pastedGroupNode2->children().front());
          REQUIRE(pastedBrushNode2);

          CHECK(pastedBrushNode2->linkId() == originalBrushLinkId);
        }
      }
    }

    SECTION("Unique names")
    {
      SECTION("A game without naming rules keeps the pasted names")
      {
        auto& map = fixture.create();
        setLinkDefinitions(map);

        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        CHECK(
          entityNamesAndTargets(map)
          == std::vector<std::pair<std::string, std::string>>{
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
          });

        const auto* triggerNode =
          dynamic_cast<EntityNode*>(map.worldNode().defaultLayer()->children()[1]);
        REQUIRE(triggerNode);
        CHECK(triggerNode->entity().property("model") == nullptr);
      }

      auto& map = fixture.create(makeNamingFixtureConfig());
      setLinkDefinitions(map);

      SECTION("Pasting into a map without clashes keeps the names")
      {
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        CHECK(
          entityNamesAndTargets(map)
          == std::vector<std::pair<std::string, std::string>>{
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
          });

        // a brush entity gets its name as its model
        const auto* triggerNode =
          dynamic_cast<EntityNode*>(map.worldNode().defaultLayer()->children()[1]);
        REQUIRE(triggerNode);
        CHECK(*triggerNode->entity().property("model") == "trigger_once_0");
      }

      SECTION("Pasting twice renames the second copy and retargets it")
      {
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        CHECK(
          entityNamesAndTargets(map)
          == std::vector<std::pair<std::string, std::string>>{
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
            {"light_1", ""},
            {"trigger_once_1", "light_1"},
          });

        const auto* triggerNode =
          dynamic_cast<EntityNode*>(map.worldNode().defaultLayer()->children()[3]);
        REQUIRE(triggerNode);
        CHECK(*triggerNode->entity().property("model") == "trigger_once_1");

        SECTION("Each paste is one undo step and redo restores the names")
        {
          map.undoCommand();
          CHECK(map.worldNode().defaultLayer()->childCount() == 2u);

          map.redoCommand();
          CHECK(
            entityNamesAndTargets(map)
            == std::vector<std::pair<std::string, std::string>>{
              {"light_0", ""},
              {"trigger_once_0", "light_0"},
              {"light_1", ""},
              {"trigger_once_1", "light_1"},
            });
        }
      }

      SECTION("A trigger pasted without its light keeps its target")
      {
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        REQUIRE(paste(map, TriggerAlone) == PasteType::Node);
        CHECK(
          entityNamesAndTargets(map)
          == std::vector<std::pair<std::string, std::string>>{
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
            {"trigger_once_1", "light_0"},
          });
      }

      SECTION("Cut and paste keeps the names")
      {
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);

        // cut the second copy
        deselectAll(map);
        selectNodes(
          map,
          {map.worldNode().defaultLayer()->children()[2],
           map.worldNode().defaultLayer()->children()[3]});
        const auto str = serializeSelectedNodes(map);
        removeSelectedNodes(map);

        REQUIRE(paste(map, str) == PasteType::Node);

        // the node writer decides the order of the entities in the clipboard
        CHECK_THAT(
          entityNamesAndTargets(map),
          Catch::Matchers::UnorderedEquals(
            std::vector<std::pair<std::string, std::string>>{
              {"light_0", ""},
              {"trigger_once_0", "light_0"},
              {"light_1", ""},
              {"trigger_once_1", "light_1"},
            }));
      }

      SECTION("References in a pasted group that joins a link set are kept")
      {
        auto* lightNode = new EntityNode{Entity{{
          {"classname", "light"},
          {"name", "light_0"},
        }}};
        auto* triggerNode = new EntityNode{Entity{{
          {"classname", "trigger_once"},
          {"name", "trigger_once_0"},
          {"target", "light_0"},
        }}};
        addNodes(map, {{&parentForNodes(map), {lightNode, triggerNode}}});

        selectNodes(map, {lightNode, triggerNode});
        auto* groupNode = groupSelectedNodes(map, "group");
        REQUIRE(groupNode != nullptr);
        deselectAll(map);
        selectNodes(map, {groupNode});
        REQUIRE(createLinkedDuplicate(map) != nullptr);

        deselectAll(map);
        selectNodes(map, {groupNode});
        const auto str = serializeSelectedNodes(map);
        deselectAll(map);

        // the pasted group joins the link set, whose next update would overwrite a
        // remapped target with the original's, so the target stays as it is
        REQUIRE(paste(map, str) == PasteType::Node);
        auto* pastedGroupNode =
          dynamic_cast<GroupNode*>(map.worldNode().defaultLayer()->children().back());
        REQUIRE(pastedGroupNode != nullptr);
        REQUIRE(pastedGroupNode->linkId() == groupNode->linkId());

        const auto pastedNamesAndTargets = [&]() {
          auto result = std::vector<std::pair<std::string, std::string>>{};
          for (const auto* child : pastedGroupNode->children())
          {
            const auto& entity = static_cast<const EntityNode*>(child)->entity();
            const auto* target = entity.property("target");
            result.emplace_back(*entity.property("name"), target ? *target : "");
          }
          return result;
        };

        const auto expected = std::vector<std::pair<std::string, std::string>>{
          {"light_2", ""},
          {"trigger_once_2", "light_0"},
        };
        CHECK(pastedNamesAndTargets() == expected);

        // an update from the original group keeps the names and the target
        openGroup(map, *groupNode);
        deselectAll(map);
        selectNodes(map, {lightNode});
        REQUIRE(setEntityProperty(map, "light_radius", "64"));
        CHECK(pastedNamesAndTargets() == expected);
      }

      SECTION("The names do not depend on the run")
      {
        for (size_t i = 0; i < 3; ++i)
        {
          REQUIRE(paste(map, LightAndTrigger) == PasteType::Node);
        }

        auto otherFixture = MapFixture{};
        auto& otherMap = otherFixture.create(makeNamingFixtureConfig());
        setLinkDefinitions(otherMap);
        for (size_t i = 0; i < 3; ++i)
        {
          REQUIRE(paste(otherMap, LightAndTrigger) == PasteType::Node);
        }

        CHECK(entityNamesAndTargets(otherMap) == entityNamesAndTargets(map));
        CHECK(
          entityNamesAndTargets(map)
          == std::vector<std::pair<std::string, std::string>>{
            {"light_0", ""},
            {"trigger_once_0", "light_0"},
            {"light_1", ""},
            {"trigger_once_1", "light_1"},
            {"light_2", ""},
            {"trigger_once_2", "light_2"},
          });
      }
    }
  }
}

} // namespace tb::mdl
