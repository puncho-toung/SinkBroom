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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/sys/win32/win_main.cpp, sys/posix/posix_main.cpp
// and framework/FileSystem.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/GameFile.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace tb::lit
{
namespace
{

std::string displayPath(const std::filesystem::path& path)
{
  return pathToUtf8(path).value_or("<a path that is not valid Unicode>");
}

#if defined(_WIN32)

// the ASCII suffix test on a name that is not converted yet: only 'A' to 'Z' fold
bool wideEndsWithNoCaseAscii(const std::wstring& text, const std::string_view suffix)
{
  if (text.size() < suffix.size())
  {
    return false;
  }
  const auto start = text.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i)
  {
    const auto c = text[start + i];
    const auto folded = c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c;
    if (folded != wchar_t(toLowerAscii(suffix[i])))
    {
      return false;
    }
  }
  return true;
}

/**
 * ListFilesRecursive (win_main.cpp:1017-1058): the folder's files ending in the
 * extension, then those of each folder below, with prefix in front. A folder that cannot
 * be read lists nothing, as FindFirstFileW failing returns; the others are still listed.
 */
void listFolderRecursive(
  const std::filesystem::path& folder,
  const std::string& prefix,
  const std::string& extension,
  std::vector<std::string>& names,
  std::vector<std::string>* warnings,
  const std::atomic<bool>* cancel)
{
  auto ec = std::error_code{};
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  auto it = std::filesystem::directory_iterator{folder, options, ec};
  for (; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
  {
    if (cancel && cancel->load())
    {
      return;
    }

    const auto& entry = *it;
    auto statusError = std::error_code{};
    const auto status = entry.symlink_status(statusError);
    if (statusError)
    {
      continue;
    }

    const auto fileName = entry.path().filename();
    const auto name = pathToUtf8(fileName);
    if (!name)
    {
      // only a folder, or a file this listing would return, is worth a warning
      const auto wanted = std::filesystem::is_directory(status)
                          || wideEndsWithNoCaseAscii(fileName.native(), extension);
      if (warnings && wanted)
      {
        warnings->push_back(fmt::format(
          "A file name in {} is not valid Unicode; it is skipped", displayPath(folder)));
      }
      continue;
    }

    if (std::filesystem::is_directory(status))
    {
      listFolderRecursive(
        entry.path(), prefix + *name + "/", extension, names, warnings, cancel);
      continue;
    }
    if (!std::filesystem::is_regular_file(status))
    {
      // a junction or a link to a folder is not followed (mirror.json)
      auto targetError = std::error_code{};
      if (entry.is_directory(targetError) || targetError)
      {
        continue;
      }
    }
    if (endsWithNoCaseAscii(*name, extension))
    {
      names.push_back(prefix + *name);
    }
  }
}

#else

bool equalsAsciiPaths(const std::string& lhs, const std::string& rhs)
{
  return equalsNoCaseAscii(lhs, rhs);
}

#endif

} // namespace

char toLowerAscii(const char c)
{
  return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

std::string toLowerAscii(const std::string_view text)
{
  auto result = std::string{text};
  for (auto& c : result)
  {
    c = toLowerAscii(c);
  }
  return result;
}

int compareNoCaseAscii(
  const std::string_view lhs, const std::string_view rhs, const std::size_t count)
{
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto a = i < lhs.size() ? lhs[i] : '\0';
    const auto b = i < rhs.size() ? rhs[i] : '\0';
    const auto la = toLowerAscii(a);
    const auto lb = toLowerAscii(b);
    if (la != lb)
    {
      return la < lb ? -1 : 1;
    }
    if (a == '\0')
    {
      return 0;
    }
  }
  return 0;
}

bool equalsNoCaseAscii(const std::string_view lhs, const std::string_view rhs)
{
  return lhs.size() == rhs.size() && compareNoCaseAscii(lhs, rhs, lhs.size()) == 0;
}

bool endsWithNoCaseAscii(const std::string_view text, const std::string_view suffix)
{
  return text.size() >= suffix.size()
         && equalsNoCaseAscii(text.substr(text.size() - suffix.size()), suffix);
}

std::optional<std::string> pathToUtf8(const std::filesystem::path& path) noexcept
{
  try
  {
    const auto text = path.u8string();
    return std::string{text.begin(), text.end()};
  }
  catch (...)
  {
    return std::nullopt;
  }
}

std::optional<std::filesystem::path> pathFromUtf8(const std::string& path) noexcept
{
  try
  {
    return std::filesystem::path{std::u8string{path.begin(), path.end()}};
  }
  catch (...)
  {
    return std::nullopt;
  }
}

Result<GameFile> GameFile::open(const std::filesystem::path& path)
{
  auto file = GameFile{};
#if defined(_WIN32)
  // share read, write and delete (R4-3.26); a directory fails without
  // FILE_FLAG_BACKUP_SEMANTICS
  const auto handle = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    return Error{
      fmt::format("Cannot open {} (error {})", displayPath(path), GetLastError())};
  }
  file.m_handle = handle;

  auto size = LARGE_INTEGER{};
  if (!GetFileSizeEx(handle, &size))
  {
    return Error{fmt::format("Cannot read the size of {}", displayPath(path))};
  }
  file.m_size = std::uint64_t(size.QuadPart);
#else
  const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    return Error{fmt::format("Cannot open {}", displayPath(path))};
  }
  file.m_fd = fd;

  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode))
  {
    return Error{fmt::format("{} is not a readable file", displayPath(path))};
  }
  file.m_size = std::uint64_t(status.st_size);
#endif
  return Result<GameFile>{std::move(file)};
}

GameFile::GameFile(GameFile&& other) noexcept
#if defined(_WIN32)
  : m_handle{std::exchange(other.m_handle, nullptr)}
#else
  : m_fd{std::exchange(other.m_fd, -1)}
#endif
  , m_size{other.m_size}
{
}

GameFile& GameFile::operator=(GameFile&& other) noexcept
{
  if (this != &other)
  {
    close();
#if defined(_WIN32)
    m_handle = std::exchange(other.m_handle, nullptr);
#else
    m_fd = std::exchange(other.m_fd, -1);
#endif
    m_size = other.m_size;
  }
  return *this;
}

GameFile::~GameFile()
{
  close();
}

void GameFile::close()
{
#if defined(_WIN32)
  if (m_handle)
  {
    CloseHandle(m_handle);
    m_handle = nullptr;
  }
#else
  if (m_fd >= 0)
  {
    ::close(m_fd);
    m_fd = -1;
  }
#endif
}

std::uint64_t GameFile::size() const
{
  return m_size;
}

Result<void> GameFile::readAt(
  const std::uint64_t offset, char* buffer, const std::size_t length) const
{
  if (offset > m_size || length > m_size - offset)
  {
    return Error{fmt::format(
      "{} bytes at {} do not fit in a file of {} bytes", length, offset, m_size)};
  }

  auto done = std::size_t{0};
  while (done < length)
  {
#if defined(_WIN32)
    const auto position = offset + done;
    auto overlapped = OVERLAPPED{};
    overlapped.Offset = DWORD(position & 0xFFFFFFFFu);
    overlapped.OffsetHigh = DWORD(position >> 32);
    const auto chunk = DWORD(std::min<std::size_t>(length - done, std::size_t{1} << 30));
    auto bytesRead = DWORD{0};
    if (
      !ReadFile(m_handle, buffer + done, chunk, &bytesRead, &overlapped)
      || bytesRead == 0)
    {
      return Error{fmt::format("Reading {} bytes at {} failed", length, offset)};
    }
#else
    const auto bytesRead =
      ::pread(m_fd, buffer + done, length - done, off_t(offset + done));
    if (bytesRead <= 0)
    {
      return Error{fmt::format("Reading {} bytes at {} failed", length, offset)};
    }
#endif
    done += std::size_t(bytesRead);
  }
  return kdl::void_success;
}

Result<std::string> readGameFile(const std::filesystem::path& path)
{
  return GameFile::open(path) | kdl::and_then([&](GameFile file) -> Result<std::string> {
           auto text = std::string{};
           text.resize(std::size_t(file.size()));
           return file.readAt(0, text.data(), text.size())
                  | kdl::transform([&]() { return std::move(text); });
         });
}

std::optional<GameFileStat> statGameFile(const std::filesystem::path& path) noexcept
{
  auto ec = std::error_code{};
  if (!std::filesystem::is_regular_file(path, ec) || ec)
  {
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
  {
    return std::nullopt;
  }
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec)
  {
    return std::nullopt;
  }
  return GameFileStat{std::uint64_t(size), std::int64_t(time.time_since_epoch().count())};
}

std::vector<std::string> listOsFiles(
  const std::filesystem::path& folder,
  const std::string& extension,
  std::vector<std::string>* warnings,
  const std::atomic<bool>* cancel)
{
  auto names = std::vector<std::string>{};

#if defined(_WIN32)
  listFolderRecursive(folder, {}, extension, names, warnings, cancel);
#else
  // the folder only; fnmatch( "*<extension>", name, 0 ) and stat says no directory
  const auto pattern = "*" + extension;
  auto ec = std::error_code{};
  for (auto it = std::filesystem::directory_iterator{folder, ec};
       !ec && it != std::filesystem::directory_iterator{};
       it.increment(ec))
  {
    if (cancel && cancel->load())
    {
      break;
    }
    auto entryError = std::error_code{};
    const auto status = it->status(entryError);
    if (entryError || std::filesystem::is_directory(status))
    {
      continue;
    }
    const auto name = pathToUtf8(it->path().filename());
    if (!name)
    {
      if (warnings)
      {
        warnings->push_back(fmt::format(
          "A file name in {} is not valid Unicode; it is skipped", displayPath(folder)));
      }
      continue;
    }
    if (fnmatch(pattern.c_str(), name->c_str(), 0) == 0)
    {
      names.push_back(*name);
    }
  }
#endif

  return names;
}

std::optional<std::filesystem::path> findLooseFile(
  const std::filesystem::path& path) noexcept
{
  try
  {
    auto ec = std::error_code{};
    if (std::filesystem::is_regular_file(path, ec))
    {
      return path;
    }
#if defined(_WIN32)
    return std::nullopt;
#else
    // OpenOSFile's fallback: the folder's files compared case-insensitively
    const auto wanted = pathToUtf8(path);
    if (!wanted)
    {
      return std::nullopt;
    }
    const auto folder = path.parent_path();
    for (auto it = std::filesystem::directory_iterator{folder, ec};
         !ec && it != std::filesystem::directory_iterator{};
         it.increment(ec))
    {
      const auto candidate = pathToUtf8(it->path());
      if (candidate && equalsAsciiPaths(*candidate, *wanted))
      {
        auto entryError = std::error_code{};
        if (std::filesystem::is_regular_file(it->path(), entryError))
        {
          return it->path();
        }
      }
    }
    return std::nullopt;
#endif
  }
  catch (...)
  {
    return std::nullopt;
  }
}

} // namespace tb::lit
