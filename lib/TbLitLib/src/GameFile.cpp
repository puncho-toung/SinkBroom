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

#include <fmt/format.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace tb::lit
{
namespace
{

char toLowerAscii(const char c)
{
  return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}

bool endsWithNoCaseAscii(const std::string& text, const std::string& suffix)
{
  if (text.size() < suffix.size())
  {
    return false;
  }
  const auto offset = text.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i)
  {
    if (toLowerAscii(text[offset + i]) != toLowerAscii(suffix[i]))
    {
      return false;
    }
  }
  return true;
}

#if !defined(_WIN32)
bool equalsNoCaseAscii(const std::string& lhs, const std::string& rhs)
{
  if (lhs.size() != rhs.size())
  {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i)
  {
    if (toLowerAscii(lhs[i]) != toLowerAscii(rhs[i]))
    {
      return false;
    }
  }
  return true;
}
#endif

} // namespace

std::string pathToUtf8(const std::filesystem::path& path)
{
  const auto text = path.u8string();
  return std::string{text.begin(), text.end()};
}

std::filesystem::path pathFromUtf8(const std::string& path)
{
  return std::filesystem::path{std::u8string{path.begin(), path.end()}};
}

std::optional<GameFile> GameFile::open(
  const std::filesystem::path& path, std::string* error)
{
  auto file = GameFile{};
#if defined(_WIN32)
  // share read and delete (R4-3.26); a directory fails without FILE_FLAG_BACKUP_SEMANTICS
  const auto handle = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    if (error)
    {
      *error = fmt::format("cannot open {} (error {})", pathToUtf8(path), GetLastError());
    }
    return std::nullopt;
  }
  file.m_handle = handle;

  auto size = LARGE_INTEGER{};
  if (!GetFileSizeEx(handle, &size))
  {
    if (error)
    {
      *error = fmt::format("cannot read the size of {}", pathToUtf8(path));
    }
    return std::nullopt;
  }
  file.m_size = std::uint64_t(size.QuadPart);
#else
  const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    if (error)
    {
      *error = fmt::format("cannot open {}", pathToUtf8(path));
    }
    return std::nullopt;
  }
  file.m_fd = fd;

  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode))
  {
    if (error)
    {
      *error = fmt::format("{} is not a readable file", pathToUtf8(path));
    }
    return std::nullopt;
  }
  file.m_size = std::uint64_t(status.st_size);
#endif
  return file;
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

bool GameFile::readAt(
  const std::uint64_t offset, char* buffer, const std::size_t length) const
{
  if (offset > m_size || length > m_size - offset)
  {
    return false;
  }

  auto done = std::size_t{0};
  while (done < length)
  {
#if defined(_WIN32)
    const auto position = offset + done;
    auto overlapped = OVERLAPPED{};
    overlapped.Offset = DWORD(position & 0xFFFFFFFFu);
    overlapped.OffsetHigh = DWORD(position >> 32);
    const auto chunk = DWORD(std::min<std::size_t>(length - done, 1u << 30));
    auto read = DWORD{0};
    if (!ReadFile(m_handle, buffer + done, chunk, &read, &overlapped) || read == 0)
    {
      return false;
    }
#else
    const auto read = ::pread(m_fd, buffer + done, length - done, off_t(offset + done));
    if (read <= 0)
    {
      return false;
    }
#endif
    done += std::size_t(read);
  }
  return true;
}

std::optional<std::string> readGameFile(
  const std::filesystem::path& path, std::string* error)
{
  auto file = GameFile::open(path, error);
  if (!file)
  {
    return std::nullopt;
  }

  auto text = std::string{};
  text.resize(std::size_t(file->size()));
  if (!file->readAt(0, text.data(), text.size()))
  {
    if (error)
    {
      *error = fmt::format("cannot read {}", pathToUtf8(path));
    }
    return std::nullopt;
  }
  return text;
}

std::optional<GameFileStat> statGameFile(const std::filesystem::path& path)
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
  const std::filesystem::path& folder, const std::string& extension)
{
  auto names = std::vector<std::string>{};
  auto ec = std::error_code{};

#if defined(_WIN32)
  // ListFilesRecursive: every folder below, the name matched by PathMatchSpecW
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (auto it = std::filesystem::recursive_directory_iterator{folder, options, ec};
       !ec && it != std::filesystem::recursive_directory_iterator{};
       it.increment(ec))
  {
    auto entryError = std::error_code{};
    if (it->is_directory(entryError))
    {
      continue;
    }
    if (endsWithNoCaseAscii(pathToUtf8(it->path().filename()), extension))
    {
      const auto relative = it->path().lexically_relative(folder).generic_u8string();
      names.emplace_back(relative.begin(), relative.end());
    }
  }
#else
  // the folder only; fnmatch( "*<extension>", name, 0 ) and stat says no directory
  const auto pattern = "*" + extension;
  for (auto it = std::filesystem::directory_iterator{folder, ec};
       !ec && it != std::filesystem::directory_iterator{};
       it.increment(ec))
  {
    auto entryError = std::error_code{};
    const auto status = it->status(entryError);
    if (entryError || std::filesystem::is_directory(status))
    {
      continue;
    }
    const auto name = pathToUtf8(it->path().filename());
    if (fnmatch(pattern.c_str(), name.c_str(), 0) == 0)
    {
      names.push_back(name);
    }
  }
#endif

  return names;
}

std::optional<std::filesystem::path> findLooseFile(const std::filesystem::path& path)
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
  const auto folder = path.parent_path();
  const auto wanted = pathToUtf8(path);
  for (auto it = std::filesystem::directory_iterator{folder, ec};
       !ec && it != std::filesystem::directory_iterator{};
       it.increment(ec))
  {
    if (equalsNoCaseAscii(pathToUtf8(it->path()), wanted))
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

} // namespace tb::lit
