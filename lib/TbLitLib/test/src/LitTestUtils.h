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

#include "lit/MaterialDecls.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

// a folder under the system's temporary folder, removed with this object
class TempFolder
{
private:
  std::filesystem::path m_path;

public:
  explicit TempFolder(const std::string& prefix);
  ~TempFolder();

  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;

  const std::filesystem::path& path() const { return m_path; }
};

std::string readTextFile(const std::filesystem::path& path);
void writeTextFile(const std::filesystem::path& path, const std::string& text);

// a material decl with one diffuse stage of this image
std::string materialText(const std::string& name, const std::string& image);

// a .resources container in the engine's format (idResourceContainer::WriteResourceFile):
// the data, then the table, then the header written over the first 12 bytes
std::string resourcesBytes(const std::vector<std::pair<std::string, std::string>>& files);

// a .pk4 (a zip) with these entries, written with miniz
void writePk4(
  const std::filesystem::path& path,
  const std::vector<std::pair<std::string, std::string>>& files);

// every file of the MaterialDecls fixture, with paths relative to the fixture folder
std::vector<DeclFile> materialDeclsFixture();

// sorted with the engine's path order (idSort_PathStr)
std::vector<std::string> sortedEnginePaths(std::vector<std::string> paths);

// the time after a write that a modification time can tell apart from the one before it
void bumpModificationTime(const std::filesystem::path& path);

/**
 * Holds a material service's parse thread before it takes its next request while closed
 * (MaterialServiceOptions::beforeTakeWorkForTesting calls pass): lookups queued then stay
 * pending until it opens. A thread already inside its wait for a request takes one more.
 */
struct ParseGate
{
  std::mutex mutex;
  std::condition_variable changed;
  bool closed = false;

  void pass()
  {
    auto lock = std::unique_lock{mutex};
    changed.wait(lock, [&] { return !closed; });
  }

  void close()
  {
    const auto lock = std::scoped_lock{mutex};
    closed = true;
  }

  void open()
  {
    {
      const auto lock = std::scoped_lock{mutex};
      closed = false;
    }
    changed.notify_all();
  }
};

// opens the gate when the test leaves, so that the service's join never waits for it
struct OpenAtExit
{
  std::shared_ptr<ParseGate> gate;
  ~OpenAtExit() { gate->open(); }
};

} // namespace tb::lit
