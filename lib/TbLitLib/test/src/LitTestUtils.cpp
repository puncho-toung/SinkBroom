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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// after windows.h
#include <tlhelp32.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#endif

#include <fmt/format.h>
#include <miniz.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <sstream>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

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

bool setUnixModificationTime(
  const std::filesystem::path& path, const std::int64_t seconds)
{
#if defined(_WIN32)
  // Sys_FileTimeStamp's conversion, backwards
  const auto ticks = std::uint64_t(seconds * 10000000 + 116444736000000000);
  const auto time = FILETIME{DWORD(ticks & 0xFFFFFFFFu), DWORD(ticks >> 32)};
  const auto handle = CreateFileW(
    path.c_str(),
    FILE_WRITE_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return false;
  }
  const auto set = SetFileTime(handle, nullptr, nullptr, &time);
  CloseHandle(handle);
  return set != 0;
#else
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = time_t(seconds);
  return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
#endif
}

ProcessCounts processCounts()
{
  auto counts = ProcessCounts{};
#if defined(_WIN32)
  // the handles first: the thread handles opened below are closed again, but not before
  // the count
  auto handles = DWORD{0};
  GetProcessHandleCount(GetCurrentProcess(), &handles);
  counts.handles = long(handles);

  using QueryThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
  const auto ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto query = ntdll ? reinterpret_cast<QueryThread>(reinterpret_cast<void*>(
                               GetProcAddress(ntdll, "NtQueryInformationThread")))
                           : nullptr;
  constexpr auto ThreadQuerySetWin32StartAddress = 9;

  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE)
  {
    auto entry = THREADENTRY32{};
    entry.dwSize = sizeof(entry);
    const auto processId = GetCurrentProcessId();
    for (auto found = Thread32First(snapshot, &entry); found;
         found = Thread32Next(snapshot, &entry))
    {
      if (entry.th32OwnerProcessID != processId)
      {
        continue;
      }
      ++counts.threads;
      auto thread = ProcessThread{entry.th32ThreadID, 0, "?", false};
      if (const auto handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.id))
      {
        auto start = static_cast<void*>(nullptr);
        if (
          query
          && query(
               handle,
               ThreadQuerySetWin32StartAddress,
               &start,
               ULONG(sizeof(start)),
               nullptr)
               >= 0)
        {
          thread.start = reinterpret_cast<std::uintptr_t>(start);
        }
        CloseHandle(handle);
      }
      auto module = HMODULE{nullptr};
      if (
        thread.start != 0
        && GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(thread.start),
          &module))
      {
        auto name = std::array<wchar_t, MAX_PATH>{};
        GetModuleFileNameW(module, name.data(), DWORD(name.size()));
        thread.where = fmt::format(
          "{}+0x{:X}",
          std::filesystem::path{name.data()}.filename().string(),
          thread.start - reinterpret_cast<std::uintptr_t>(module));
        thread.inNtdll = module == ntdll;
      }
      else if (thread.start != 0)
      {
        thread.where = fmt::format("0x{:X}", thread.start);
      }
      counts.threadSet.push_back(std::move(thread));
    }
    CloseHandle(snapshot);
  }
#endif
  return counts;
}

std::vector<ProcessThread> newThreads(
  const ProcessCounts& baseline, const ProcessCounts& now)
{
  auto result = std::vector<ProcessThread>{};
  for (const auto& thread : now.threadSet)
  {
    const auto had = std::any_of(
      baseline.threadSet.begin(), baseline.threadSet.end(), [&](const auto& old) {
        return old.id == thread.id && old.start == thread.start;
      });
    if (!had)
    {
      result.push_back(thread);
    }
  }
  return result;
}

std::vector<ProcessThread> newForeignThreads(
  const ProcessCounts& baseline, const ProcessCounts& now)
{
  auto result = newThreads(baseline, now);
  std::erase_if(result, [](const auto& thread) { return thread.inNtdll; });
  return result;
}

std::string describeThreads(const std::vector<ProcessThread>& threads)
{
  if (threads.empty())
  {
    return "none";
  }
  auto text = std::string{};
  for (const auto& thread : threads)
  {
    text +=
      fmt::format("{}{} (id {})", text.empty() ? "" : ", ", thread.where, thread.id);
  }
  return text;
}

#if defined(_MSC_VER) && defined(_DEBUG)
bool heapGrows(const std::function<void()>& f)
{
  _CrtMemState before;
  _CrtMemCheckpoint(&before);
  f();
  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  _CrtMemState difference;
  const auto grew = _CrtMemDifference(&difference, &before, &after) != 0;
  if (grew)
  {
    // the statistics and the blocks still allocated since, on stderr
    const auto mode = _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    const auto file = _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtMemDumpStatistics(&difference);
    _CrtMemDumpAllObjectsSince(&before);
    _CrtSetReportFile(_CRT_WARN, file);
    _CrtSetReportMode(_CRT_WARN, mode);
  }
  return grew;
}

HeapGrowth heapGrowth(const std::function<void()>& f)
{
  _CrtMemState before;
  _CrtMemCheckpoint(&before);
  f();
  _CrtMemState after;
  _CrtMemCheckpoint(&after);
  return HeapGrowth{
    static_cast<long long>(after.lCounts[_NORMAL_BLOCK])
      - static_cast<long long>(before.lCounts[_NORMAL_BLOCK]),
    static_cast<long long>(after.lSizes[_NORMAL_BLOCK])
      - static_cast<long long>(before.lSizes[_NORMAL_BLOCK])};
}
#endif

} // namespace tb::lit
