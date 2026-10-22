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

// A1.11: GameFile, statGameFile, listOsFiles and the path helpers, tested directly

#include "LitTestUtils.h"
#include "fs/TestEnvironment.h"
#include "lit/GameFile.h"

#include "kd/result.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace tb::lit
{
namespace
{

std::vector<std::string> sorted(std::vector<std::string> names)
{
  std::sort(names.begin(), names.end());
  return names;
}

#if defined(_WIN32)

// FILETIME's 100-ns ticks of a Unix time plus a fraction of a second
FILETIME fileTimeOf(const std::int64_t unixSeconds, const std::int64_t extraTicks)
{
  const auto ticks =
    std::uint64_t(unixSeconds * 10000000 + 116444736000000000 + extraTicks);
  return FILETIME{DWORD(ticks & 0xFFFFFFFFu), DWORD(ticks >> 32)};
}

bool setWriteTime(const std::filesystem::path& path, const FILETIME& time)
{
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
}

/**
 * A directory junction at link to target (both in the test's folder), as mklink /J makes
 * one: a mount-point reparse point, which needs no privilege. The layout is the SDK's
 * REPARSE_DATA_BUFFER (ntifs.h), whose user-mode headers do not declare it.
 */
bool createJunction(
  const std::filesystem::path& link, const std::filesystem::path& target)
{
  std::filesystem::create_directory(link);
  const auto handle = CreateFileW(
    link.c_str(),
    GENERIC_WRITE,
    0,
    nullptr,
    OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return false;
  }

  const auto print = std::filesystem::absolute(target).wstring();
  const auto substitute = L"\\??\\" + print;
  const auto nameBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  // tag, data length and reserved; four name offsets and lengths; the names
  auto buffer = std::vector<std::uint8_t>(8 + 8 + nameBytes, 0);
  const auto put16 = [&](const std::size_t at, const std::size_t value) {
    const auto v = std::uint16_t(value);
    std::memcpy(buffer.data() + at, &v, 2);
  };
  const auto tag = DWORD{IO_REPARSE_TAG_MOUNT_POINT};
  std::memcpy(buffer.data(), &tag, 4);
  put16(4, 8 + nameBytes);
  put16(8, 0);
  put16(10, substitute.size() * sizeof(wchar_t));
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));
  put16(14, print.size() * sizeof(wchar_t));
  std::memcpy(
    buffer.data() + 16, substitute.c_str(), substitute.size() * sizeof(wchar_t));
  std::memcpy(
    buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t),
    print.c_str(),
    print.size() * sizeof(wchar_t));

  auto returned = DWORD{0};
  const auto made = DeviceIoControl(
    handle,
    FSCTL_SET_REPARSE_POINT,
    buffer.data(),
    DWORD(buffer.size()),
    nullptr,
    0,
    &returned,
    nullptr);
  CloseHandle(handle);
  return made != 0;
}

#endif

} // namespace

TEST_CASE("GameFile")
{
  const auto env = fs::TestEnvironment{};
  const auto path = env.dir() / "file.bin";
  writeTextFile(path, "0123456789");

  SECTION("opens a file, knows its size and reads within it")
  {
    auto file = GameFile::open(path) | kdl::value();
    CHECK(file.size() == 10u);

    auto bytes = std::string(3, '\0');
    CHECK(file.readAt(2, bytes.data(), bytes.size()).is_success());
    CHECK(bytes == "234");
    CHECK(file.readAt(10, bytes.data(), 0).is_success());
    // never past the size it was opened with
    CHECK(file.readAt(8, bytes.data(), 3).is_error());
    CHECK(file.readAt(11, bytes.data(), 0).is_error());

    auto moved = std::move(file);
    CHECK(moved.size() == 10u);
    CHECK(moved.readAt(0, bytes.data(), 3).is_success());
    CHECK(bytes == "012");
  }

  SECTION("a missing file or a directory cannot be opened")
  {
    CHECK(GameFile::open(env.dir() / "none.bin").is_error());
    CHECK(GameFile::open(env.dir()).is_error());
  }

  SECTION("an open file may be replaced or deleted (R4-3.26)")
  {
    auto file = GameFile::open(path) | kdl::value();
    auto ec = std::error_code{};
    std::filesystem::rename(path, env.dir() / "renamed.bin", ec);
    CHECK_FALSE(ec);
    std::filesystem::remove(env.dir() / "renamed.bin", ec);
    CHECK_FALSE(ec);
  }

  SECTION("readGameFile reads the whole file")
  {
    CHECK((readGameFile(path) | kdl::value()) == "0123456789");
    CHECK(readGameFile(env.dir() / "none.bin").is_error());
  }
}

TEST_CASE("statGameFile")
{
  const auto env = fs::TestEnvironment{};
  const auto path = env.dir() / "file.bin";
  writeTextFile(path, "0123456789");

  SECTION("a file's size, change stamp and Unix seconds (A4, P.15)")
  {
#if defined(_WIN32)
    // 2023-11-14 22:13:20.9 UTC: the engine's whole seconds drop the 0.9 s
    REQUIRE(setWriteTime(path, fileTimeOf(1700000000, 9000000)));
    const auto stat = statGameFile(path);
    REQUIRE(stat);
    CHECK(stat->size == 10u);
    CHECK(stat->unixSeconds == 1700000000);
    CHECK(stat->time == 1700000000ll * 10000000 + 116444736000000000 + 9000000);

    // a second later in ticks is a second later in Unix seconds, a tick earlier is not
    REQUIRE(setWriteTime(path, fileTimeOf(1700000001, 0)));
    CHECK(statGameFile(path)->unixSeconds == 1700000001);
    REQUIRE(setWriteTime(path, fileTimeOf(1700000001, -1)));
    CHECK(statGameFile(path)->unixSeconds == 1700000000);
#else
    // P.15: st_mtime as the engine's Sys_FileTimeStamp reads it
    struct stat status{};
    REQUIRE(::stat(path.c_str(), &status) == 0);
    const auto stat = statGameFile(path);
    REQUIRE(stat);
    CHECK(stat->size == 10u);
    CHECK(stat->unixSeconds == std::int64_t(status.st_mtime));
#endif
  }

  SECTION("a directory or a missing file has none")
  {
    CHECK_FALSE(statGameFile(env.dir()));
    CHECK_FALSE(statGameFile(env.dir() / "none.bin"));
  }
}

TEST_CASE("listOsFiles")
{
  const auto env = fs::TestEnvironment{};
  const auto folder = env.dir() / "materials";
  writeTextFile(folder / "a.mtr", "a");
  writeTextFile(folder / "B.MTR", "b");
  writeTextFile(folder / "c.txt", "c");
  writeTextFile(folder / "sub" / "d.mtr", "d");

  SECTION("the files with the extension, as Sys_ListFiles lists them")
  {
#if defined(_WIN32)
    // PathMatchSpecW ignores case; the folders below are listed too
    CHECK(
      sorted(listOsFiles(folder, ".mtr"))
      == std::vector<std::string>{"B.MTR", "a.mtr", "sub/d.mtr"});
#else
    // fnmatch is case-sensitive, and the folder itself is listed alone
    CHECK(sorted(listOsFiles(folder, ".mtr")) == std::vector<std::string>{"a.mtr"});
#endif
    CHECK(listOsFiles(env.dir() / "none", ".mtr").empty());
  }

  SECTION("a set cancel flag lists nothing (G4)")
  {
    const auto cancel = std::atomic<bool>{true};
    CHECK(listOsFiles(folder, ".mtr", nullptr, &cancel).empty());
  }

#if defined(_WIN32)
  SECTION("a junction is followed as the engine follows it, a loop is not (A3.8)")
  {
    writeTextFile(env.dir() / "elsewhere" / "e.mtr", "e");
    REQUIRE(createJunction(folder / "linked", env.dir() / "elsewhere"));
    // a junction to the folder itself would loop
    REQUIRE(createJunction(folder / "sub" / "loop", folder));

    auto warnings = std::vector<std::string>{};
    CHECK(
      sorted(listOsFiles(folder, ".mtr", &warnings))
      == std::vector<std::string>{"B.MTR", "a.mtr", "linked/e.mtr", "sub/d.mtr"});
    REQUIRE(warnings.size() == 1u);
    CHECK_THAT(
      warnings.front(),
      Catch::Matchers::ContainsSubstring("links to a folder it lies in"));

    // the junctions go before the folder, so that nothing is removed through them
    CHECK(RemoveDirectoryW((folder / "sub" / "loop").c_str()));
    CHECK(RemoveDirectoryW((folder / "linked").c_str()));
  }
#endif
}

TEST_CASE("findLooseFile and the name helpers")
{
  const auto env = fs::TestEnvironment{};
  writeTextFile(env.dir() / "Mixed.Case", "x");

  SECTION("findLooseFile")
  {
    CHECK(findLooseFile(env.dir() / "Mixed.Case"));
    // Windows ignores case; elsewhere the folder is searched as OpenOSFile searches it
    CHECK(findLooseFile(env.dir() / "mixed.case"));
    CHECK_FALSE(findLooseFile(env.dir() / "none"));
    CHECK_FALSE(findLooseFile(env.dir()));
  }

  SECTION("paths to and from UTF-8")
  {
    const auto name = std::string{"caf\xC3\xA9/\xE3\x81\x81.mtr"};
    const auto path = pathFromUtf8(name);
    REQUIRE(path);
    CHECK(pathToUtf8(*path) == name);
#if defined(_WIN32)
    CHECK_FALSE(pathToUtf8(std::filesystem::path{L"bad\xD800"}));
#endif
  }

  SECTION("the engine's ASCII case rules")
  {
    CHECK(toLowerAscii("ABC\xC3\x89z") == "abc\xC3\x89z");
    CHECK(equalsNoCaseAscii("Materials/A.MTR", "materials/a.mtr"));
    CHECK_FALSE(equalsNoCaseAscii("a", "ab"));
    CHECK(compareNoCaseAscii("abc", "ABD", 2) == 0);
    CHECK(compareNoCaseAscii("abc", "ABD", 3) < 0);
    CHECK(endsWithNoCaseAscii("file.TGA", ".tga"));
    CHECK_FALSE(endsWithNoCaseAscii("tga", ".tga"));
  }
}

} // namespace tb::lit
