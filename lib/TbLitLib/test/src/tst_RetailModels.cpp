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
// .bmd5mesh is MD5B, another format, counted apart (P7.19). The source half (the loose
// base/models/** through the validators and the copied loaders) is LP7.1b's.

#include "lit/BprocModel.h"
#include "lit/GameFileSource.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
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

} // namespace tb::lit
