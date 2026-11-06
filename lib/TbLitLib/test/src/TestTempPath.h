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

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tb::lit
{
namespace detail
{

/**
 * V-F.10 (2026-11-06): the paths testTempPath made in this process, removed (files and
 * folders, recursively) when the process ends normally; and the names whose leftovers of
 * other runs were swept already
 */
struct TestTempPaths
{
  std::mutex mutex;
  std::set<std::filesystem::path> made;
  std::set<std::string> swept;

  TestTempPaths() = default;
  TestTempPaths(const TestTempPaths&) = delete;
  TestTempPaths& operator=(const TestTempPaths&) = delete;

  ~TestTempPaths()
  {
    for (const auto& path : made)
    {
      auto ec = std::error_code{};
      std::filesystem::remove_all(path, ec);
    }
  }
};

inline TestTempPaths& testTempPaths()
{
  static auto paths = TestTempPaths{};
  return paths;
}

/**
 * V-F.10: a name's leftovers of other runs - another process id in testTempPath's own
 * pattern ("<stem>-<digits><extension>") - whose last change is more than a day old, in
 * folder: a run that ended abnormally kept them. Nothing else is touched.
 */
inline void sweepTestTempLeftovers(
  const std::filesystem::path& folder,
  const std::string& stem,
  const std::string& extension,
  const std::string& ownPid)
{
  const auto prefix = stem + "-";
  const auto now = std::filesystem::file_time_type::clock::now();
  auto ec = std::error_code{};
  for (auto it = std::filesystem::directory_iterator{folder, ec};
       !ec && it != std::filesystem::directory_iterator{};
       it.increment(ec))
  {
    const auto name = it->path().filename().string();
    if (
      name.size() <= prefix.size() + extension.size() || !name.starts_with(prefix)
      || !name.ends_with(extension))
    {
      continue;
    }
    const auto pid =
      name.substr(prefix.size(), name.size() - prefix.size() - extension.size());
    if (pid == ownPid || !std::ranges::all_of(pid, [](const char c) {
          return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }))
    {
      continue;
    }
    auto entryError = std::error_code{};
    const auto time = it->last_write_time(entryError);
    if (!entryError && now - time > std::chrono::hours{24})
    {
      std::filesystem::remove_all(it->path(), entryError);
    }
  }
}

} // namespace detail

/**
 * F1.13, F2.14 (2026-11-05): a test's own path in the temporary folder, its name with the
 * process id before its extension ("TbLitLibTest-frame-log-1234.csv"), so that two test
 * processes at once (Debug beside Release, a reviewer's copy beside the gate) never
 * delete or append to each other's files. Header-only: TbLitLibTest and TbUiLibTest's lit
 * tests both use it; a test removes recursively only paths this made. V-F.10
 * (2026-11-06): every path made is removed when the process ends normally, and at a
 * name's first use the leftovers of that name from other runs older than a day are
 * removed (its own pattern only; never a younger one, which another process may be
 * using).
 */
inline std::filesystem::path testTempPath(const std::string_view name)
{
#if defined(_WIN32)
  const auto pid = std::to_string(static_cast<long long>(_getpid()));
#else
  const auto pid = std::to_string(static_cast<long long>(getpid()));
#endif
  const auto path = std::filesystem::path{std::string{name}};
  const auto stem = path.stem().string();
  const auto extension = path.extension().string();
  const auto folder = std::filesystem::temp_directory_path();
  const auto made = folder / (stem + "-" + pid + extension);
  auto& paths = detail::testTempPaths();
  const auto lock = std::scoped_lock{paths.mutex};
  if (paths.swept.insert(stem + extension).second)
  {
    detail::sweepTestTempLeftovers(folder, stem, extension, pid);
  }
  paths.made.insert(made);
  return made;
}

} // namespace tb::lit
