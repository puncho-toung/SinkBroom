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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

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

// sets a file's modification time to whole Unix seconds, as the engine reads them (A4):
// SetFileTime on Windows, utimensat elsewhere; false when it cannot
bool setUnixModificationTime(const std::filesystem::path& path, std::int64_t seconds);

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

/**
 * One thread of this process: its id and its Win32 start address
 * (NtQueryInformationThread class 9, ThreadQuerySetWin32StartAddress), resolved against
 * the loaded modules as "module+0xOFFSET" ("?" when it cannot be read).
 */
struct ProcessThread
{
  unsigned long id = 0;
  std::uintptr_t start = 0;
  std::string where;
  // its start inside an OS worker's module: ntdll (the thread pool) or crypt32
  bool osWorker = false;
};

/**
 * The process's OS handles and threads (U2.12's measure). LP5.3 step 0
 * (44-FEED-THREAD-FLAKE.md): the thread count alone cannot tell a leak from the OS's own
 * thread-pool workers, which a session's create or teardown may add (start inside ntdll,
 * ntdll.dll+0x52B20 on the investigator's machine; LP5.3 step 11: crypt32's chain-engine
 * worker too) and which live until the process exits; so the set is recorded with each
 * thread's start and the checks compare sets. Windows only; elsewhere every field is
 * empty.
 */
struct ProcessCounts
{
  long threads = 0;
  long handles = 0;
  std::vector<ProcessThread> threadSet;
};

ProcessCounts processCounts();

// the threads of now that baseline did not have (the same id and start counts as had)
std::vector<ProcessThread> newThreads(
  const ProcessCounts& baseline, const ProcessCounts& now);
// those of newThreads whose start is not an OS worker's: what a count check fails on
std::vector<ProcessThread> newForeignThreads(
  const ProcessCounts& baseline, const ProcessCounts& now);
// "none", or each thread as "where (id N)", comma-separated
std::string describeThreads(const std::vector<ProcessThread>& threads);

#if defined(_MSC_VER) && defined(_DEBUG)
// runs f between two checkpoints of the CRT's debug heap; true when the heap grew, with
// its statistics dumped
bool heapGrows(const std::function<void()>& f);

// the normal blocks and bytes the heap holds more after f than before it (negative for
// fewer), for a check whose subject keeps a state of a few blocks that varies (a polling
// thread's listings): a leak in a loop shows once per pass
struct HeapGrowth
{
  long long blocks = 0;
  long long bytes = 0;
};
HeapGrowth heapGrowth(const std::function<void()>& f);
#endif

} // namespace tb::lit
