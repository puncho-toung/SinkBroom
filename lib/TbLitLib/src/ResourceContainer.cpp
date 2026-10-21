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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/File_Resource.cpp and
// framework/File.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/ResourceContainer.h"

#include "lit/GameFile.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace tb::lit
{
namespace
{

constexpr std::size_t HeaderSize = 12;

std::int32_t readBigInt32(const char* bytes)
{
  const auto* b = reinterpret_cast<const unsigned char*>(bytes);
  return std::int32_t(
    (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8)
    | std::uint32_t(b[3]));
}

std::int32_t readLittleInt32(const char* bytes)
{
  const auto* b = reinterpret_cast<const unsigned char*>(bytes);
  return std::int32_t(
    std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16)
    | (std::uint32_t(b[3]) << 24));
}

// GameFile::readAt as a step of a pipeline, failing with error when the bytes cannot be
// read
Result<void> readExactly(
  const GameFile& file,
  const std::uint64_t offset,
  char* bytes,
  const std::size_t count,
  std::string error)
{
  return file.readAt(offset, bytes, count)
         | kdl::or_else(
           [&](const auto&) -> Result<void> { return Error{std::move(error)}; });
}

// idResourceCacheEntry::Read for every entry of the table, in its order; an entry whose
// data does not fit in the file is dropped with a warning
Result<std::vector<ResourceEntry>> readEntries(
  const std::string& table,
  const std::string& name,
  const std::uint64_t fileSize,
  std::vector<std::string>& warnings,
  const std::atomic<bool>* cancel)
{
  const auto truncated = [&]() -> Result<std::vector<ResourceEntry>> {
    return Error{
      fmt::format("Resource file {}: its table is truncated; it is not read", name)};
  };

  const auto numFileResources = readBigInt32(table.data());
  if (numFileResources < 0)
  {
    return truncated();
  }

  auto entries = std::vector<ResourceEntry>{};
  auto position = std::size_t{4};
  for (std::int32_t i = 0; i < numFileResources; ++i)
  {
    // R5.20: a stop waits for one table entry
    if (cancel && cancel->load())
    {
      return Error{fmt::format("Reading resource file {} was cancelled", name)};
    }

    // idResourceCacheEntry::Read: ReadString, then ReadBig offset and length
    if (table.size() - position < 4)
    {
      return truncated();
    }
    const auto nameLength = readLittleInt32(table.data() + position);
    position += 4;

    auto entryName = std::string{};
    // idFile::ReadString reads nothing for a negative length
    if (nameLength >= 0)
    {
      if (table.size() - position < std::size_t(nameLength))
      {
        return truncated();
      }
      entryName = table.substr(position, std::size_t(nameLength));
      position += std::size_t(nameLength);
    }

    if (table.size() - position < 8)
    {
      return truncated();
    }
    const auto offset = readBigInt32(table.data() + position);
    const auto length = readBigInt32(table.data() + position + 4);
    position += 8;

    // the engine's lookups and comparisons see the name up to its first NUL
    entryName.resize(std::strlen(entryName.c_str()));
    entryName = canonicalResourceName(entryName);
    if (
      offset < 0 || length < 0
      || std::uint64_t(offset) + std::uint64_t(length) > fileSize)
    {
      warnings.push_back(fmt::format(
        "Resource file {}: entry '{}' ({} bytes at {}) does not fit in the file; it is "
        "skipped",
        name,
        entryName,
        length,
        offset));
      continue;
    }

    entries.push_back(
      ResourceEntry{std::move(entryName), std::uint32_t(offset), std::uint32_t(length)});
  }
  return entries;
}

} // namespace

std::string canonicalResourceName(const std::string_view path)
{
  // idStr::BackSlashesToSlashes, then idStr::ToLower; its CharIsUpper also accepts
  // 0xC0-0xDF, but a char is signed in the engine's builds, so only ASCII is lowered
  auto name = toLowerAscii(path);
  std::replace(name.begin(), name.end(), '\\', '/');
  return name;
}

Result<ResourceContainer> ResourceContainer::open(
  const std::filesystem::path& path,
  std::string name,
  std::vector<std::string>& warnings,
  const std::atomic<bool>* cancel)
{
  // idResourceContainer::Init's warning
  const auto unopened = fmt::format("Unable to open resource file {}", name);
  return GameFile::open(path)
         | kdl::or_else([&](const auto&) -> Result<GameFile> { return Error{unopened}; })
         | kdl::and_then([&](GameFile file) -> Result<ResourceContainer> {
             const auto stat = statGameFile(path);
             if (!stat)
             {
               return Error{unopened};
             }

             const auto fileSize = file.size();
             const auto tooShort = fmt::format(
               "Resource file {} is too short for its header; it is not read", name);
             if (fileSize < HeaderSize)
             {
               return Error{tooShort};
             }
             auto header = std::array<char, HeaderSize>{};
             auto table = std::string{};
             return readExactly(file, 0, header.data(), header.size(), tooShort)
                    | kdl::and_then([&]() -> Result<void> {
                        if (
                          std::uint32_t(readBigInt32(header.data())) != ResourceFileMagic)
                        {
                          // the engine stops here with FatalError(
                          // "resourceFileMagic != RESOURCE_FILE_MAGIC" )
                          return Error{fmt::format(
                            "Resource file {}: resourceFileMagic != RESOURCE_FILE_MAGIC; "
                            "it is not read",
                            name)};
                        }

                        const auto tableOffset = readBigInt32(header.data() + 4);
                        const auto tableLength = readBigInt32(header.data() + 8);
                        if (
                          tableOffset < 0 || tableLength < 4
                          || std::uint64_t(tableOffset) + std::uint64_t(tableLength)
                               > fileSize)
                        {
                          return Error{fmt::format(
                            "Resource file {}: its table ({} bytes at {}) does not fit "
                            "in "
                            "its {} bytes; it is not read",
                            name,
                            tableLength,
                            tableOffset,
                            fileSize)};
                        }

                        table.resize(std::size_t(tableLength));
                        return readExactly(
                          file,
                          std::uint64_t(tableOffset),
                          table.data(),
                          table.size(),
                          fmt::format(
                            "Resource file {}: its table cannot be read", name));
                      })
                    | kdl::and_then([&]() {
                        return readEntries(table, name, fileSize, warnings, cancel);
                      })
                    | kdl::transform([&](std::vector<ResourceEntry> entries) {
                        auto container = ResourceContainer{};
                        container.m_path = path;
                        container.m_fileSize = fileSize;
                        container.m_fileTime = stat->time;
                        container.m_entries = std::move(entries);
                        for (std::size_t i = 0; i < container.m_entries.size(); ++i)
                        {
                          // the later of two entries with one name wins, as idHashIndex
                          // finds it
                          container.m_index[container.m_entries[i].name] = i;
                        }
                        container.m_name = std::move(name);
                        return container;
                      });
           });
}

const std::string& ResourceContainer::name() const
{
  return m_name;
}

const std::filesystem::path& ResourceContainer::path() const
{
  return m_path;
}

const std::vector<ResourceEntry>& ResourceContainer::entries() const
{
  return m_entries;
}

const ResourceEntry* ResourceContainer::find(const std::string_view gamePath) const
{
  const auto it = m_index.find(canonicalResourceName(gamePath));
  return it != m_index.end() ? &m_entries[it->second] : nullptr;
}

Result<std::string> ResourceContainer::read(
  const ResourceEntry& entry, const GameFile& file) const
{
  const auto stat = statGameFile(m_path);
  if (
    !stat || stat->size != m_fileSize || stat->time != m_fileTime
    || file.size() != m_fileSize)
  {
    return Error{fmt::format(
      "Resource file {} changed since its table was read; '{}' is not read from it",
      m_name,
      entry.name)};
  }

  auto bytes = std::string{};
  bytes.resize(entry.length);
  return readExactly(
           file,
           entry.offset,
           bytes.data(),
           bytes.size(),
           fmt::format("Resource file {}: '{}' cannot be read", m_name, entry.name))
         | kdl::transform([&]() { return std::move(bytes); });
}

std::uint64_t ResourceContainer::fileSize() const
{
  return m_fileSize;
}

std::int64_t ResourceContainer::fileTime() const
{
  return m_fileTime;
}

} // namespace tb::lit
