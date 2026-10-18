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
#include "mdl/EntityNode.h"
#include "mdl/Issue.h"
#include "mdl/IssueQuickFix.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_Nodes.h"
#include "mdl/TestUtils.h"
#include "mdl/Transaction.h"
#include "mdl/Validator.h"
#include "mdl/WorldNode.h"

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

void applyQuickFix(
  Map& map,
  const IssueType type,
  const std::vector<const Issue*>& issues,
  const std::string& description)
{
  const auto quickFixes = map.worldNode().quickFixes(type);
  const auto quickFix = std::ranges::find_if(
    quickFixes, [&](const auto* q) { return q->description() == description; });
  REQUIRE(quickFix != quickFixes.end());

  // like the issue browser
  auto transaction = Transaction{map, "Apply Quick Fix"};
  (*quickFix)->apply(map, issues);
  CHECK(transaction.commit());
}

} // namespace

TEST_CASE("IssueQuickFix")
{
  auto fixture = MapFixture{};
  auto& map = fixture.create(NamingFixtureConfig);

  const auto* validator = findValidator(map.worldNode(), "Empty property value");
  REQUIRE(validator != nullptr);
  const auto type = validator->type();

  SECTION("A single issue fix reads every issue before it changes anything")
  {
    SECTION("Issues of two entities with the same name")
    {
      // changing one entity destroys the issues of every entity with its name
      auto* entityNode1 = new EntityNode{Entity{{
        {"classname", "info_null"},
        {"name", "a"},
        {"foo", ""},
      }}};
      auto* entityNode2 = new EntityNode{Entity{{
        {"classname", "info_null"},
        {"name", "a"},
        {"foo", ""},
      }}};
      addNodes(map, {{&parentForNodes(map), {entityNode1, entityNode2}}});

      const auto issues = collectIssues(map.worldNode(), type);
      REQUIRE(issues.size() == 2u);

      applyQuickFix(map, type, issues, "Delete Property");

      CHECK_FALSE(entityNode1->entity().hasProperty("foo"));
      CHECK_FALSE(entityNode2->entity().hasProperty("foo"));
      CHECK(collectIssues(map.worldNode(), type).empty());
    }

    SECTION("Two issues of one entity")
    {
      auto* entityNode = new EntityNode{Entity{{
        {"classname", "info_null"},
        {"foo", ""},
        {"bar", ""},
      }}};
      addNodes(map, {{&parentForNodes(map), {entityNode}}});

      const auto issues = collectIssues(map.worldNode(), type);
      REQUIRE(issues.size() == 2u);

      applyQuickFix(map, type, issues, "Delete Property");

      CHECK_FALSE(entityNode->entity().hasProperty("foo"));
      CHECK_FALSE(entityNode->entity().hasProperty("bar"));
      CHECK(collectIssues(map.worldNode(), type).empty());
    }
  }
}

} // namespace tb::mdl
