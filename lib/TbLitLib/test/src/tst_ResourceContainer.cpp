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

#include "LitTestUtils.h"
#include "lit/GameFile.h"
#include "lit/ResourceContainer.h"

#include "kd/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

void setBig32(std::string& bytes, const std::size_t at, const std::uint32_t value)
{
  bytes[at] = char((value >> 24) & 0xFF);
  bytes[at + 1] = char((value >> 16) & 0xFF);
  bytes[at + 2] = char((value >> 8) & 0xFF);
  bytes[at + 3] = char(value & 0xFF);
}

void appendBig32(std::string& out, const std::uint32_t value)
{
  out.push_back(char((value >> 24) & 0xFF));
  out.push_back(char((value >> 16) & 0xFF));
  out.push_back(char((value >> 8) & 0xFF));
  out.push_back(char(value & 0xFF));
}

void appendLittle32(std::string& out, const std::uint32_t value)
{
  out.push_back(char(value & 0xFF));
  out.push_back(char((value >> 8) & 0xFF));
  out.push_back(char((value >> 16) & 0xFF));
  out.push_back(char((value >> 24) & 0xFF));
}

// one table entry as idResourceCacheEntry::Write writes it: the name's length (little
// endian), the name, then offset and length (big endian)
std::string tableEntry(
  const std::uint32_t nameLength,
  const std::string& name,
  const std::uint32_t offset,
  const std::uint32_t length)
{
  auto out = std::string{};
  appendLittle32(out, nameLength);
  out += name;
  appendBig32(out, offset);
  appendBig32(out, length);
  return out;
}

// a container of the data "abcdef" at offset 12, then a table of count and entries
std::string container(const std::uint32_t count, const std::string& entries)
{
  auto out = std::string(12, '\0');
  out += "abcdef";
  const auto tableOffset = std::uint32_t(out.size());
  appendBig32(out, count);
  out += entries;
  setBig32(out, 0, ResourceFileMagic);
  setBig32(out, 4, tableOffset);
  setBig32(out, 8, std::uint32_t(out.size()) - tableOffset);
  return out;
}

std::string errorOf(const Result<ResourceContainer>& result)
{
  return result.is_error() ? std::get<Error>(result.error()).msg : std::string{};
}

} // namespace

TEST_CASE("ResourceContainer")
{
  const auto folder = TempFolder{"tb-lit-resources"};
  const auto file = folder.path() / "test.resources";
  const auto good = resourcesBytes({
    {"Materials\\One.mtr", "one"},
    {"materials/two.mtr", "two"},
    {"materials/one.mtr", "one again"},
  });
  // the table: the count, then three entries of 4 + 17 + 8 bytes
  const auto tableOffset = good.size() - (4 + 3 * 12 + 17 + 17 + 17);

  auto warnings = std::vector<std::string>{};
  const auto open = [&](const std::string& bytes) {
    writeTextFile(file, bytes);
    warnings.clear();
    return ResourceContainer::open(file, "test.resources", warnings);
  };

  SECTION("reads the engine's format")
  {
    const auto opened = open(good);
    REQUIRE(opened.is_success());
    const auto& resources = opened.value();
    CHECK(warnings.empty());
    REQUIRE(resources.entries().size() == 3u);
    CHECK(resources.entries()[0].name == "materials/one.mtr");

    // case-insensitive, and the later of two entries with one name
    const auto* one = resources.find("MATERIALS/ONE.MTR");
    REQUIRE(one);
    CHECK(one->length == 9u);

    auto handle = GameFile::open(file);
    REQUIRE(handle.is_success());
    CHECK(resources.read(*one, handle.value()).value() == "one again");
    CHECK(
      resources.read(*resources.find("materials/two.mtr"), handle.value()).value()
      == "two");
  }

  SECTION("a corrupt container is never read out of bounds")
  {
    const auto refused = [&](const std::string& bytes, const std::string& reason) {
      const auto opened = open(bytes);
      CHECK(opened.is_error());
      CHECK_THAT(errorOf(opened), Catch::Matchers::ContainsSubstring(reason));
      CHECK(warnings.empty());
    };

    // truncated: no header, then the table cut off
    refused(good.substr(0, 5), "too short for its header");
    refused(good.substr(0, good.size() - 3), "does not fit");

    auto wrongMagic = good;
    setBig32(wrongMagic, 0, 0xDEADBEEFu);
    refused(wrongMagic, "RESOURCE_FILE_MAGIC");

    auto tablePastEnd = good;
    setBig32(tablePastEnd, 4, std::uint32_t(good.size()));
    refused(tablePastEnd, "does not fit");

    // A3.12: a negative table offset, and a table too short for its count
    auto negativeTable = good;
    setBig32(negativeTable, 4, 0xFFFFFFF0u);
    refused(negativeTable, "does not fit");
    for (std::uint32_t length = 0; length < 4; ++length)
    {
      auto shortTable = good;
      setBig32(shortTable, 8, length);
      refused(shortTable, "does not fit");
    }

    auto hugeTable = good;
    setBig32(hugeTable, 8, 0x7FFFFFF0u);
    refused(hugeTable, "does not fit");

    // the entry count, then a name length, larger than the table holds
    auto countPastTable = good;
    setBig32(countPastTable, tableOffset, 1000000u);
    refused(countPastTable, "truncated");

    auto namePastTable = good;
    namePastTable[tableOffset + 4] = char(0x7F);
    namePastTable[tableOffset + 7] = char(0x7F);
    refused(namePastTable, "truncated");

    auto negativeCount = good;
    setBig32(negativeCount, tableOffset, 0xFFFFFFFFu);
    refused(negativeCount, "truncated");

    // A3.12: an entry cut off after its name, and one cut off in its length
    refused(container(1, tableEntry(5, "a.mtr", 12, 3).substr(0, 4 + 5)), "truncated");
    refused(
      container(1, tableEntry(5, "a.mtr", 12, 3).substr(0, 4 + 5 + 6)), "truncated");
  }

  SECTION("an entry whose data does not fit is skipped, the others are kept")
  {
    auto entryPastEnd = good;
    // the first entry's offset: after the count, its name length and its name
    setBig32(entryPastEnd, tableOffset + 4 + 4 + 17, std::uint32_t(good.size()));

    const auto opened = open(entryPastEnd);
    REQUIRE(opened.is_success());
    REQUIRE(warnings.size() == 1u);
    CHECK_THAT(warnings[0], Catch::Matchers::ContainsSubstring("does not fit"));
    CHECK(opened.value().entries().size() == 2u);

    // A3.12: a negative offset, and a negative length
    const auto negative = open(container(
      3,
      tableEntry(5, "a.mtr", 0xFFFFFFFFu, 3) + tableEntry(5, "b.mtr", 12, 0xFFFFFFFFu)
        + tableEntry(5, "c.mtr", 12, 6)));
    REQUIRE(negative.is_success());
    CHECK(warnings.size() == 2u);
    REQUIRE(negative.value().entries().size() == 1u);
    CHECK(negative.value().entries()[0].name == "c.mtr");
  }

  SECTION("names as the engine reads them")
  {
    // A3.12: ReadString reads nothing for a negative length, so the entry has no name
    const auto unnamed = open(container(1, tableEntry(0xFFFFFFFFu, "", 12, 3)));
    REQUIRE(unnamed.is_success());
    REQUIRE(unnamed.value().entries().size() == 1u);
    CHECK(unnamed.value().entries()[0].name.empty());

    // a name holding a NUL is seen up to it
    const auto withNul =
      open(container(1, tableEntry(8, std::string{"a.mtr\0zz", 8}, 12, 3)));
    REQUIRE(withNul.is_success());
    REQUIRE(withNul.value().find("a.mtr"));
    CHECK(withNul.value().find("a.mtr")->length == 3u);
  }

  SECTION("a container that changed after its table was read is not read from")
  {
    const auto opened = open(good);
    REQUIRE(opened.is_success());
    writeTextFile(file, good.substr(0, 20));
    auto handle = GameFile::open(file);
    REQUIRE(handle.is_success());
    const auto& resources = opened.value();
    const auto read =
      resources.read(*resources.find("materials/two.mtr"), handle.value());
    REQUIRE(read.is_error());
    CHECK_THAT(
      std::get<Error>(read.error()).msg, Catch::Matchers::ContainsSubstring("changed"));
  }

  SECTION("a stop ends the table read")
  {
    // R5.20, G4: the cancel flag is checked for every table entry
    writeTextFile(file, good);
    const auto cancel = std::atomic<bool>{true};
    const auto opened =
      ResourceContainer::open(file, "test.resources", warnings, &cancel);
    CHECK_THAT(errorOf(opened), Catch::Matchers::ContainsSubstring("cancelled"));
  }
}

} // namespace tb::lit
