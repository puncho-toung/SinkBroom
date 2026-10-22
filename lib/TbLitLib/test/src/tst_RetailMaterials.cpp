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
// names (the D3-Sink build folder with base/), and are skipped without it. They compare
// SinkBroom with the engine's output that TOOLS/lit-preview/capture_goldens.py captured
// into fixture/lit/goldens (G8); the engine is not run again. The goldens and the totals
// below are a snapshot of that game folder, its loose decl files included (N.4): an edit
// under base/ fails these tests until capture_goldens.py runs again.
//
// - The sweep parses every material and table through GameDeclSource and the parse
//   thread, and asserts the decl files and bytes, the decls per type and the warnings
//   against the engine's.
// - The goldens assert the engine's implicit decls, its print output, its parse warnings
//   and its image keys for every captured decl, and the editor images against the
//   engine's own export, base/_bl/materials.json.
// - The light sweep (LP1b.3) parses every light* entityDef through the light mirror and
//   allows only the warnings the engine would print too.
// - LP1c.1: one retail map's bake through the full C8 search (its .bproc in its
//   .resources, its light grid in the GI .pk4), and the material sweep's image keys
//   named as the engine names their caches, counted, a sample read, and compared with
//   the engine's own Binarize lines.

#include "LitTestUtils.h"
#include "TestEnvironment.h"
#include "lit/EntityState.h"
#include "lit/GameDeclSource.h"
#include "lit/GameFile.h"
#include "lit/ImageCache.h"
#include "lit/ImageFallback.h"
#include "lit/LightData.h"
#include "lit/LightParse.h"
#include "lit/MapBake.h"
#include "lit/MaterialDecls.h"
#include "lit/MaterialService.h"

#include "kd/result.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

using namespace std::chrono_literals;

constexpr auto Separator = "----------";
// textureLighting_t's SL_DIFFUSE
constexpr int LightingDiffuse = 2;

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
    // R1.4: a CRLF checkout reads as an LF one
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
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

// W.7: the image loader's "Binarize File:" lines name the generated file with the usage
// digits the load chose after it began (metal04_rmao#__1200: the _rmao rename's usage,
// M-ID-DIFF-02, A3.10); the print test counts them, and the image-cache test compares
// them, name and reason, with the images the service would binarize (LP1c.2)
bool isBinarizeLine(const std::string& line)
{
  return line.rfind("Binarize File:", 0) == 0;
}

// the engine's image loader prints these while a touch loads a material's images; the
// preview loads no image in LP1a, so they are compared with the image keys instead
bool isImageLoaderLine(const std::string& line)
{
  return line.rfind("WARNING: Couldn't load image", 0) == 0
         || line.rfind("WARNING: Couldn't load cube image", 0) == 0
         || isBinarizeLine(line);
}

std::string declTypeOf(const std::string& kind)
{
  return kind == "table" ? "table" : "material";
}

// listDecls: the decls of each type, the decl files and their KB
struct EngineDeclList
{
  std::map<std::string, int> counts;
  int files = 0;
  int kilobytes = 0;
};

EngineDeclList engineDeclList(const std::vector<GoldenBlock>& goldens)
{
  auto list = EngineDeclList{};
  const auto* block = findBlock(goldens, "list", "decls");
  REQUIRE(block);
  for (const auto& line : block->lines)
  {
    auto size = 0;
    auto count = 0;
    char type[64] = {};
    auto decls = 0;
    if (
      std::sscanf(line.c_str(), "%d total decls is %d decl files", &decls, &list.files)
      == 2)
    {
      continue;
    }
    auto structures = 0;
    if (
      std::sscanf(
        line.c_str(), "%dKB in text, %dKB in structures", &list.kilobytes, &structures)
      == 2)
    {
      continue;
    }
    if (std::sscanf(line.c_str(), " %dk %d %63s", &size, &count, type) == 3)
    {
      list.counts[type] = count;
    }
  }
  return list;
}

// "@@ names <type>": the number of the engine's decls, the hash of their sorted names,
// and the names no decl file defines for SinkBroom
struct EngineNames
{
  std::size_t count = 0;
  std::string hash;
  std::vector<std::string> only;
};

std::optional<EngineNames> engineNames(
  const std::vector<GoldenBlock>& goldens, const std::string& type)
{
  const auto* block = findBlock(goldens, "names", type);
  if (!block)
  {
    return std::nullopt;
  }
  auto names = EngineNames{};
  for (const auto& line : block->lines)
  {
    if (line.rfind("count ", 0) == 0)
    {
      names.count = std::size_t(std::stoul(line.substr(6)));
    }
    else if (line.rfind("hash ", 0) == 0)
    {
      names.hash = line.substr(5);
    }
    else if (line.rfind("only ", 0) == 0)
    {
      names.only.push_back(line.substr(5));
    }
  }
  return names;
}

// The engine's warnings that SinkBroom cannot report, each with its reason. Every other
// warning of the capture must be SinkBroom's, and every one of SinkBroom's the engine's.
struct EngineOnlyWarning
{
  const char* line;
  const char* why;
};

const auto EngineOnlyWarnings = std::vector<EngineOnlyWarning>{
  {"WARNING: Unable to open resource file maps/_startup.resources",
   "the engine's start-up opens maps/_startup.resources by name "
   "(idFileSystemLocal::BeginLevelLoad for the start-up map); the game folder has none, "
   "and SinkBroom registers only the .resources files the folders list (M-FS-02)"},
};

// what a "Couldn't load image" line names: the image, and the usage and cube digits of
// its generated name (GetGeneratedName's "#__%02d%02d")
struct EngineImage
{
  std::string name;
  std::optional<int> usage;
  bool cube = false;
};

std::optional<EngineImage> engineImage(const std::string& line)
{
  const auto cubePrefix = std::string{"WARNING: Couldn't load cube image: "};
  if (line.rfind(cubePrefix, 0) == 0)
  {
    return EngineImage{line.substr(cubePrefix.size()), std::nullopt, true};
  }
  const auto prefix = std::string{"WARNING: Couldn't load image: "};
  if (line.rfind(prefix, 0) != 0)
  {
    return std::nullopt;
  }
  const auto rest = line.substr(prefix.size());
  const auto split = rest.rfind(" : ");
  const auto marker = rest.rfind("#__");
  if (
    split == std::string::npos || marker == std::string::npos || marker + 7 > rest.size())
  {
    return std::nullopt;
  }
  const auto usage = std::stoi(rest.substr(marker + 3, 2));
  const auto cube = std::stoi(rest.substr(marker + 5, 2));
  return EngineImage{rest.substr(0, split), usage, cube != 0};
}

std::vector<MaterialImageKey> imageKeysOf(const MaterialEntry& entry)
{
  auto keys = std::vector<MaterialImageKey>{};
  const auto add = [&](const std::optional<MaterialImageKey>& key) {
    if (key)
    {
      keys.push_back(*key);
    }
  };
  for (const auto& stage : entry.stages)
  {
    add(stage.image);
    for (const auto& image : stage.fragmentImages)
    {
      add(image);
    }
  }
  add(entry.lightFalloffImage);
  add(entry.fastPathBumpImage);
  add(entry.fastPathDiffuseImage);
  add(entry.fastPathSpecularImage);
  return keys;
}

// idImageManager::ImageFromFile's image name (M-ID-FN-01): the image extensions removed
// anywhere, backslashes made slashes
std::string imageName(std::string name)
{
  for (const auto* extension : {".tga", ".jpg", ".png", ".exr", ".hdr"})
  {
    for (auto at = name.find(extension); at != std::string::npos;
         at = name.find(extension))
    {
      name.erase(at, std::char_traits<char>::length(extension));
    }
  }
  std::replace(name.begin(), name.end(), '\\', '/');
  return name;
}

// idMaterial::GetEditorImage without loading: the first diffuse stage's image, else the
// first stage's, else _default; an explicit qer_editorimage instead, unless its image
// turns out to be defaulted, which only loading can tell
std::vector<std::string> editorImageCandidates(const MaterialEntry& entry)
{
  auto fallback = std::optional<MaterialImageKey>{};
  const auto diffuse =
    std::find_if(entry.stages.begin(), entry.stages.end(), [](const auto& s) {
      return s.lighting == LightingDiffuse;
    });
  if (diffuse != entry.stages.end())
  {
    fallback = diffuse->image;
  }
  if (!fallback && !entry.stages.empty())
  {
    fallback = entry.stages.front().image;
  }
  const auto fallbackName = fallback ? fallback->name : std::string{"_default"};
  if (entry.editorImageName.empty())
  {
    return {fallbackName};
  }
  return {imageName(entry.editorImageName), fallbackName};
}

// base/_bl/materials.json, the engine's exportMaterialsToBlender: each material's name
// and its editorImage, the one field the export writes
std::map<std::string, std::string> engineEditorImages(const std::filesystem::path& path)
{
  auto images = std::map<std::string, std::string>{};
  auto stream = std::istringstream{readTextFile(path)};
  auto line = std::string{};
  auto material = std::string{};
  const auto quoted = [](const std::string& text, const std::size_t from) {
    const auto start = text.find('"', from);
    const auto end = start == std::string::npos ? start : text.find('"', start + 1);
    return end == std::string::npos ? std::string{}
                                    : text.substr(start + 1, end - start - 1);
  };
  while (std::getline(stream, line))
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    const auto key = quoted(line, 0);
    if (key == "editorImage")
    {
      images[material] = quoted(line, line.find(':'));
    }
    else if (!key.empty() && line.find('{') != std::string::npos && key != "materials")
    {
      material = key;
    }
  }
  return images;
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
  options.makeSource = [path = *gamePath](const std::atomic<bool>& cancel) {
    return std::unique_ptr<DeclSource>{std::make_unique<GameDeclSource>(path, &cancel)};
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
  auto created = MaterialService::create(std::move(options));
  REQUIRE(created.is_success());
  auto service = std::move(created).value();
  REQUIRE(
    service->waitForStatsForTesting([](const auto& s) { return s.parseAllDone; }, 600s));
  const auto total =
    std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
  const auto stats = service->stats();
  const auto table = service->table();
  service->requestStop();
  service->join();

  // C25, Q18: the retail materials that set each geometry flag, each entry counted once
  auto entries = std::set<const MaterialEntry*>{};
  table->materials.forEach([&](const auto&, const auto& slot) {
    if (slot->entry)
    {
      entries.insert(slot->entry.get());
    }
  });
  auto mikktspace = std::vector<std::string>{};
  auto unsmoothedTangents = 0;
  auto createBackSides = 0;
  // LP1c.2 (C21): every parsed material's editor-image size, Ready or Default with why
  auto readySizes = 0;
  auto defaultReasons = std::map<std::string, std::vector<std::string>>{};
  for (const auto* entry : entries)
  {
    if (entry->mikktspace)
    {
      mikktspace.push_back(entry->name);
    }
    unsmoothedTangents += entry->unsmoothedTangents ? 1 : 0;
    createBackSides += entry->createBackSides ? 1 : 0;
    if (entry->editorImageSize.state == EditorImageSize::State::Ready)
    {
      ++readySizes;
    }
    else
    {
      // the reason without the image's name, so that alike reasons group
      const auto& reason = entry->editorImageSize.reason;
      const auto kind = reason.substr(0, reason.find(':'));
      defaultReasons[kind.empty() ? std::string{"(none)"} : kind].push_back(
        entry->name + ": " + reason);
    }
  }

  auto lock = std::scoped_lock{mutex};
  auto report = std::ostringstream{};
  report << "Retail sweep of " << gamePath->string() << "\n";
  report << fmt::format(
    "  containers: {}; decl files: {} ({:.2f} MB); decl text kept: {:.2f} MB\n",
    stats.sourceContainers,
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
  report << fmt::format(
    "  geometry flags of {} parsed materials (C25, Q18): mikktspace {}, "
    "unsmoothedTangents {}, createBackSides {}\n",
    entries.size(),
    mikktspace.size(),
    unsmoothedTangents,
    createBackSides);
  for (const auto& name : mikktspace)
  {
    report << "  mikktspace: " << name << "\n";
  }
  auto defaultSizes = std::size_t{0};
  for (const auto& [kind, materials] : defaultReasons)
  {
    defaultSizes += materials.size();
  }
  report << fmt::format(
    "  editor-image sizes (C21) of {} parsed materials: {} Ready, {} Default\n",
    entries.size(),
    readySizes,
    defaultSizes);
  for (const auto& [kind, materials] : defaultReasons)
  {
    report << fmt::format("  Default x{}: {}\n", materials.size(), kind);
    for (std::size_t i = 0; i < std::min<std::size_t>(materials.size(), 8); ++i)
    {
      report << "    " << materials[i] << "\n";
    }
  }
  for (const auto& info : infos)
  {
    report << "  info: " << info << "\n";
  }
  std::cout << report.str() << std::flush;

  CHECK(stats.materials == 7733);
  CHECK(stats.tables == 309);
  // C21: every size resolved, Ready or Default with its reason
  CHECK(readySizes + int(defaultSizes) == int(entries.size()));
  CHECK(readySizes > 0);
  CHECK(defaultReasons.count("(none)") == 0u);
  CHECK(stats.defaultedMaterials == 15);
  CHECK(stats.warnings == warnings.size());
  CHECK(stats.failedWorkItems == 0u);
  // W.1: the decl guards refuse nothing here; newpdas/enpro_hammer.pda, a string of 1075
  // characters whose first dot is its fifth, is read
  CHECK(std::none_of(warnings.begin(), warnings.end(), [](const auto& warning) {
    return warning.find("holds a token of 1024") != std::string::npos
           || warning.find("exceeded 256 stages") != std::string::npos;
  }));

  const auto goldens = readGoldens();
  const auto list = engineDeclList(goldens);

  // the engine's listDecls of the same data: the same decl files and bytes
  CHECK(stats.declFiles == list.files);
  CHECK(int(stats.declFileBytes >> 10) == list.kilobytes);

  // A3.4: the decls of every type. The engine has more only where its start-up made
  // implicit decls, which no decl file defines: the golden test asserts their names
  for (const auto& count : stats.declCountsAtBuild)
  {
    CAPTURE(count.type, count.count);
    const auto names = engineNames(goldens, count.type);
    const auto implicit = names ? int(names->only.size()) : 0;
    REQUIRE(list.counts.count(count.type) == 1);
    CHECK(list.counts.at(count.type) == count.count + implicit);
  }
  // 7788 = 7733 + 55 materials, 3091 = 3090 + 1 sound shader
  CHECK(list.counts.at("material") == 7788);
  CHECK(engineNames(goldens, "material")->only.size() == 55u);
  CHECK(list.counts.at("sound") == 3091);
  CHECK(engineNames(goldens, "sound")->only.size() == 1u);

  // A3.3: SinkBroom's warnings are the engine's, as a set: the capture touched every decl
  // whose parse warns here, so the engine reported them too
  const auto* all = findBlock(goldens, "warnings", "all");
  REQUIRE(all);
  auto engineWarnings = std::set<std::string>{};
  for (const auto& line : all->lines)
  {
    if (!line.empty() && !isImageLoaderLine(line))
    {
      engineWarnings.insert(line);
    }
  }
  for (const auto& engineOnly : EngineOnlyWarnings)
  {
    CAPTURE(engineOnly.line, engineOnly.why);
    CHECK(engineWarnings.erase(engineOnly.line) == 1u);
  }
  auto ourWarnings = std::set<std::string>{};
  for (const auto& warning : warnings)
  {
    ourWarnings.insert("WARNING: " + warning);
  }
  CHECK(ourWarnings.size() == warnings.size());
  CHECK(ourWarnings.size() == 51u);
  CHECK(ourWarnings == engineWarnings);
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
  REQUIRE(decls.constructionError().empty());
  decls.takeMessages();
  const auto goldens = readGoldens();

  SECTION("the engine's implicit decls are the names no decl file defines")
  {
    // A3.4: every decl SinkBroom has, the engine has too; the names only the engine has
    // have no decl in the same files (the same 925 files and KB, asserted by the sweep),
    // so its start-up made them implicitly (fonts, GUI and SWF assets, _white, and a
    // material and a sound shader named "}" from a malformed reference)
    for (const auto* type : {"material", "sound"})
    {
      CAPTURE(type);
      const auto names = engineNames(goldens, type);
      REQUIRE(names);
      auto ours = decls.declNames(type);
      for (const auto& name : names->only)
      {
        CAPTURE(name);
        CHECK(std::find(ours.begin(), ours.end(), name) == ours.end());
        const auto touched = decls.touch(type, name);
        REQUIRE(touched);
        CHECK_FALSE(*touched.value);
      }
      ours.insert(ours.end(), names->only.begin(), names->only.end());
      std::sort(ours.begin(), ours.end());
      auto joined = std::string{};
      for (std::size_t i = 0; i < ours.size(); ++i)
      {
        joined += (i ? "\n" : "") + ours[i];
      }
      CHECK(ours.size() == names->count);
      CHECK(fmt::format("{:016x}", fnv1a64(joined)) == names->hash);
    }
    decls.takeMessages();
  }

  SECTION("the print output, the parse warnings and the image keys")
  {
    auto compared = 0;
    auto implicitBlocks = 0;
    auto defaultedBlocks = 0;
    auto imageLines = 0;
    auto binarizeLines = 0;
    for (const auto& block : goldens)
    {
      if (block.kind != "material" && block.kind != "table" && block.kind != "implicit")
      {
        continue;
      }
      ++compared;
      CAPTURE(block.kind, block.name);

      // the engine's block: its parse warnings and image loads, then its print output
      auto engineWarnings = std::vector<std::string>{};
      auto engineImages = std::vector<EngineImage>{};
      auto enginePrint = std::vector<std::string>{};
      for (const auto& line : block.lines)
      {
        if (isImageLoaderLine(line))
        {
          if (auto image = engineImage(line))
          {
            engineImages.push_back(std::move(*image));
          }
          binarizeLines += isBinarizeLine(line) ? 1 : 0;
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

      // SinkBroom: the console's touch, then the print command; an implicit material has
      // no decl to touch, so the lookup makes it, as the engine's start-up did
      const auto type = declTypeOf(block.kind);
      const auto touched = decls.touch(type, block.name);
      INFO(touched.error);
      REQUIRE(touched);
      if (block.kind == "implicit")
      {
        ++implicitBlocks;
        CHECK_FALSE(*touched.value);
        const auto made = decls.lookup(block.name);
        REQUIRE(made);
        CHECK((*made.value)->implicit);
      }
      else
      {
        CHECK(*touched.value);
      }
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
      // A3.3: the parse warnings of each block, asserted
      CHECK(ourWarnings == engineWarnings);

      if (type != "material")
      {
        continue;
      }
      const auto entry = decls.lookup(block.name);
      REQUIRE(entry);
      defaultedBlocks += (*entry.value)->defaulted ? 1 : 0;

      // A3.2: each image the engine failed to load names one of SinkBroom's image keys,
      // with the generated name's usage and cube state. The engine retries a failed load
      // at every touch, so an image of an earlier block can show again. A defaulted
      // material's entry holds the default definition's stages, so the images its failed
      // parse made are looked for among the registry's image records.
      const auto keys =
        (*entry.value)->defaulted ? decls.imageKeys() : imageKeysOf(**entry.value);
      for (const auto& image : engineImages)
      {
        ++imageLines;
        CAPTURE(image.name, image.usage.value_or(-1), image.cube);
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& k) {
          return k.name == image.name && (k.cube != 0) == image.cube
                 && (!image.usage || k.usage == *image.usage);
        });
        CHECK(key != keys.end());
      }
    }
    decls.takeMessages();

    std::cout << fmt::format(
      "Goldens: {} blocks compared ({} implicit, {} defaulted materials), {} image "
      "lines, {} Binarize lines counted\n",
      compared,
      implicitBlocks,
      defaultedBlocks,
      imageLines,
      binarizeLines);
    // 34 materials and 9 tables, the 15 defaulted materials, 3 more tables that warn,
    // and the 55 implicit materials
    CHECK(compared == 116);
    CHECK(implicitBlocks == 55);
    CHECK(defaultedBlocks == 15);
    // W.7: the "Couldn't load" lines, each compared above, and the Binarize lines, only
    // counted until LP1c (isBinarizeLine, A3.10)
    CHECK(imageLines == 27);
    CHECK(binarizeLines == 3);
  }

  SECTION("the editor images agree with the engine's export")
  {
    // A3.2: a stage field the print does not show. Every material the export lists and
    // SinkBroom has is compared, from loose files, .pk4 files and .resources files alike
    // (W.7), so an edit of a loose material's editor image fails this test until the
    // engine's export (base/_bl/materials.json) is made again
    const auto exported =
      engineEditorImages(*gamePath / "base" / "_bl" / "materials.json");
    REQUIRE(exported.size() > 7000u);

    auto checked = std::map<int, int>{};
    auto mismatches = std::map<int, std::vector<std::string>>{};
    for (int i = 0; i < decls.materialCount(); ++i)
    {
      const auto entry = decls.lookupIndex(i);
      if (!entry)
      {
        continue;
      }
      const auto& material = **entry.value;
      const auto origin = source.origin(material.sourceFile);
      const auto it = exported.find(material.name);
      if (it == exported.end() || !origin)
      {
        continue;
      }
      const auto kind = int(origin->kind);
      ++checked[kind];
      // the same image record at the pinned commit: the export (April 2024) predates the
      // engine's stripping of every image extension (95336f49, January 2025: it wrote
      // textures/common/origin.png), and a record is found case-insensitively and keeps
      // the spelling of the material that made it first, which the engine's and
      // SinkBroom's parse orders choose differently (ice2_d against ice2_D, A3.10)
      const auto exportedImage = toLowerAscii(imageName(it->second));
      auto candidates = editorImageCandidates(material);
      for (auto& candidate : candidates)
      {
        candidate = toLowerAscii(candidate);
      }
      if (
        std::find(candidates.begin(), candidates.end(), exportedImage)
        == candidates.end())
      {
        mismatches[kind].push_back(fmt::format(
          "{} ({}): engine {}, SinkBroom {}",
          material.name,
          material.sourceFile,
          it->second,
          candidates.front()));
      }
    }
    decls.takeMessages();
    for (const auto& [kind, count] : checked)
    {
      std::cout << fmt::format(
        "Editor images from {}: {} materials compared, {} differ\n",
        kind == 0   ? "loose files"
        : kind == 1 ? ".pk4 files"
                    : ".resources files",
        count,
        mismatches[kind].size());
      for (std::size_t i = 0; i < std::min<std::size_t>(mismatches[kind].size(), 25); ++i)
      {
        std::cout << "  " << mismatches[kind][i] << "\n";
      }
    }
    std::cout << std::flush;
    auto total = 0;
    for (const auto& [kind, count] : checked)
    {
      total += count;
      CHECK(mismatches[kind].empty());
    }
    // every material the export lists and SinkBroom still has (7661 of 7733)
    CHECK(total > 7600);
  }
}

namespace
{

std::string jsonString(const std::string& text)
{
  auto out = std::string{"\""};
  for (const auto c : text)
  {
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += c;
    }
    else if (static_cast<unsigned char>(c) < 0x20)
    {
      out += fmt::format("\\u{:04x}", int(static_cast<unsigned char>(c)));
    }
    else
    {
      out += c;
    }
  }
  return out + "\"";
}

std::string jsonList(const std::vector<std::string>& items)
{
  auto out = std::string{"["};
  for (std::size_t i = 0; i < items.size(); ++i)
  {
    out += (i ? ",\n  " : "\n  ") + jsonString(items[i]);
  }
  return out + "]";
}

std::vector<std::string> problems(std::vector<DeclMessage> messages)
{
  auto texts = std::vector<std::string>{};
  for (auto& message : messages)
  {
    if (message.kind != DeclMessageKind::Info)
    {
      texts.push_back(std::move(message.text));
    }
  }
  return texts;
}

} // namespace

// What TOOLS/lit-preview/capture_goldens.py needs from SinkBroom to choose what the
// engine touches: every decl name per type, the defaulted materials and the decls whose
// parse warns. Written to TB_LIT_DUMP_PATH as JSON; skipped without it.
TEST_CASE("Retail materials: dump for the engine capture", "[.][retail-dump]")
{
  const auto gamePath = retailGamePath();
  const auto* dumpPath = std::getenv("TB_LIT_DUMP_PATH");
  if (!gamePath || !dumpPath || !*dumpPath)
  {
    SKIP("TB_LIT_GAME_PATH and TB_LIT_DUMP_PATH name the game and the dump");
  }

  auto source = GameDeclSource{*gamePath};
  auto decls = MaterialDecls{source};
  source.closeFiles();
  REQUIRE(decls.constructionError().empty());
  const auto loadWarnings = problems(decls.takeMessages());

  auto defaulted = std::vector<std::string>{};
  auto warned = std::vector<std::string>{};
  auto warnings = loadWarnings;
  for (int i = 0; i < decls.materialCount(); ++i)
  {
    const auto entry = decls.lookupIndex(i);
    const auto name = decls.materialName(i);
    if (entry && (*entry.value)->defaulted)
    {
      defaulted.push_back(name);
    }
    auto found = problems(decls.takeMessages());
    if (!found.empty() || !entry)
    {
      warned.push_back("material " + name);
    }
    warnings.insert(warnings.end(), found.begin(), found.end());
  }
  for (int i = 0; i < decls.tableCount(); ++i)
  {
    const auto table = decls.parseTable(i);
    auto found = problems(decls.takeMessages());
    if (!found.empty() || !table)
    {
      warned.push_back("table " + (table ? *table.value : std::to_string(i)));
    }
    warnings.insert(warnings.end(), found.begin(), found.end());
  }

  auto json = std::string{"{\n\"types\": {"};
  auto first = true;
  for (const auto& count : decls.declCounts())
  {
    json += (first ? "\n" : ",\n") + jsonString(count.type) + ": "
            + jsonList(decls.declNames(count.type));
    first = false;
  }
  json += "},\n\"defaulted\": " + jsonList(defaulted);
  json += ",\n\"warned\": " + jsonList(warned);
  json += ",\n\"loadWarnings\": " + jsonList(loadWarnings);
  json += ",\n\"warnings\": " + jsonList(warnings) + "\n}\n";
  writeTextFile(dumpPath, json);
  std::cout << fmt::format(
    "Dump: {} materials, {} defaulted, {} warned decls, {} warnings -> {}\n",
    decls.materialCount(),
    defaulted.size(),
    warned.size(),
    warnings.size(),
    dumpPath);
}

} // namespace tb::lit

namespace tb::lit
{
namespace
{

// the whole-word, case-insensitive occurrences of word in text (comments included, so it
// can only overcount the keyword)
std::size_t countWord(const std::string& text, const std::string_view word)
{
  const auto isWordChar = [](const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  };
  const auto lower = [](const char c) {
    return char(std::tolower(static_cast<unsigned char>(c)));
  };
  auto count = std::size_t{0};
  for (auto i = std::size_t{0}; i + word.size() <= text.size(); ++i)
  {
    if (i > 0 && isWordChar(text[i - 1]))
    {
      continue;
    }
    if (i + word.size() < text.size() && isWordChar(text[i + word.size()]))
    {
      continue;
    }
    if (std::equal(
          word.begin(),
          word.end(),
          text.begin() + std::ptrdiff_t(i),
          [&](const char a, const char b) { return lower(a) == lower(b); }))
    {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE("Retail materials: the mikktspace keyword (Q18)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the sweep needs the game folder");
  }

  // Q18 (15-LP1BC-PACKAGES.md): every decl file the registry reads, from the containers
  // and the user's loose files alike (GameDeclSource's search, read-only), is searched
  // for the keyword; MikkTSpace is copied only if one is found (M-ID-DIFF-04)
  auto source = GameDeclSource{*gamePath};
  auto files = std::size_t{0};
  auto occurrences = std::size_t{0};
  auto withKeyword = std::vector<std::string>{};
  for (const auto& folder : MaterialDecls::engineDeclFolders())
  {
    for (const auto& name : source.listFiles(folder.folder, folder.extension))
    {
      const auto path = folder.folder + "/" + name;
      auto time = std::int64_t{0};
      const auto text = source.readFile(path, &time);
      REQUIRE(text);
      ++files;
      if (const auto count = countWord(*text, "mikktspace"))
      {
        occurrences += count;
        withKeyword.push_back(path);
      }
    }
  }
  source.closeFiles();

  std::cout << fmt::format(
    "Q18: {} decl files searched, {} occurrences of the mikktspace keyword{}\n",
    files,
    occurrences,
    withKeyword.empty() ? "" : " in " + fmt::format("{}", fmt::join(withKeyword, ", ")));
  CHECK(files > 900);
  CHECK(occurrences == 0);
}

TEST_CASE(
  "Retail materials: the light entityDefs through the light mirror", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the sweep needs the game folder");
  }

  // LP1b.3: every retail entityDef named light* parses through the light mirror (its
  // merged keys as a light's spawn args, no map keys) with no warning but those the
  // engine would print too: bad target info (gameLocal.Printf), levels <= 0
  // (gameLocal.Error), an unknown math_type (idLib::Warning), an inverse that fails
  // (idLib::Warning)
  auto source = GameDeclSource{*gamePath};
  auto decls = MaterialDecls{source};
  source.closeFiles();
  REQUIRE(decls.constructionError().empty());
  decls.takeMessages();

  const auto engineWarns = [](const std::string& warning) {
    return warning.find("has bad target info") != std::string::npos
           || warning.rfind("Invalid light level", 0) == 0
           || warning.rfind("math_type '", 0) == 0
           || warning.find("invert failed") != std::string::npos;
  };

  auto parsed = 0;
  auto idLights = 0;
  auto listed = std::vector<std::string>{};
  auto unexpected = std::vector<std::string>{};
  for (const auto& name : decls.declNames("entityDef"))
  {
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
      return char(std::tolower(c));
    });
    if (lower.rfind("light", 0) != 0)
    {
      continue;
    }
    auto def = decls.entityDef(name);
    REQUIRE(def);
    const auto& keys = (*def.value)->keys;
    const auto light = parseLight(mergeSpawnArgs({{"classname", name}}, &keys));
    const auto derived = deriveLight(light, nullptr, nullptr);
    ++parsed;
    const auto* spawnclass = findKey(keys, "spawnclass");
    if (spawnclass && *spawnclass == "idLight")
    {
      ++idLights;
    }
    for (const auto* warnings : {&light.warnings, &derived.warnings})
    {
      for (const auto& warning : *warnings)
      {
        listed.push_back(fmt::format("{}: {}", name, warning));
        if (!engineWarns(warning))
        {
          unexpected.push_back(listed.back());
        }
      }
    }
    if (name == "light")
    {
      // the fixture's light goldens merge no entityDef (tst_LightData.cpp): the retail
      // light def sets none of the keys the parse and the derivation read, or sets it to
      // the engine's default
      const auto defaults =
        std::map<std::string, std::string>{{"levels", "1"}, {"start_off", "0"}};
      for (const auto* key :
           {"light_origin",   "origin",    "light_target", "light_up",     "light_right",
            "light_start",    "light_end", "light_center", "light_radius", "light",
            "light_rotation", "rotation",  "light_angles", "angles",       "angle",
            "_color",         "texture",   "noshadows",    "parallel",     "style",
            "num_styles",     "levels",    "start_off",    "math_enabled"})
      {
        CAPTURE(key);
        const auto* value = findKey(keys, key);
        if (const auto d = defaults.find(key); d != defaults.end() && value)
        {
          CHECK(*value == d->second);
        }
        else
        {
          CHECK_FALSE(value);
        }
      }
      std::cout << fmt::format(
        "entityDef light: {}\n",
        fmt::join(
          [&] {
            auto texts = std::vector<std::string>{};
            for (const auto& [k, v] : keys)
            {
              // the editor's keys say nothing the game reads
              if (k.rfind("editor_", 0) != 0)
              {
                texts.push_back(fmt::format("{} = {}", k, v));
              }
            }
            return texts;
          }(),
          "; "));
    }
  }
  decls.takeMessages();
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  std::cout << fmt::format(
    "light entityDefs: {} named light*, {} of them spawnclass idLight; {} warnings{}\n",
    parsed,
    idLights,
    listed.size(),
    listed.empty() ? "" : ":\n  " + fmt::format("{}", fmt::join(listed, "\n  ")));
  CHECK(parsed > 0);
  CHECK(unexpected.empty());
}

TEST_CASE("Retail materials: bakes and image caches (LP1c.1)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the sweep needs the game folder");
  }
  const auto started = std::chrono::steady_clock::now();
  auto source = GameDeclSource{*gamePath};

  // C10, D55: one retail map's bake through the full search; retail ships the area BSP
  // as a .bproc in its .resources and the light grid in the GI .pk4
  const auto mapName = std::string{"maps/game/admin"};
  const auto bake = readMapBake(source, mapName);
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  const auto foundIn = [&](const std::string& path) {
    for (const auto& file : bake.files)
    {
      if (file.path == path)
      {
        return file.probe.kind == GameFileProbe::Kind::Missing ? std::string{"none"}
               : file.probe.container.empty()                  ? std::string{"loose"}
                                                               : file.probe.container;
      }
    }
    return std::string{"not consulted"};
  };
  REQUIRE(bake.tree);
  std::cout << fmt::format(
    "Retail bake of {}: geometry {} ({} areas, {} nodes, {} portals, {} models; .proc "
    "{}, .bproc {}), bake base {}, grid {} ({} entries; .lightgrid {}, .blightgrid {}), "
    "SH probes {}, dropped: {}\n",
    mapName,
    bake.geometry == MapBake::Geometry::Proc ? "proc" : "bproc",
    bake.tree->numAreas,
    bake.tree->nodes.size(),
    bake.tree->portals.size(),
    bake.tree->models.size(),
    foundIn(mapName + ".proc"),
    foundIn("generated/" + mapName + ".bproc"),
    bake.bakeBase,
    bake.grid == MapBake::Grid::Binary ? "binary"
    : bake.grid == MapBake::Grid::Text ? "text"
                                       : "none",
    bake.lightGrid ? bake.lightGrid->areas.size() : std::size_t{0},
    foundIn(bake.bakeBase + ".lightgrid"),
    foundIn("generated/" + bake.bakeBase + ".blightgrid"),
    bake.shProbes ? fmt::format("{}", bake.shProbes->size()) : std::string{"none"},
    bake.dropReason.empty() ? std::string{"no"} : bake.dropReason);
  CHECK(bake.geometry != MapBake::Geometry::None);
  CHECK(bake.grid == MapBake::Grid::Binary);
  CHECK(foundIn(bake.bakeBase + ".lightgrid") == "_rbdoom_global_illumination_data.pk4");
  CHECK(
    foundIn("generated/" + bake.bakeBase + ".blightgrid")
    == "_rbdoom_global_illumination_data.pk4");
  CHECK(bake.dropReason.empty());

  // its light grid's irradiance atlases resolve to the GI .pk4's caches (SC105)
  auto decls = MaterialDecls{source};
  REQUIRE(decls.constructionError().empty());
  auto gridImages = 0;
  auto noAtlas = std::vector<std::string>{};
  REQUIRE(bake.lightGrid);
  for (const auto& entry : bake.lightGrid->areas)
  {
    const auto name = decls.imageCacheName(lightGridImageKey(bake.bakeBase, entry.area));
    REQUIRE(name);
    const auto slot = resolveImageCache(source, *name.value);
    if (slot.state == ImageCacheSlot::State::Ready)
    {
      ++gridImages;
      CHECK(slot.image->textureType == engine_image::Type2D);
      continue;
    }
    // an area the baker made no atlas for: report its grid
    auto valid = 0;
    for (const auto& point : entry.points)
    {
      valid += point.valid ? 1 : 0;
    }
    noAtlas.push_back(fmt::format(
      "area {} ({} points, {} valid, bounds {} {} {})",
      entry.area,
      entry.points.size(),
      valid,
      entry.bounds[0],
      entry.bounds[1],
      entry.bounds[2]));
    // the baker makes no atlas for an area without grid points
    CHECK(slot.state == ImageCacheSlot::State::Missing);
    CHECK(entry.points.empty());
  }
  std::cout << fmt::format(
    "Retail light grid atlases: {} of {} Ready; none for {}\n",
    gridImages,
    bake.lightGrid->areas.size(),
    noAtlas.empty() ? std::string{"none"} : fmt::format("{}", fmt::join(noAtlas, ", ")));
  source.closeFiles();

  // the image keys of the material sweep (every material parsed, as the sweep parses
  // them) and their caches, through the probe: the cache file, or the BFHACK name's
  for (auto i = 0; i < decls.materialCount(); ++i)
  {
    static_cast<void>(decls.lookupIndex(i));
  }
  const auto keys = decls.imageKeys();
  auto generated = 0;
  auto found = 0;
  auto viaBfhack = 0;
  auto failed = 0;
  auto names = std::set<std::string>{};
  auto existing = std::vector<ImageCacheName>{};
  for (const auto& key : keys)
  {
    const auto name = decls.imageCacheName(key);
    if (!name)
    {
      ++failed;
      continue;
    }
    names.insert(name.value->generatedName);
    if (name.value->generated)
    {
      ++generated;
    }
    else if (source.probe(name.value->file).exists())
    {
      ++found;
      existing.push_back(*name.value);
    }
    else if (
      source.usingResourceFiles() && !name.value->bfhackFile.empty()
      && source.probe(name.value->bfhackFile).exists())
    {
      ++viaBfhack;
      existing.push_back(*name.value);
    }
  }

  // every 25th found cache read and checked as the engine checks it, and every light
  // image's (TD_LIGHT, RGB565 in the BFG's data)
  auto read = 0;
  auto ready = 0;
  auto rgb565 = 0;
  auto staleReasons = std::map<std::string, int>{};
  for (std::size_t i = 0; i < existing.size(); ++i)
  {
    if (i % 25 != 0 && existing[i].usage != engine_image::UsageLight)
    {
      continue;
    }
    const auto slot = resolveImageCache(source, existing[i]);
    source.closeFiles();
    ++read;
    if (slot.state == ImageCacheSlot::State::Ready)
    {
      ++ready;
      if (slot.image->storedFormat == engine_image::FormatRgb565)
      {
        ++rgb565;
        // the expansion doubles every level's bytes
        auto stored = std::size_t{0};
        for (const auto& level : slot.image->levels)
        {
          stored += level.size / 2;
        }
        CHECK(slot.image->data.size() == stored * 2);
      }
    }
    else
    {
      ++staleReasons[slot.reason.substr(0, slot.reason.find('\''))];
    }
  }
  static_cast<void>(decls.takeMessages());
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));

  // W.7, A3.10: the engine's own Binarize lines name the post-load usages of the
  // materialorb's images (the _s specular made _rmao, #__1200): the names are among ours
  auto binarized = std::vector<std::string>{};
  for (const auto& block : readGoldens())
  {
    for (const auto& line : block.lines)
    {
      if (isBinarizeLine(line))
      {
        const auto start = line.find('\'') + 1;
        binarized.push_back(line.substr(start, line.find('\'', start) - start));
      }
    }
  }
  auto stale = std::vector<std::string>{};
  for (const auto& [reason, count] : staleReasons)
  {
    stale.push_back(fmt::format("{} x{}", reason, count));
  }
  std::cout << fmt::format(
    "Retail image keys: {} keys, {} generated by the engine, {} caches found ({} through "
    "the BFHACK name), {} without a cache, {} names refused; {} light grid atlases "
    "Ready; "
    "read {} caches: {} Ready ({} RGB565 expanded), stale: {}; Binarize names {}; {:.0f} "
    "ms\n",
    keys.size(),
    generated,
    found + viaBfhack,
    viaBfhack,
    int(keys.size()) - generated - found - viaBfhack - failed,
    failed,
    gridImages,
    read,
    ready,
    rgb565,
    stale.empty() ? std::string{"none"} : fmt::format("{}", fmt::join(stale, ", ")),
    fmt::format("{}", fmt::join(binarized, ", ")),
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
      .count());
  REQUIRE(binarized.size() == 3u);
  for (const auto& name : binarized)
  {
    CAPTURE(name);
    CHECK(names.count(name) == 1u);
  }

  // A3.10 closes (LP1c.2): each golden block's Binarize lines, name and reason, equal the
  // images of that material the service would binarize after the time call (no cache, or
  // one the engine would not use), with the engine's reason; and the source fallback
  // makes each from its source with the copied loaders
  auto fromSource = std::vector<std::string>{};
  for (const auto& block : readGoldens())
  {
    auto engineBinarized = std::set<std::pair<std::string, std::string>>{};
    for (const auto& line : block.lines)
    {
      if (isBinarizeLine(line))
      {
        const auto start = line.find('\'') + 1;
        const auto name = line.substr(start, line.find('\'', start) - start);
        const auto reasonStart = line.find("reason '") + 8;
        engineBinarized.emplace(
          name, line.substr(reasonStart, line.size() - reasonStart - 1));
      }
    }
    if (engineBinarized.empty())
    {
      continue;
    }
    CAPTURE(block.name);
    const auto entry = decls.lookup(block.name);
    REQUIRE(entry);
    auto ourBinarized = std::set<std::pair<std::string, std::string>>{};
    for (const auto& key : imageKeysOf(**entry.value))
    {
      const auto name = decls.imageCacheName(key);
      REQUIRE(name);
      // a stage's image is also a fast-path image: once each
      if (
        name.value->generated
        || std::any_of(ourBinarized.begin(), ourBinarized.end(), [&](const auto& made) {
             return made.first == name.value->generatedName;
           }))
      {
        continue;
      }
      const auto slot = resolveImageCache(source, *name.value);
      if (slot.state == ImageCacheSlot::State::Ready)
      {
        continue;
      }
      ourBinarized.emplace(name.value->generatedName, slot.reason);
      const auto made = resolveImage(decls, source, key);
      INFO(made.sourceReason);
      REQUIRE(made.image);
      CHECK(made.image->fromSource);
      fromSource.push_back(fmt::format(
        "{} {}x{} format {} colour {} ({} levels)",
        name.value->generatedName,
        made.image->width,
        made.image->height,
        made.image->storedFormat,
        made.image->colorFormat,
        made.image->numLevels));
    }
    CHECK(ourBinarized == engineBinarized);
  }
  source.closeFiles();
  static_cast<void>(decls.takeMessages());
  static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  std::cout << fmt::format(
    "Retail Binarize images made from their sources: {}\n",
    fmt::format("{}", fmt::join(fromSource, "; ")));
  CHECK(fromSource.size() == 3u);
  CHECK(failed == 0);
  CHECK(found > 0);
  CHECK(ready > 0);
  CHECK(gridImages + int(noAtlas.size()) == bake.tree->numAreas);
}

} // namespace tb::lit
