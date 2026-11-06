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

#include "../../../TbLitLib/test/src/TestTempPath.h"
#include "ui/LitPreviewPanel.h"
#include "ui/SystemPaths.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

namespace tb::ui
{
namespace
{

/**
 * F1.17 (2026-11-05, DQ-18 (i)): "nothing written under the user-data folder" as a
 * mechanism, not a habit: before any test, every lit preview panel of the process takes a
 * container-index folder of the run's own
 * (LitPreviewPanel::setDefaultIndexFolderForTesting; the three tests that set a folder of
 * their own keep it), and the user-data folder's lit-preview folder is listed by metadata
 * alone (each entry's path, size and change time; never a byte of the index); at the
 * run's end it is listed again, and a change fails the run (its exit code 3, both
 * listings printed).
 */
struct Entry
{
  std::string path;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type time;

  auto operator<=>(const Entry&) const = default;
};

std::filesystem::path litUserData()
{
  return SystemPaths::userDataDirectory() / "lit-preview";
}

std::vector<Entry> listing(const std::filesystem::path& folder)
{
  auto entries = std::vector<Entry>{};
  auto ec = std::error_code{};
  for (auto it = std::filesystem::recursive_directory_iterator{folder, ec};
       !ec && it != std::filesystem::recursive_directory_iterator{};
       it.increment(ec))
  {
    auto entryError = std::error_code{};
    const auto size = it->is_regular_file(entryError) ? it->file_size(entryError) : 0;
    const auto time = it->last_write_time(entryError);
    entries.push_back(Entry{
      std::filesystem::relative(it->path(), folder, entryError).generic_string(),
      size,
      time});
  }
  std::ranges::sort(entries);
  return entries;
}

void print(const std::string& label, const std::vector<Entry>& entries)
{
  std::cerr << label << " (" << entries.size() << " entries):\n";
  for (const auto& entry : entries)
  {
    std::cerr << "  " << entry.path << ", " << entry.size << " bytes, "
              << std::chrono::duration_cast<std::chrono::seconds>(
                   entry.time.time_since_epoch())
                   .count()
              << "\n";
  }
}

class LitUserDataListener : public Catch::EventListenerBase
{
private:
  std::vector<Entry> m_before;
  std::filesystem::path m_defaultFolder;

public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(const Catch::TestRunInfo&) override
  {
    m_defaultFolder = lit::testTempPath("TbUiLibTest-lit-default-index");
    auto ec = std::error_code{};
    std::filesystem::remove_all(m_defaultFolder, ec);
    LitPreviewPanel::setDefaultIndexFolderForTesting(m_defaultFolder);
    m_before = listing(litUserData());
  }

  void testRunEnded(const Catch::TestRunStats&) override
  {
    LitPreviewPanel::setDefaultIndexFolderForTesting(std::nullopt);
    auto ec = std::error_code{};
    std::filesystem::remove_all(m_defaultFolder, ec);
    const auto after = listing(litUserData());
    if (after != m_before)
    {
      std::cerr
        << "F1.17: the run wrote under the user-data folder's lit-preview folder ("
        << litUserData().string() << ")\n";
      print("before", m_before);
      print("after", after);
      std::cerr.flush();
      std::cout.flush();
      std::exit(3);
    }
  }
};

} // namespace

CATCH_REGISTER_LISTENER(LitUserDataListener)

} // namespace tb::ui
