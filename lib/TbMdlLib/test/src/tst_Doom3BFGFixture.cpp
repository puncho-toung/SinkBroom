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

#include "TestEnvironment.h"
#include "TestParserStatus.h"
#include "fs/DiskIO.h"
#include "fs/Reader.h"
#include "mdl/CatchConfig.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/FgdParser.h"
#include "mdl/GameConfigFixture.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"

#include <filesystem>

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{

TEST_CASE("Doom3BFGFixture")
{
  const auto gamePath = getFixtureRoot() / "test" / "mdl" / "Game" / "Doom3BFG";

  SECTION("Doom3BFGGameInfo")
  {
    CHECK(Doom3BFGGameInfo.gameConfig.name == "Doom 3 BFG");
    CHECK(Doom3BFGGameInfo.gamePathPreference.defaultValue == gamePath);
    CHECK(std::filesystem::exists(gamePath / "base" / "materials" / "tb_test.mtr"));
  }

  SECTION("Doom3BFGFixtureConfig")
  {
    auto fixture = MapFixture{};
    auto& map = fixture.create(Doom3BFGFixtureConfig);

    CHECK(map.gameInfo().gameConfig.name == "Doom 3 BFG");

    // the builtin FGD is found next to the fixture's virtual config file
    const auto* lightDefinition = map.entityDefinitionManager().definition("light");
    REQUIRE(lightDefinition != nullptr);
    CHECK(getType(*lightDefinition) == EntityDefinitionType::Point);
  }

  SECTION("DOOM-3-tbtest.fgd")
  {
    const auto path = gamePath / "base" / "_tb" / "fgd" / "DOOM-3-tbtest.fgd";
    auto file = fs::Disk::openFile(path) | kdl::value();
    auto reader = file->reader().buffer();

    auto parser = FgdParser{reader.stringView(), RgbaF{1.0f, 1.0f, 1.0f, 1.0f}, path};

    auto status = TestParserStatus{};
    const auto definitions = parser.parseDefinitions(status);
    REQUIRE(definitions.is_success());
    CHECK(definitions.value().size() == 10u);
    CHECK(status.countStatus(LogLevel::Error) == 0u);
    CHECK(status.countStatus(LogLevel::Warn) == 0u);
  }
}

} // namespace tb::mdl
