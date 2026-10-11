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

#include "mdl/CatchConfig.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/LinkSourceValidator.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/PropertyDefinition.h"
#include "mdl/TestUtils.h"
#include "mdl/WorldNode.h"

#include "kd/ranges/to.h"
#include "kd/vector_utils.h"

#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("LinkSourceValidator")
{
  auto fixture = MapFixture{};
  auto& map = fixture.create(NamingFixtureConfig);

  // the Doom 3 fixture FGD has no link target key besides name, so foo stands in for one
  map.entityDefinitionManager().setDefinitions({
    {"target_null",
     Color{},
     "",
     {
       PropertyDefinition{"name", PropertyValueTypes::LinkTarget{}, "", ""},
       PropertyDefinition{"foo", PropertyValueTypes::LinkTarget{}, "", ""},
     },
     PointEntityDefinition{vm::bbox3d{16.0}, {}, {}}},
  });

  // nothing refers to either key
  addNodes(
    map,
    {{&parentForNodes(map),
      {new EntityNode{Entity{{
        {"classname", "target_null"},
        {"name", "target_null_0"},
        {"foo", "bar"},
      }}}}}});

  const auto reportedKeys = [&](const std::vector<const Validator*>& validators) {
    return kdl::vec_sort(
      collectIssues(map.worldNode(), validators)
      | std::views::transform([](const auto* issue) {
          return static_cast<const EntityPropertyIssue*>(issue)->propertyKey();
        })
      | kdl::ranges::to<std::vector>());
  };

  SECTION("An ignored key is not reported, another link target key is")
  {
    const auto validator = LinkSourceValidator{map.entityLinkManager(), {"name"}};
    CHECK(reportedKeys({&validator}) == std::vector<std::string>{"foo"});
  }

  SECTION("Without ignored keys, every unused link target key is reported")
  {
    const auto validator = LinkSourceValidator{map.entityLinkManager()};
    CHECK(reportedKeys({&validator}) == std::vector<std::string>{"foo", "name"});
  }

  SECTION("A map whose game has naming rules ignores the name key")
  {
    const auto* validator = findValidator(map.worldNode(), "Missing entity link source");
    REQUIRE(validator != nullptr);
    CHECK(reportedKeys({validator}) == std::vector<std::string>{"foo"});
  }
}

} // namespace tb::mdl
