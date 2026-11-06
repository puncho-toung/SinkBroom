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

#include <filesystem>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace tb::lit
{

/**
 * F1.13, F2.14 (2026-11-05): a test's own path in the temporary folder, its name with the
 * process id before its extension ("TbLitLibTest-frame-log-1234.csv"), so that two test
 * processes at once (Debug beside Release, a reviewer's copy beside the gate) never
 * delete or append to each other's files. Header-only: TbLitLibTest and TbUiLibTest's lit
 * tests both use it; a test removes recursively only paths this made.
 */
inline std::filesystem::path testTempPath(const std::string_view name)
{
#if defined(_WIN32)
  const auto pid = static_cast<long long>(_getpid());
#else
  const auto pid = static_cast<long long>(getpid());
#endif
  const auto path = std::filesystem::path{std::string{name}};
  auto file = path.stem().string() + "-" + std::to_string(pid);
  file += path.extension().string();
  return std::filesystem::temp_directory_path() / file;
}

} // namespace tb::lit
