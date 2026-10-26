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

#include "StackThread.h"
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
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include <catch2/catch_message.hpp>
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
  // LP5.3 step 11: crypt32's chain-engine worker (CRYPT32.dll+0x1BF50), which the OS
  // starts on its own (2 of 3 [litgpu] runs of the float/redock section)
  const auto crypt32 = GetModuleHandleW(L"crypt32.dll");
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
        thread.osWorker = module == ntdll || (crypt32 && module == crypt32);
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
    const auto had = std::ranges::any_of(baseline.threadSet, [&](const auto& old) {
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
  std::erase_if(result, [](const auto& thread) { return thread.osWorker; });
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
namespace
{

/**
 * LP7.1b step 0: the threads that carry Catch2's pair (CatchThreadBlocks). A thread_local
 * of this executable is made, like Catch2's g_messages and g_messageScopes, by the
 * executable's TLS callback when a thread starts (MSVC initialises namespace-scope
 * thread_locals there, 49-START-STOP-HEAP-FLAKE.md §2) and destroyed when the thread
 * ends; a thread that never gets the callback (the loader's workers, which live about 60
 * s, the investigation's ten-second trios) carries neither. Each live witness holds its
 * thread's id in a constinit slot: nothing is allocated, so the witness costs the heap
 * nothing.
 */
constexpr auto CarrierSlots = std::size_t{1024};
constinit std::array<std::atomic<unsigned long>, CarrierSlots> s_carriers{};
using Carriers = std::array<unsigned long, CarrierSlots>;

struct CarrierWitness
{
  CarrierWitness() noexcept
  {
    const auto id = static_cast<unsigned long>(GetCurrentThreadId());
    for (auto& slot : s_carriers)
    {
      auto expected = 0ul;
      if (slot.compare_exchange_strong(expected, id))
      {
        return;
      }
    }
  }

  ~CarrierWitness()
  {
    const auto id = static_cast<unsigned long>(GetCurrentThreadId());
    for (auto& slot : s_carriers)
    {
      auto expected = id;
      if (slot.compare_exchange_strong(expected, 0ul))
      {
        return;
      }
    }
  }

  CarrierWitness(const CarrierWitness&) = delete;
  CarrierWitness& operator=(const CarrierWitness&) = delete;

  bool alive = true;
};

thread_local CarrierWitness t_carrier;

void takeCarriers(Carriers& carriers)
{
  for (std::size_t i = 0; i < CarrierSlots; ++i)
  {
    carriers[i] = s_carriers[i].load();
  }
}

// the ids in `of` that `in` does not hold
long long carriersMissing(const Carriers& of, const Carriers& in)
{
  auto missing = 0LL;
  for (const auto id : of)
  {
    if (id != 0 && std::ranges::find(in, id) == in.end())
    {
      ++missing;
    }
  }
  return missing;
}

bool carries(const Carriers& carriers, const unsigned long id)
{
  return std::ranges::find(carriers, id) != carriers.end();
}

// a checkpoint and the carriers at it, taken again while a thread starts or ends between
// the two (nothing is allocated here: the sentinel stays the newest block)
void checkpointWithCarriers(_CrtMemState& state, Carriers& carriers)
{
  auto first = Carriers{};
  do
  {
    takeCarriers(first);
    _CrtMemCheckpoint(&state);
    takeCarriers(carriers);
  } while (first != carriers);
}

struct SentinelFree
{
  void operator()(void* block) const { _free_dbg(block, _NORMAL_BLOCK); }
};

void report(const char* text)
{
  _CrtDbgReport(_CRT_WARN, nullptr, 0, nullptr, "%s", text);
}

std::string describeWithCarriers(
  const std::vector<ProcessThread>& threads, const Carriers& carriers)
{
  if (threads.empty())
  {
    return "none";
  }
  auto text = std::string{};
  for (const auto& thread : threads)
  {
    text += fmt::format(
      "{}{} (id {}{})",
      text.empty() ? "" : ", ",
      thread.where,
      thread.id,
      carries(carriers, thread.id) ? ", Catch2's pair" : "");
  }
  return text;
}

} // namespace

HeapMeasure measureHeap(const std::function<void()>& f, const bool dump)
{
  // this thread's witness, made at its start
  static_cast<void>(t_carrier.alive);
  // allocated before the sentinel: older than the checkpoint, freed after the second one
  const auto threadsBefore = processCounts();
  auto carriersBefore = Carriers{};
  auto carriersAfter = Carriers{};

  const auto sentinel = std::unique_ptr<void, SentinelFree>{
    _malloc_dbg(1, _NORMAL_BLOCK, __FILE__, __LINE__)};
  _CrtMemState before;
  checkpointWithCarriers(before, carriersBefore);
  const auto libraryBefore = liveLibraryThreads();
  f();
  const auto libraryAfter = liveLibraryThreads();
  _CrtMemState after;
  checkpointWithCarriers(after, carriersAfter);

  const auto measure = HeapMeasure{
    static_cast<long long>(after.lCounts[_NORMAL_BLOCK])
      - static_cast<long long>(before.lCounts[_NORMAL_BLOCK]),
    static_cast<long long>(after.lSizes[_NORMAL_BLOCK])
      - static_cast<long long>(before.lSizes[_NORMAL_BLOCK]),
    static_cast<long long>(after.lSizes[_CLIENT_BLOCK])
      - static_cast<long long>(before.lSizes[_CLIENT_BLOCK]),
    carriersMissing(carriersAfter, carriersBefore),
    carriersMissing(carriersBefore, carriersAfter),
    libraryBefore,
    libraryAfter};

  const auto dumped = dump && (measure.grew() || measure.threadLeak());
  auto mode = 0;
  auto file = _HFILE{};
  if (dumped)
  {
    mode = _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    file = _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    // the measure, the statistics and the blocks since the checkpoint first, before
    // anything is allocated after it (no heap here: the walk lists only the window's)
    auto sentinelRequest = 0L;
    _CrtIsMemoryBlock(sentinel.get(), 1, &sentinelRequest, nullptr, nullptr);
    auto header = std::array<char, 512>{};
    std::snprintf(
      header.data(),
      header.size(),
      "heap measure: %lld normal blocks, %lld bytes, %lld client bytes since the "
      "checkpoint (the sentinel {%ld}: the walk stops there); threads with Catch2's "
      "pair born %lld, gone %lld (theirs: %lld blocks, %lld bytes); library threads "
      "%lld before, %lld after\n",
      measure.blocks,
      measure.bytes,
      measure.clientBytes,
      sentinelRequest,
      measure.threadsBorn,
      measure.threadsGone,
      CatchThreadBlocks * measure.threads(),
      CatchThreadBytes * measure.threads(),
      static_cast<long long>(libraryBefore),
      static_cast<long long>(libraryAfter));
    report(header.data());
    _CrtMemState difference;
    _CrtMemDifference(&difference, &before, &after);
    _CrtMemDumpStatistics(&difference);
    _CrtMemDumpAllObjectsSince(&before);
  }

  const auto threadsAfter = processCounts();
  if (dumped)
  {
    const auto text = fmt::format(
      "threads born: {}\nthreads gone: {}\n",
      describeWithCarriers(newThreads(threadsBefore, threadsAfter), carriersAfter),
      describeWithCarriers(newThreads(threadsAfter, threadsBefore), carriersBefore));
    report(text.c_str());
    _CrtSetReportFile(_CRT_WARN, file);
    _CrtSetReportMode(_CRT_WARN, mode);
  }
  return measure;
}

bool heapGrows(const std::function<void()>& f)
{
  const auto measure = measureHeap(f, true);
  if (measure.grew())
  {
    UNSCOPED_INFO(fmt::format(
      "the heap grew: {} normal blocks, {} bytes, {} client bytes, where the threads "
      "born ({}) and gone ({}) account for {} blocks, {} bytes",
      measure.blocks,
      measure.bytes,
      measure.clientBytes,
      measure.threadsBorn,
      measure.threadsGone,
      CatchThreadBlocks * measure.threads(),
      CatchThreadBytes * measure.threads()));
  }
  if (measure.threadLeak())
  {
    UNSCOPED_INFO(fmt::format(
      "a thread leak: {} library thread(s) before the window, {} after it",
      measure.libraryThreadsBefore,
      measure.libraryThreadsAfter));
  }
  return measure.grew() || measure.threadLeak();
}

namespace
{

std::atomic<int> s_dumpedBlocks{0};
// the report's text, its first DumpTextBytes kept (a constinit buffer: no heap)
constexpr auto DumpTextBytes = std::size_t{64} * 1024;
constinit std::array<char, DumpTextBytes> s_dumpText{};
std::atomic<std::size_t> s_dumpTextSize{0};

int countDumpedBlocks(const int reportType, char* message, int* returnValue)
{
  if (reportType != _CRT_WARN)
  {
    return FALSE;
  }
  if (message != nullptr)
  {
    if (std::strstr(message, "normal block at") != nullptr)
    {
      ++s_dumpedBlocks;
    }
    const auto length = std::strlen(message);
    const auto at = s_dumpTextSize.fetch_add(length);
    if (at < DumpTextBytes)
    {
      std::memcpy(s_dumpText.data() + at, message, std::min(length, DumpTextBytes - at));
    }
  }
  if (returnValue != nullptr)
  {
    *returnValue = 0;
  }
  return TRUE;
}

} // namespace

DumpCounter::DumpCounter()
{
  s_dumpedBlocks = 0;
  s_dumpTextSize = 0;
  _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &countDumpedBlocks);
}

DumpCounter::~DumpCounter()
{
  _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, &countDumpedBlocks);
}

int DumpCounter::blocks() const
{
  return s_dumpedBlocks.load();
}

std::string DumpCounter::text() const
{
  return {s_dumpText.data(), std::min(s_dumpTextSize.load(), DumpTextBytes)};
}

std::size_t carrierThreadsForTesting()
{
  static_cast<void>(t_carrier.alive);
  auto carriers = Carriers{};
  takeCarriers(carriers);
  return std::size_t(
    std::ranges::count_if(carriers, [](const auto id) { return id != 0; }));
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

std::string minimalBinaryModel(
  const std::int64_t timeStamp, const std::string& material, const std::string& name)
{
  auto out = std::string{};
  const auto big = [&](const std::uint64_t value, const int bytes) {
    for (auto i = bytes - 1; i >= 0; --i)
    {
      out.push_back(char((value >> (8 * i)) & 0xFF));
    }
  };
  const auto little = [&](const void* data, const std::size_t size) {
    out.append(static_cast<const char*>(data), size);
  };
  const auto text = [&](const std::string& value) {
    const auto length = std::int32_t(value.size());
    little(&length, 4);
    out += value;
  };
  const auto vec3 = [&](const float x, const float y, const float z) {
    const float v[3] = {x, y, z};
    little(v, 12);
  };
  // 'B' 'R' 'M' 110, the time, one surface
  big(
    (std::uint64_t('B') << 24) | (std::uint64_t('R') << 16) | (std::uint64_t('M') << 8)
      | 110,
    4);
  big(std::uint64_t(timeStamp), 8);
  big(1, 4);
  big(0, 4);
  text(material);
  out.push_back(1);
  vec3(0, 0, 0);
  vec3(8, 8, 0);
  // generateNormals, tangentsCalculated, perfectHull, referencedIndexes
  out.push_back(0);
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  big(3, 4);
  big(3, 4);
  for (const auto& p : {std::array<float, 3>{0, 0, 0}, {8, 0, 0}, {8, 8, 0}})
  {
    vec3(p[0], p[1], p[2]);
    big(0, 2);
    big(0, 2);
    const unsigned char normal[4] = {128, 128, 255, 0};
    const unsigned char tangent[4] = {255, 128, 128, 255};
    const unsigned char color[4] = {255, 255, 255, 255};
    const unsigned char color2[4] = {0, 0, 0, 0};
    little(normal, 4);
    little(tangent, 4);
    little(color, 4);
    little(color2, 4);
  }
  big(3, 4);
  big(0, 2);
  big(1, 2);
  big(2, 2);
  // no sil indexes, mirrored or dup vertices, dominant tris, masked occlusion data
  big(0, 4);
  big(0, 4);
  big(0, 4);
  out.push_back(0);
  big(0, 4);
  big(0, 4);
  // the model's bounds, the frame counters, the name, the nine bools
  vec3(0, 0, 0);
  vec3(8, 8, 0);
  big(0, 4);
  big(0, 4);
  big(0, 4);
  text(name);
  for (const auto b : {false, false, false, false, true, false, true, true, true})
  {
    out.push_back(b ? 1 : 0);
  }
  return out;
}

} // namespace tb::lit
