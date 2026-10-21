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

// The hidden retail tests ([.][retail]): they read the game folder that TB_LIT_GAME_PATH
// names (the D3-Sink build folder with base/), and are skipped without it.
//
// - The sweep parses every material and table through GameDeclSource and the parse thread
//   and reports the counts, the defaulted materials, the warnings, the kept decl text and
//   the times.
// - The goldens compare SinkBroom's printMaterial and printTable output with the
// engine's,
//   captured by TOOLS/lit-preview/capture_goldens.py into fixture/lit/goldens; the
//   engine is not run again.

#include "LitTestUtils.h"
#include "TestEnvironment.h"
#include "lit/GameDeclSource.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Separator = "----------";

std::optional<std::filesystem::path> retailGamePath()
{
  const auto* value = std::getenv("TB_LIT_GAME_PATH");
  if (!value || !*value)
  {
    return std::nullopt;
  }
  return std::filesystem::path{value};
}

struct GoldenBlock
{
  std::string kind;
  std::string name;
  std::vector<std::string> lines;
};

std::vector<GoldenBlock> readGoldens()
{
  const auto path = getFixtureRoot() / "test" / "lit" / "goldens" / "engine_print.txt";
  auto stream = std::istringstream{readTextFile(path)};
  auto blocks = std::vector<GoldenBlock>{};
  auto line = std::string{};
  while (std::getline(stream, line))
  {
    if (line.rfind("@@ ", 0) == 0)
    {
      const auto kindEnd = line.find(' ', 3);
      blocks.push_back(
        GoldenBlock{line.substr(3, kindEnd - 3), line.substr(kindEnd + 1), {}});
    }
    else if (!blocks.empty())
    {
      blocks.back().lines.push_back(line);
    }
  }
  return blocks;
}

const GoldenBlock* findBlock(
  const std::vector<GoldenBlock>& blocks,
  const std::string& kind,
  const std::string& name)
{
  const auto it = std::find_if(blocks.begin(), blocks.end(), [&](const auto& block) {
    return block.kind == kind && block.name == name;
  });
  return it != blocks.end() ? &*it : nullptr;
}

std::uint64_t fnv1a64(const std::string& text)
{
  auto hash = std::uint64_t{0xCBF29CE484222325ull};
  for (const auto c : text)
  {
    hash ^= std::uint64_t(static_cast<unsigned char>(c));
    hash *= 0x100000001B3ull;
  }
  return hash;
}

std::vector<std::string> splitLines(const std::string& text)
{
  auto lines = std::vector<std::string>{};
  auto start = std::size_t{0};
  while (start < text.size())
  {
    const auto end = text.find('\n', start);
    if (end == std::string::npos)
    {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

// the capture's form: the decl text between the separators as its hash and sizes, and
// no carriage return at a line's end
std::vector<std::string> hashDeclText(const std::vector<std::string>& lines)
{
  auto out = std::vector<std::string>{};
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    if (lines[i] == Separator)
    {
      const auto end =
        std::find(lines.begin() + std::ptrdiff_t(i) + 1, lines.end(), Separator);
      if (end != lines.end())
      {
        const auto j = std::size_t(end - lines.begin());
        auto text = std::string{};
        for (auto k = i + 1; k < j; ++k)
        {
          text += (k > i + 1 ? "\n" : "") + lines[k];
        }
        out.push_back(Separator);
        out.push_back(fmt::format(
          "#decl-text fnv1a64={:016x} bytes={} lines={}",
          fnv1a64(text),
          text.size(),
          j - i - 1));
        out.push_back(Separator);
        i = j;
        continue;
      }
    }
    auto line = lines[i];
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    out.push_back(std::move(line));
  }
  return out;
}

// the engine's image loader prints these while a touch loads a material's images; the
// preview loads no image in LP1a
bool isImageLoaderLine(const std::string& line)
{
  return line.rfind("WARNING: Couldn't load image", 0) == 0
         || line.rfind("WARNING: Couldn't load cube image", 0) == 0
         || line.rfind("Binarize File:", 0) == 0;
}

std::string declTypeOf(const std::string& kind)
{
  return kind == "material" ? "material" : "table";
}

} // namespace

TEST_CASE("Retail materials: the sweep", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the retail sweep needs the game folder");
  }

  auto mutex = std::mutex{};
  auto warnings = std::vector<std::string>{};
  auto infos = std::vector<std::string>{};
  auto options = MaterialServiceOptions{};
  options.makeSource = [path = *gamePath] {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(path)};
  };
  options.listener = [&](const MaterialServiceEvent& event) {
    if (event.kind != MaterialServiceEvent::Kind::Message)
    {
      return;
    }
    auto lock = std::scoped_lock{mutex};
    (event.level == DeclMessageKind::Info ? infos : warnings).push_back(event.text);
  };
  options.pollInterval = 0ms;
  options.parseAll = true;

  const auto started = std::chrono::steady_clock::now();
  auto service = MaterialService{std::move(options)};
  REQUIRE(service.waitForStats([](const auto& s) { return s.parseAllDone; }, 600s));
  const auto total =
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
  const auto stats = service.stats();
  service.requestStop();
  service.join();

  auto lock = std::scoped_lock{mutex};
  auto report = std::ostringstream{};
  report << "Retail sweep of " << gamePath->string() << "\n";
  report << fmt::format(
    "  decl files: {} ({:.2f} MB); decl text kept: {:.2f} MB\n",
    stats.declFiles,
    double(stats.declFileBytes) / (1024.0 * 1024.0),
    double(stats.declTextBytes) / (1024.0 * 1024.0));
  report << "  decls per type after the build (after the bulk parse where it differs):";
  for (std::size_t i = 0; i < stats.declCountsAtBuild.size(); ++i)
  {
    const auto& atBuild = stats.declCountsAtBuild[i];
    report << " " << atBuild.type << " " << atBuild.count;
    if (i < stats.declCounts.size() && stats.declCounts[i].count != atBuild.count)
    {
      report << " (" << stats.declCounts[i].count << ")";
    }
    report << ";";
  }
  report << "\n";
  report << fmt::format(
    "  materials {}, defaulted {}; tables {}\n",
    stats.materials,
    stats.defaultedMaterials,
    stats.tables);
  report << fmt::format(
    "  build {:.0f} ms (of which the container scan {:.0f} ms), bulk parse {:.0f} ms, "
    "total {:.0f} ms\n",
    stats.buildSeconds * 1000.0,
    stats.sourceSeconds * 1000.0,
    stats.parseAllSeconds * 1000.0,
    total.count() * 1000.0);
  report << fmt::format("  warnings and errors: {}\n", warnings.size());
  for (std::size_t i = 0; i < std::min<std::size_t>(warnings.size(), 20); ++i)
  {
    report << "    " << warnings[i] << "\n";
  }
  for (const auto& info : infos)
  {
    report << "  info: " << info << "\n";
  }
  std::cout << report.str() << std::flush;

  CHECK(stats.materials > 0);
  CHECK(stats.tables > 0);
  CHECK(stats.warnings == warnings.size());

  // the engine's listDecls of the same data: the same decl files and bytes
  const auto goldens = readGoldens();
  const auto* list = findBlock(goldens, "list", "decls");
  REQUIRE(list);
  auto engineFiles = 0;
  auto engineKilobytes = 0;
  for (const auto& line : list->lines)
  {
    auto decls = 0;
    if (
      std::sscanf(line.c_str(), "%d total decls is %d decl files", &decls, &engineFiles)
      == 2)
    {
      continue;
    }
    std::sscanf(line.c_str(), "%dKB in text", &engineKilobytes);
  }
  CHECK(stats.declFiles == engineFiles);
  CHECK(int(stats.declFileBytes >> 10) == engineKilobytes);

  // the engine's "previously defined" warnings of the decl files, each once
  const auto* duplicates = findBlock(goldens, "warnings", "previously-defined");
  REQUIRE(duplicates);
  auto engineDuplicates =
    std::set<std::string>{duplicates->lines.begin(), duplicates->lines.end()};
  engineDuplicates.erase("");
  auto ourDuplicates = std::set<std::string>{};
  for (const auto& warning : warnings)
  {
    if (warning.find(" previously defined at ") != std::string::npos)
    {
      ourDuplicates.insert("WARNING: " + warning);
    }
  }
  CHECK(ourDuplicates == engineDuplicates);
}

TEST_CASE("Retail materials: the engine's print output", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the goldens need the game folder");
  }

  auto source = GameDeclSource{*gamePath};
  auto decls = MaterialDecls{source};
  source.closeFiles();
  decls.takeMessages();

  auto matched = 0;
  auto compared = 0;
  auto warningDifferences = std::vector<std::string>{};
  for (const auto& block : readGoldens())
  {
    if (block.kind != "material" && block.kind != "table")
    {
      continue;
    }
    ++compared;
    CAPTURE(block.kind, block.name);

    // the engine's block: its parse warnings, then its print output
    auto engineWarnings = std::vector<std::string>{};
    auto enginePrint = std::vector<std::string>{};
    for (const auto& line : block.lines)
    {
      if (isImageLoaderLine(line))
      {
        continue;
      }
      if (enginePrint.empty() && line.rfind("WARNING: ", 0) == 0)
      {
        engineWarnings.push_back(line);
        continue;
      }
      enginePrint.push_back(line);
    }
    while (!enginePrint.empty() && enginePrint.back().empty())
    {
      enginePrint.pop_back();
    }

    // SinkBroom: the console's touch, then the print command
    const auto type = declTypeOf(block.kind);
    const auto touched = decls.touch(type, block.name);
    INFO(touched.error);
    REQUIRE(touched);
    CHECK(*touched.value);
    auto ourWarnings = std::vector<std::string>{};
    for (const auto& message : decls.takeMessages())
    {
      if (message.kind != DeclMessageKind::Info)
      {
        ourWarnings.push_back("WARNING: " + message.text);
      }
    }
    const auto printed = decls.printDecl(type, block.name);
    INFO(printed.error);
    REQUIRE(printed);
    auto ourPrint = hashDeclText(splitLines(*printed.value));
    while (!ourPrint.empty() && ourPrint.back().empty())
    {
      ourPrint.pop_back();
    }

    CHECK(ourPrint == enginePrint);
    if (ourPrint == enginePrint)
    {
      ++matched;
    }
    if (ourWarnings != engineWarnings)
    {
      warningDifferences.push_back(fmt::format(
        "{} {}: engine {} warnings, SinkBroom {}",
        block.kind,
        block.name,
        engineWarnings.size(),
        ourWarnings.size()));
    }
  }

  std::cout << fmt::format(
    "Goldens: {} of {} print blocks match the engine's exactly\n", matched, compared);
  for (const auto& difference : warningDifferences)
  {
    std::cout << "  parse warnings differ: " << difference << "\n";
  }
  std::cout << std::flush;
  CHECK(compared == 43);
  CHECK(matched == compared);
}

} // namespace tb::lit
