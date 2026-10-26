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

// LP7.1a (C51): the retail model sweep's binary half: every engine binary of a render
// model the C8 search finds (generated/rendermodels/**.b*: the .resources and .pk4
// entries, the loose files) is read through readBinaryModel, a .bglb's GLMB tail too;
// .bmd5mesh is MD5B, another format, counted apart (P7.19). LP7.1b: the source half,
// every loose base/models/** source of a model extension through the source path (the
// validators and the copied loaders), each loaded record against the engine's binary of
// it when the binary-first path would take one. LP7.5: the .glb and .gltf sources too
// (K40 closed), each reported on its own line: the outcome, its nodes, primitives and
// vertices, its binary's match and its load time; the longest parse-thread item (the
// read, the validator, the parse and the plan) and pool job (the finish) of the sweep.

#include "ModelValidators.h"
#include "lit/BprocModel.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/ModelLoading.h"
#include "lit/ModelRecord.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::optional<std::filesystem::path> retailGamePath()
{
  const auto* value = std::getenv("TB_LIT_GAME_PATH");
  if (!value || !*value)
  {
    return std::nullopt;
  }
  return std::filesystem::path{value};
}

std::string lowered(std::string text)
{
  std::ranges::transform(text, text.begin(), [](const char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
  });
  return text;
}

std::string extensionOf(const std::string& name)
{
  const auto dot = name.find_last_of('.');
  const auto slash = name.find_last_of('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
  {
    return {};
  }
  return lowered(name.substr(dot));
}

struct Count
{
  std::size_t files = 0;
  std::size_t bytes = 0;
  double milliseconds = 0.0;
  std::size_t failed = 0;
};

} // namespace

TEST_CASE("Retail models: the engine's binaries read (LP7.1a, C51)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the retail sweep needs the game folder");
  }

  const auto started = std::chrono::steady_clock::now();
  auto source = GameFileSource{*gamePath};
  const auto prefix = std::string{"generated/rendermodels/"};

  // the names, once each (the C8 read finds the copy that wins)
  auto names = std::map<std::string, std::string>{};
  auto extensions = std::vector<std::string>{
    ".bobj", ".base", ".blwo", ".bma", ".bglb", ".bgltf", ".bmd5mesh", ".bmd3", ".bflt"};
  for (const auto& entry : source.containerEntryNamesForTesting())
  {
    const auto name = lowered(entry);
    const auto extension = extensionOf(name);
    if (name.rfind(prefix, 0) == 0 && extension.size() > 2 && extension[1] == 'b')
    {
      names.try_emplace(name, entry);
      if (std::ranges::find(extensions, extension) == extensions.end())
      {
        extensions.push_back(extension);
      }
    }
  }
  const auto containerNames = names.size();
  // the loose files: a listing recurses below the folder on Windows (C8)
  for (const auto& extension : extensions)
  {
    for (const auto& name : source.listFiles("generated/rendermodels", extension))
    {
      names.try_emplace(lowered(prefix + name), prefix + name);
    }
  }

  auto counts = std::map<std::string, Count>{};
  auto longest = 0.0;
  auto longestName = std::string{};
  auto failures = std::vector<std::string>{};
  for (const auto& [key, name] : names)
  {
    const auto extension = extensionOf(key);
    auto& count = counts[extension];
    const auto itemStarted = std::chrono::steady_clock::now();
    auto found = GameFileProbe{};
    const auto bytes = source.readGameFile(name, &found);
    if (!bytes)
    {
      ++count.failed;
      failures.push_back(name + ": not read");
      continue;
    }
    ++count.files;
    count.bytes += bytes->size();
    if (extension == ".bmd5mesh")
    {
      // MD5B, not BRM: never read (K41, P7.19)
    }
    else
    {
      const auto data = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(bytes->data()), bytes->size()};
      auto offset = std::size_t{0};
      auto read = false;
      readBinaryModel(data, offset) | kdl::transform([&](const auto&) { read = true; })
        | kdl::transform_error([&](const auto& e) {
            ++count.failed;
            failures.push_back(name + ": " + e.msg);
          });
      if (read && extension == ".bglb" && !readGlmbTail(data, offset).is_success())
      {
        ++count.failed;
        failures.push_back(name + ": its GLMB tail");
      }
    }
    const auto ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - itemStarted)
                      .count();
    count.milliseconds += ms;
    if (ms > longest)
    {
      longest = ms;
      longestName = name;
    }
  }
  source.closeFiles();
  const auto total =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
      .count();

  auto files = std::size_t{0};
  std::cout << fmt::format(
    "retail models: {} binaries ({} in containers), {:.0f} ms in all, the longest item "
    "{:.2f} ms ({})\n",
    names.size(),
    containerNames,
    total,
    longest,
    longestName);
  for (const auto& [extension, count] : counts)
  {
    files += count.files;
    std::cout << fmt::format(
      "  {}: {} files, {:.2f} MB, {:.0f} ms, {} failed{}\n",
      extension,
      count.files,
      double(count.bytes) / (1024.0 * 1024.0),
      count.milliseconds,
      count.failed,
      extension == ".bmd5mesh" ? " (MD5B, counted apart, not read)" : "");
  }
  for (const auto& failure : failures)
  {
    std::cout << "  failed: " << failure << "\n";
  }
  CHECK(files > 0);
  CHECK(failures.empty());
}

namespace
{

// LP7.1b: the first field in which a source-path record differs from the engine's binary
// of it, or "" when every field is equal (the goldens' comparison, tst_ModelLoading)
// LP7.5: when the two records have the same surfaces and vertex counts, the vertices
// whose fields differ, counted per field (xyz, st, normal, tangent, color, color2), and
// the surfaces whose indexes differ; "" otherwise
std::string fieldCounts(const ModelRecord& source, const ModelRecord& binary)
{
  if (source.surfaces.size() != binary.surfaces.size())
  {
    return {};
  }
  auto counts = std::array<std::size_t, 7>{};
  auto vertices = std::size_t{0};
  auto indexes = std::size_t{0};
  for (std::size_t s = 0; s < source.surfaces.size(); ++s)
  {
    const auto& a = source.surfaces[s];
    const auto& b = binary.surfaces[s];
    if (a.verts.size() != b.verts.size())
    {
      return {};
    }
    vertices += a.verts.size();
    indexes += a.indexes != b.indexes ? 1 : 0;
    for (std::size_t v = 0; v < a.verts.size(); ++v)
    {
      const auto& p = a.verts[v];
      const auto& q = b.verts[v];
      counts[0] += p.xyz != q.xyz ? 1 : 0;
      counts[1] += p.st != q.st ? 1 : 0;
      // the normal's x, y, z bytes, and its fourth, which SetNormal never writes
      // (DrawVert.h:221-246)
      counts[2] +=
        std::equal(p.normal.begin(), p.normal.begin() + 3, q.normal.begin()) ? 0 : 1;
      counts[6] += p.normal[3] != q.normal[3] ? 1 : 0;
      counts[3] += p.tangent != q.tangent ? 1 : 0;
      counts[4] += p.color != q.color ? 1 : 0;
      counts[5] += p.color2 != q.color2 ? 1 : 0;
    }
  }
  return fmt::format(
    "of {} vertices, differing: xyz {}, st {}, normal x-z {}, normal[3] {}, tangent {}, "
    "color {}, color2 {}; {} surfaces' indexes differ",
    vertices,
    counts[0],
    counts[1],
    counts[2],
    counts[6],
    counts[3],
    counts[4],
    counts[5],
    indexes);
}

std::string firstDifference(const ModelRecord& source, const ModelRecord& binary)
{
  if (source.surfaces.size() != binary.surfaces.size())
  {
    return fmt::format(
      "surface count {} != {}", source.surfaces.size(), binary.surfaces.size());
  }
  for (std::size_t s = 0; s < source.surfaces.size(); ++s)
  {
    const auto& a = source.surfaces[s];
    const auto& b = binary.surfaces[s];
    if (a.material != b.material)
    {
      return fmt::format("surface {} material {} != {}", s, a.material, b.material);
    }
    if (a.verts.size() != b.verts.size())
    {
      return fmt::format(
        "surface {} vertex count {} != {}", s, a.verts.size(), b.verts.size());
    }
    for (std::size_t v = 0; v < a.verts.size(); ++v)
    {
      const auto& p = a.verts[v];
      const auto& q = b.verts[v];
      const auto field = p.xyz != q.xyz           ? "xyz"
                         : p.st != q.st           ? "st"
                         : p.normal != q.normal   ? "normal"
                         : p.tangent != q.tangent ? "tangent"
                         : p.color != q.color     ? "color"
                         : p.color2 != q.color2   ? "color2"
                                                  : "";
      if (*field)
      {
        return fmt::format("surface {} vertex {} {}", s, v, field);
      }
    }
    if (a.indexes != b.indexes)
    {
      return fmt::format("surface {} indexes", s);
    }
    if (a.dupVerts != b.dupVerts || a.mirroredVerts != b.mirroredVerts)
    {
      return fmt::format("surface {} dupVerts or mirroredVerts", s);
    }
    if (
      a.bounds != b.bounds || a.generateNormals != b.generateNormals
      || a.tangentsCalculated != b.tangentsCalculated)
    {
      return fmt::format("surface {} bounds or flags", s);
    }
  }
  if (source.bounds != binary.bounds)
  {
    return "the model's bounds";
  }
  if (
    source.hasDrawingSurfaces != binary.hasDrawingSurfaces
    || source.hasInteractingSurfaces != binary.hasInteractingSurfaces
    || source.hasShadowCastingSurfaces != binary.hasShadowCastingSurfaces)
  {
    return "the has-surfaces flags";
  }
  return {};
}

struct SourceCount
{
  std::size_t files = 0;
  std::size_t bytes = 0;
  double milliseconds = 0.0;
  std::size_t loaded = 0;
  std::vector<std::string> refused;
  std::size_t binaryEqual = 0;
  std::vector<std::string> binaryDifferent;
  std::size_t binaryStale = 0;
  std::size_t noBinary = 0;
  // LP7.5: the records drawn as nothing (K41), and each glTF file's line
  std::size_t skinned = 0;
  std::vector<std::string> lines;
};

} // namespace

TEST_CASE(
  "Retail models: the loose sources through the source path (LP7.1b, C51)", "[.][retail]")
{
  const auto gamePath = retailGamePath();
  if (!gamePath)
  {
    SKIP("TB_LIT_GAME_PATH is not set: the retail sweep needs the game folder");
  }

  const auto started = std::chrono::steady_clock::now();
  auto source = GameFileSource{*gamePath};
  // the retail decls, for the materials the sources name (FindMaterial on the parse
  // thread's registry, as the service's)
  auto decls = MaterialDecls{source};
  static_cast<void>(decls.takeMessages());
  const auto declsMs =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
      .count();

  // every loose models/** file of a model extension (the listing recurses on Windows,
  // C8; container entries are the binary half's)
  const auto extensions = std::vector<std::string>{
    ".obj", ".ase", ".lwo", ".glb", ".gltf", ".fbx", ".ma", ".md5mesh", ".md3", ".flt"};
  auto counts = std::map<std::string, SourceCount>{};
  auto longest = 0.0;
  auto longestName = std::string{};
  // LP7.5 (C51, P7.14): the longest parse-thread item and pool job
  auto longestParse = 0.0;
  auto longestParseName = std::string{};
  auto longestFinish = 0.0;
  auto longestFinishName = std::string{};
  for (const auto& extension : extensions)
  {
    for (const auto& listed : source.listFiles("models", extension))
    {
      const auto name = "models/" + listed;
      const auto probe = source.probe(name);
      if (probe.kind != GameFileProbe::Kind::Loose)
      {
        continue;
      }
      auto& count = counts[extension];
      const auto itemStarted = std::chrono::steady_clock::now();
      auto found = GameFileProbe{};
      const auto bytes = source.readGameFile(name, &found);
      if (!bytes)
      {
        count.refused.push_back(name + ": not read");
        continue;
      }
      ++count.files;
      count.bytes += bytes->size();
      const auto canonical = canonicalModelName(name);
      const auto dispatch = modelDispatch(canonical);
      const auto gltf = extension == ".glb" || extension == ".gltf";
      auto facts = GltfFileFacts{};
      if (gltf)
      {
        static_cast<void>(gltfFileProblem(*bytes, extension == ".glb", &facts));
      }
      auto outcome = std::string{};
      auto binaryMatch = std::string{"no binary"};
      auto parseMs = 0.0;
      auto finishMs = 0.0;
      if (dispatch.path == ModelPath::Source)
      {
        const auto parseStarted = std::chrono::steady_clock::now();
        auto read = readModelSource(decls, canonical, *bytes, found.time);
        static_cast<void>(decls.takeMessages());
        parseMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - parseStarted)
                    .count();
        if (parseMs > longestParse)
        {
          longestParse = parseMs;
          longestParseName = name;
        }
        if (read.skinned)
        {
          // LP7.5 (K41): an animated glTF, nothing drawn
          ++count.skinned;
          outcome = "Skinned (K41): " + read.reason;
        }
        else if (!read.reason.empty())
        {
          // a refused retail file is a finding: reported, the validator kept
          count.refused.push_back(read.reason);
          outcome = "refused: " + read.reason;
        }
        else
        {
          REQUIRE(read.input);
          const auto finishStarted = std::chrono::steady_clock::now();
          const auto model = finishModel(*read.input);
          static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
          finishMs = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - finishStarted)
                       .count();
          if (finishMs > longestFinish)
          {
            longestFinish = finishMs;
            longestFinishName = name;
          }
          // LP7.2 (step 1): the engine's 16-bit limit, after the finish
          if (const auto problem = modelSurfaceProblem(model))
          {
            count.refused.push_back(canonical + ": " + *problem);
            outcome = "refused after the finish: " + *problem;
          }
          else
          {
            ++count.loaded;
            outcome = fmt::format(
              "loaded: {} surfaces, {} vertices after the finish{}",
              model.surfaces.size(),
              std::accumulate(
                model.surfaces.begin(),
                model.surfaces.end(),
                std::size_t{0},
                [](const std::size_t n, const EngineModelSurface& s) {
                  return n + s.verts.size();
                }),
              read.input->note.empty() ? "" : "; " + read.input->note);
            const auto record = modelRecordFromEngine(
              canonical,
              model,
              ModelRecord::State::Ready,
              ModelRecord::SourceKind::Source,
              found.time,
              {});
            // the binary-first path's own check on real data
            const auto binary = lookUpModelBinary(source, canonical, found.time);
            if (binary.result == ModelBinaryLookup::Result::Accepted && binary.model)
            {
              const auto stored =
                modelRecordFromBinary(canonical, *binary.model, binary.glmb, found.time);
              const auto difference = firstDifference(record, stored);
              if (difference.empty())
              {
                ++count.binaryEqual;
                binaryMatch = "equal to its binary";
              }
              else
              {
                const auto fields = fieldCounts(record, stored);
                count.binaryDifferent.push_back(
                  canonical + ": " + difference
                  + (fields.empty() ? "" : " (" + fields + ")"));
                binaryMatch = "differs from its binary: " + difference
                              + (fields.empty() ? "" : " (" + fields + ")");
              }
            }
            else if (binary.result == ModelBinaryLookup::Result::Stale)
            {
              ++count.binaryStale;
              binaryMatch = "its binary stale";
            }
            else
            {
              ++count.noBinary;
            }
          }
        }
      }
      const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - itemStarted)
                        .count();
      count.milliseconds += ms;
      if (gltf)
      {
        count.lines.push_back(fmt::format(
          "{} ({:.2f} MB): {}; {} nodes, {} primitives, {} vertices; {}; {:.1f} ms (the "
          "parse thread's {:.1f}, the finish {:.1f})",
          name,
          double(bytes->size()) / (1024.0 * 1024.0),
          outcome,
          facts.nodes,
          facts.primitives,
          facts.vertices,
          binaryMatch,
          ms,
          parseMs,
          finishMs));
      }
      if (ms > longest)
      {
        longest = ms;
        longestName = name;
      }
    }
  }
  source.closeFiles();
  const auto total =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
      .count();

  std::cout << fmt::format(
    "retail model sources: {:.0f} ms in all ({:.0f} ms the decls), the longest item "
    "{:.2f} ms ({}); the longest parse-thread item {:.2f} ms ({}), pool job {:.2f} ms "
    "({})\n",
    total,
    declsMs,
    longest,
    longestName,
    longestParse,
    longestParseName,
    longestFinish,
    longestFinishName);
  auto loaded = std::size_t{0};
  for (const auto& [extension, count] : counts)
  {
    const auto supported = extension == ".obj" || extension == ".ase"
                           || extension == ".lwo" || extension == ".glb"
                           || extension == ".gltf";
    std::cout << fmt::format(
      "  {}: {} files, {:.2f} MB, {:.0f} ms{}\n",
      extension,
      count.files,
      double(count.bytes) / (1024.0 * 1024.0),
      count.milliseconds,
      supported
        ? fmt::format(
            ", {} loaded, {} refused, {} skinned; binaries: {} equal, {} different, "
            "{} stale, {} none",
            count.loaded,
            count.refused.size(),
            count.skinned,
            count.binaryEqual,
            count.binaryDifferent.size(),
            count.binaryStale,
            count.noBinary)
        : std::string{" (the default cube)"});
    for (const auto& refusal : count.refused)
    {
      std::cout << "    refused: " << refusal << "\n";
    }
    for (const auto& difference : count.binaryDifferent)
    {
      std::cout << "    differs from its binary: " << difference << "\n";
    }
    for (const auto& line : count.lines)
    {
      std::cout << "    " << line << "\n";
    }
    loaded += count.loaded;
  }
  CHECK(loaded > 0);
}

} // namespace tb::lit
