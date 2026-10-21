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

#include "TestEnvironment.h"

#include <fmt/format.h>
#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

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

void writeBig32(std::string& out, const std::size_t at, const std::uint32_t value)
{
  out[at] = char((value >> 24) & 0xFF);
  out[at + 1] = char((value >> 16) & 0xFF);
  out[at + 2] = char((value >> 8) & 0xFF);
  out[at + 3] = char(value & 0xFF);
}

} // namespace

TempFolder::TempFolder(const std::string& prefix)
{
  static auto counter = std::atomic<int>{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  m_path = std::filesystem::temp_directory_path()
           / fmt::format("{}-{}-{}", prefix, stamp, counter++);
  std::filesystem::create_directories(m_path);
}

TempFolder::~TempFolder()
{
  auto ec = std::error_code{};
  std::filesystem::remove_all(m_path, ec);
}

std::string readTextFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto text = std::stringstream{};
  text << stream.rdbuf();
  return text.str();
}

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

std::string materialText(const std::string& name, const std::string& image)
{
  return fmt::format("{}\n{{\n\tdiffusemap\t{}\n}}\n", name, image);
}

std::string resourcesBytes(const std::vector<std::pair<std::string, std::string>>& files)
{
  auto out = std::string(12, '\0');
  auto offsets = std::vector<std::uint32_t>{};
  for (const auto& [name, data] : files)
  {
    offsets.push_back(std::uint32_t(out.size()));
    out += data;
  }

  const auto tableOffset = std::uint32_t(out.size());
  appendBig32(out, std::uint32_t(files.size()));
  for (std::size_t i = 0; i < files.size(); ++i)
  {
    appendLittle32(out, std::uint32_t(files[i].first.size()));
    out += files[i].first;
    appendBig32(out, offsets[i]);
    appendBig32(out, std::uint32_t(files[i].second.size()));
  }
  const auto tableLength = std::uint32_t(out.size()) - tableOffset;

  writeBig32(out, 0, 0xD000000Du);
  writeBig32(out, 4, tableOffset);
  writeBig32(out, 8, tableLength);
  return out;
}

void writePk4(
  const std::filesystem::path& path,
  const std::vector<std::pair<std::string, std::string>>& files)
{
  std::filesystem::create_directories(path.parent_path());
  auto zip = mz_zip_archive{};
  mz_zip_zero_struct(&zip);
  const auto pathText = path.string();
  REQUIRE(mz_zip_writer_init_file(&zip, pathText.c_str(), 0));
  for (const auto& [name, data] : files)
  {
    REQUIRE(mz_zip_writer_add_mem(
      &zip, name.c_str(), data.data(), data.size(), mz_uint(MZ_DEFAULT_LEVEL)));
  }
  REQUIRE(mz_zip_writer_finalize_archive(&zip));
  REQUIRE(mz_zip_writer_end(&zip));
}

std::vector<DeclFile> materialDeclsFixture()
{
  const auto root = getFixtureRoot() / "test" / "lit" / "MaterialDecls";
  auto files = std::vector<DeclFile>{};
  for (const auto& entry : std::filesystem::recursive_directory_iterator{root})
  {
    if (entry.is_regular_file())
    {
      files.push_back(DeclFile{
        std::filesystem::relative(entry.path(), root).generic_string(),
        readTextFile(entry.path()),
        0});
    }
  }
  return files;
}

std::vector<std::string> sortedEnginePaths(std::vector<std::string> paths)
{
  std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
    return compareEnginePaths(lhs, rhs) < 0;
  });
  return paths;
}

void bumpModificationTime(const std::filesystem::path& path)
{
  // a write within the file system's time resolution may keep the old time
  const auto time = std::filesystem::last_write_time(path);
  std::filesystem::last_write_time(path, time + std::chrono::seconds{2});
}

} // namespace tb::lit
