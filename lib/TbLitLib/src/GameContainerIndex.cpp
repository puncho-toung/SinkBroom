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
// GPL Source Code and RBDOOM-3-BFG (neo/framework/FileSystem.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/GameContainerIndex.h"

#include "lit/GameFile.h"
#include "lit/GameFileSource.h"
#include "lit/MaterialDecls.h"
#include "lit/ResourceContainer.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace tb::lit
{
namespace
{

using Clock = std::chrono::steady_clock;

double secondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void sortEnginePaths(std::vector<std::string>& names)
{
  // idSort_PathStr
  std::ranges::sort(names, [](const auto& lhs, const auto& rhs) {
    return compareEnginePaths(lhs, rhs) < 0;
  });
}

/**
 * The key's base (C67 (a)): the folder made absolute once (against the process's current
 * folder) and lexically normal, then as the file system spells it when it exists
 * (canonical: the folders' own case, links resolved). Any thread.
 */
std::filesystem::path spelledBase(const std::filesystem::path& folder)
{
  auto ec = std::error_code{};
  auto absolute = std::filesystem::absolute(folder, ec);
  if (ec)
  {
    absolute = folder;
  }
  absolute = absolute.lexically_normal();
  auto canonical = std::filesystem::canonical(absolute, ec);
  return ec ? absolute : canonical;
}

// ----- the index file -----

constexpr auto FileMagic = std::string_view{"SBLITIDX"};
// the magic, the version, the path unit size, the payload size
constexpr auto HeaderBytes = std::size_t{8 + 4 + 4 + 8};
constexpr auto HashBytes = std::size_t{8};
constexpr auto PathUnitBytes = std::uint32_t(sizeof(std::filesystem::path::value_type));
// how a stored path is spelled: its UTF-8 (when that gives the same native path back),
// else its native units
constexpr auto PathAsUtf8 = std::uint8_t{0};
constexpr auto PathAsUnits = std::uint8_t{1};

// a little-endian integer of count bytes at bytes
std::uint64_t littleEndianAt(const char* bytes, const std::size_t count)
{
  auto value = std::uint64_t{0};
  if constexpr (std::endian::native == std::endian::little)
  {
    std::memcpy(&value, bytes, count);
  }
  else
  {
    for (std::size_t i = 0; i < count; ++i)
    {
      value |= std::uint64_t(static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
  }
  return value;
}

/**
 * The payload's hash: FNV-1a's step over 64-bit little-endian words, each folded back
 * (the high half into the low), then over the bytes left. Any one-bit change changes it
 * (each step is a bijection of the hash). Pure.
 */
std::uint64_t payloadHash(const char* bytes, const std::size_t count)
{
  constexpr auto Prime = std::uint64_t{1099511628211ull};
  auto hash = std::uint64_t{14695981039346656037ull};
  auto i = std::size_t{0};
  for (; i + 8 <= count; i += 8)
  {
    hash = (hash ^ littleEndianAt(bytes + i, 8)) * Prime;
    hash ^= hash >> 32;
  }
  for (; i < count; ++i)
  {
    hash = (hash ^ std::uint64_t(static_cast<unsigned char>(bytes[i]))) * Prime;
  }
  return hash;
}

class FileWriter
{
private:
  std::string m_bytes;

  void unsignedOf(const std::uint64_t value, const std::size_t count)
  {
    char bytes[8];
    for (std::size_t i = 0; i < count; ++i)
    {
      bytes[i] = char((value >> (8 * i)) & 0xFFu);
    }
    m_bytes.append(bytes, count);
  }

public:
  explicit FileWriter(const std::size_t reserve) { m_bytes.reserve(reserve); }

  void raw(const std::string_view value) { m_bytes.append(value); }
  void u8(const std::uint8_t value) { m_bytes.push_back(char(value)); }
  void u32(const std::uint32_t value) { unsignedOf(value, 4); }
  void u64(const std::uint64_t value) { unsignedOf(value, 8); }
  void i64(const std::int64_t value) { u64(std::uint64_t(value)); }

  void text(const std::string& value)
  {
    u32(std::uint32_t(value.size()));
    m_bytes += value;
  }

  // a path's native units, each as little-endian bytes of the unit's size
  void units(const std::filesystem::path& value)
  {
    const auto& native = value.native();
    u32(std::uint32_t(native.size()));
    for (const auto unit : native)
    {
      unsignedOf(
        std::uint64_t(
          static_cast<std::make_unsigned_t<std::filesystem::path::value_type>>(unit)),
        PathUnitBytes);
    }
  }

  // a path as its UTF-8 when that spells it again, else as its units
  void path(const std::filesystem::path& value)
  {
    if (const auto utf8 = pathToUtf8(value))
    {
      if (const auto back = pathFromUtf8(*utf8); back && back->native() == value.native())
      {
        u8(PathAsUtf8);
        text(*utf8);
        return;
      }
    }
    u8(PathAsUnits);
    units(value);
  }

  void stamps(const std::vector<ContainerStamp>& values)
  {
    u32(std::uint32_t(values.size()));
    for (const auto& value : values)
    {
      text(value.name);
      u64(value.size);
      i64(value.stamp);
    }
  }

  std::string& bytes() { return m_bytes; }
};

/**
 * Reads the payload with every length and count checked against what is left, as
 * ResourceContainer checks a table against its file: a read past the end fails the
 * reader and returns zeros, so that nothing out of bounds is ever read.
 */
class FileReader
{
private:
  const std::string& m_bytes;
  std::size_t m_position;
  std::size_t m_end;
  bool m_failed = false;

public:
  FileReader(const std::string& bytes, const std::size_t begin, const std::size_t end)
    : m_bytes{bytes}
    , m_position{begin}
    , m_end{end}
  {
  }

  bool failed() const { return m_failed; }
  bool atEnd() const { return m_position == m_end; }

  bool has(const std::size_t count)
  {
    if (m_failed || m_end - m_position < count)
    {
      m_failed = true;
      return false;
    }
    return true;
  }

  std::uint64_t unsignedOf(const std::size_t count)
  {
    if (!has(count))
    {
      return 0;
    }
    const auto value = littleEndianAt(m_bytes.data() + m_position, count);
    m_position += count;
    return value;
  }

  std::uint8_t u8() { return std::uint8_t(unsignedOf(1)); }
  std::uint32_t u32() { return std::uint32_t(unsignedOf(4)); }
  std::uint64_t u64() { return unsignedOf(8); }
  std::int64_t i64() { return std::int64_t(unsignedOf(8)); }

  // a count of records of at least minBytes each, which what is left must hold
  std::uint32_t count(const std::size_t minBytes)
  {
    const auto value = u32();
    if (!m_failed && std::uint64_t(value) * minBytes > m_end - m_position)
    {
      m_failed = true;
      return 0;
    }
    return m_failed ? 0 : value;
  }

  std::string text()
  {
    const auto size = u32();
    if (!has(size))
    {
      return {};
    }
    auto value = m_bytes.substr(m_position, size);
    m_position += size;
    return value;
  }

  std::filesystem::path units()
  {
    const auto count = u32();
    if (!has(std::size_t(count) * PathUnitBytes))
    {
      return {};
    }
    auto native = std::filesystem::path::string_type{};
    native.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
      native.push_back(std::filesystem::path::value_type(unsignedOf(PathUnitBytes)));
    }
    return std::filesystem::path{std::move(native)};
  }

  std::filesystem::path path()
  {
    switch (u8())
    {
    case PathAsUtf8:
      if (auto path = pathFromUtf8(text()))
      {
        return std::move(*path);
      }
      break;
    case PathAsUnits:
      return units();
    default:
      break;
    }
    m_failed = true;
    return {};
  }

  std::vector<ContainerStamp> stamps()
  {
    // a name's length, the size, the stamp
    const auto count = this->count(4 + 8 + 8);
    auto values = std::vector<ContainerStamp>{};
    values.reserve(count);
    for (std::uint32_t i = 0; i < count && !m_failed; ++i)
    {
      auto name = text();
      const auto size = u64();
      const auto stamp = i64();
      values.push_back(ContainerStamp{std::move(name), size, stamp});
    }
    return values;
  }
};

// a stored table's container in its key, at or after from: its index, or none
std::optional<std::size_t> stampOf(
  const std::vector<ContainerStamp>& stamps,
  const std::string& name,
  const std::size_t from)
{
  for (auto i = from; i < stamps.size(); ++i)
  {
    if (stamps[i].name == name)
    {
      return i;
    }
  }
  return std::nullopt;
}

/**
 * The tables agree with their key: each table is a container the key lists, in the key's
 * order, with the key's size and stamp (a container that changed while the scan read it
 * holds a table of another file than its key's: never stored, never read)
 */
bool tablesMatchKey(const GameContainerTables& tables)
{
  auto next = std::size_t{0};
  for (const auto& container : tables.resources)
  {
    const auto at = stampOf(tables.key.resources, container->name(), next);
    if (
      !at || tables.key.resources[*at].size != container->fileSize()
      || tables.key.resources[*at].stamp != container->fileTime())
    {
      return false;
    }
    next = *at + 1;
  }
  next = 0;
  for (const auto& pk4 : tables.pk4s)
  {
    const auto at = stampOf(tables.key.pk4s, pk4->name, next);
    if (
      !at || tables.key.pk4s[*at].size != pk4->size
      || tables.key.pk4s[*at].stamp != pk4->stamp)
    {
      return false;
    }
    next = *at + 1;
  }
  return true;
}

std::string hexOf(const std::uint64_t value)
{
  return fmt::format("{:016x}", value);
}

} // namespace

GameContainerListing listGameContainers(
  const std::filesystem::path& folder, const std::atomic<bool>* cancel)
{
  // AddGameDirectory( <game path>, "base" ), FileSystem.cpp:3174-3252
  auto listing = GameContainerListing{};
  listing.base = spelledBase(folder);
  const auto cancelled = [&] { return cancel && cancel->load(); };

  auto resourceNames =
    listOsFiles(folder, ".resources", &listing.resourceWarnings, cancel);
  sortEnginePaths(resourceNames);
  auto mapResourceNames =
    listOsFiles(folder / "maps", ".resources", &listing.resourceWarnings, cancel);
  sortEnginePaths(mapResourceNames);
  for (auto& name : mapResourceNames)
  {
    resourceNames.push_back("maps/" + name);
  }

  // a container listed twice (Windows lists base/maps in both passes) is kept at its
  // later place: the earlier one finds nothing the later one does not find first
  for (std::size_t i = 0; i < resourceNames.size(); ++i)
  {
    auto later = false;
    for (auto k = i + 1; k < resourceNames.size() && !later; ++k)
    {
      later = equalsNoCaseAscii(resourceNames[i], resourceNames[k]);
    }
    if (!later && pathFromUtf8(resourceNames[i]))
    {
      listing.resources.push_back(resourceNames[i]);
    }
  }

  // RB: Zip support, the base pass only
  auto pk4Names = listOsFiles(folder, ".pk4", &listing.pk4Warnings, cancel);
  sortEnginePaths(pk4Names);
  for (auto& name : pk4Names)
  {
    if (pathFromUtf8(name))
    {
      listing.pk4s.push_back(std::move(name));
    }
  }
  if (cancelled())
  {
    return listing;
  }

  // the key: one stat per container; a container that cannot be statted leaves no key
  auto key = GameContainerKey{listing.base, {}, {}};
  const auto stampAll =
    [&](const std::vector<std::string>& names, std::vector<ContainerStamp>& stamps) {
      for (const auto& name : names)
      {
        const auto stat = statGameFile(listing.base / *pathFromUtf8(name));
        if (!stat)
        {
          return false;
        }
        stamps.push_back(ContainerStamp{name, stat->size, stat->time});
      }
      return true;
    };
  if (stampAll(listing.resources, key.resources) && stampAll(listing.pk4s, key.pk4s))
  {
    listing.key = std::move(key);
  }
  return listing;
}

std::string writeContainerIndexFile(const GameContainerTables& tables)
{
  // the bytes, about: the header, the hash, and per entry its name and two integers
  auto estimate = HeaderBytes + HashBytes + std::size_t{4096};
  for (const auto& container : tables.resources)
  {
    for (const auto& entry : container->entries())
    {
      estimate += entry.name.size() + 12;
    }
  }
  for (const auto& pk4 : tables.pk4s)
  {
    for (const auto& name : pk4->names)
    {
      estimate += 2 * name.size() + 9;
    }
  }

  auto file = FileWriter{estimate};
  file.raw(FileMagic);
  file.u32(ContainerIndexFileVersion);
  file.u32(PathUnitBytes);
  // the payload's size, written over once it is known
  file.u64(0);

  file.units(tables.key.base);
  file.stamps(tables.key.resources);
  file.stamps(tables.key.pk4s);
  file.u32(std::uint32_t(tables.warnings.size()));
  for (const auto& warning : tables.warnings)
  {
    file.text(warning);
  }
  file.u32(std::uint32_t(tables.resources.size()));
  for (const auto& container : tables.resources)
  {
    // the path is the key's base and the name, made again on read
    file.text(container->name());
    file.u64(container->fileSize());
    file.i64(container->fileTime());
    file.u32(std::uint32_t(container->entries().size()));
    for (const auto& entry : container->entries())
    {
      file.text(entry.name);
      file.u32(entry.offset);
      file.u32(entry.length);
    }
  }
  file.u32(std::uint32_t(tables.pk4s.size()));
  for (const auto& pk4 : tables.pk4s)
  {
    file.text(pk4->name);
    file.i64(pk4->stamp);
    file.u64(pk4->size);
    file.u32(std::uint32_t(pk4->names.size()));
    for (const auto& name : pk4->names)
    {
      file.text(name);
      file.path(pk4->index.at(name));
    }
  }

  auto& bytes = file.bytes();
  const auto payloadBytes = std::uint64_t(bytes.size() - HeaderBytes);
  for (std::size_t i = 0; i < 8; ++i)
  {
    bytes[HeaderBytes - 8 + i] = char((payloadBytes >> (8 * i)) & 0xFFu);
  }
  file.u64(payloadHash(bytes.data() + HeaderBytes, std::size_t(payloadBytes)));
  return std::move(bytes);
}

std::optional<GameContainerTables> readContainerIndexFile(
  const std::string& bytes,
  const GameContainerKey& key,
  std::string* error,
  bool* damaged)
{
  const auto fail = [&](const std::string& why, const bool isDamaged) {
    if (error)
    {
      *error = why;
    }
    if (damaged)
    {
      *damaged = isDamaged;
    }
    return std::optional<GameContainerTables>{};
  };

  if (bytes.size() < HeaderBytes + HashBytes)
  {
    return fail(
      fmt::format("it is {} bytes, shorter than its header", bytes.size()), true);
  }
  if (std::string_view{bytes}.substr(0, FileMagic.size()) != FileMagic)
  {
    return fail("it is no container index", true);
  }
  auto header = FileReader{bytes, FileMagic.size(), HeaderBytes};
  const auto version = header.u32();
  const auto unitBytes = header.u32();
  const auto payloadBytes = header.u64();
  if (version != ContainerIndexFileVersion)
  {
    return fail(fmt::format("its version is {}", version), false);
  }
  if (unitBytes != PathUnitBytes)
  {
    return fail(fmt::format("its paths have {}-byte units", unitBytes), true);
  }
  if (payloadBytes != bytes.size() - HeaderBytes - HashBytes)
  {
    return fail(
      fmt::format(
        "it holds {} bytes for a payload of {}",
        bytes.size() - HeaderBytes - HashBytes,
        payloadBytes),
      true);
  }
  const auto payloadEnd = HeaderBytes + std::size_t(payloadBytes);
  auto trailer = FileReader{bytes, payloadEnd, bytes.size()};
  if (trailer.u64() != payloadHash(bytes.data() + HeaderBytes, std::size_t(payloadBytes)))
  {
    return fail("its hash does not match its bytes", true);
  }

  auto reader = FileReader{bytes, HeaderBytes, payloadEnd};
  auto tables = GameContainerTables{};
  tables.key.base = reader.units();
  tables.key.resources = reader.stamps();
  tables.key.pk4s = reader.stamps();
  if (reader.failed())
  {
    return fail("its key is cut short", true);
  }
  if (tables.key != key)
  {
    // a container added, removed, renamed, resized or restamped, or another base
    return fail("it holds another key", false);
  }

  const auto warnings = reader.count(4);
  for (std::uint32_t i = 0; i < warnings && !reader.failed(); ++i)
  {
    tables.warnings.push_back(reader.text());
  }

  // a name's length, the size, the time, the entry count
  const auto resources = reader.count(4 + 8 + 8 + 4);
  for (std::uint32_t i = 0; i < resources && !reader.failed(); ++i)
  {
    auto name = reader.text();
    const auto fileSize = reader.u64();
    const auto fileTime = reader.i64();
    // a name's length, the offset, the length
    const auto count = reader.count(4 + 4 + 4);
    auto entries = std::vector<ResourceEntry>{};
    entries.reserve(count);
    for (std::uint32_t k = 0; k < count && !reader.failed(); ++k)
    {
      auto entryName = reader.text();
      const auto offset = reader.u32();
      const auto length = reader.u32();
      entries.push_back(ResourceEntry{std::move(entryName), offset, length});
    }
    const auto path = pathFromUtf8(name);
    if (reader.failed() || !path)
    {
      return fail("a .resources table is cut short", true);
    }
    auto container = ResourceContainer::fromTable(
      name, key.base / *path, fileSize, fileTime, std::move(entries));
    if (!container)
    {
      return fail(fmt::format("an entry of {} does not fit in it", name), true);
    }
    tables.resources.push_back(
      std::make_shared<const ResourceContainer>(std::move(*container)));
  }

  // a name's length, the stamp, the size, the name count
  const auto pk4s = reader.count(4 + 8 + 8 + 4);
  for (std::uint32_t i = 0; i < pk4s && !reader.failed(); ++i)
  {
    auto pk4 = std::make_shared<Pk4Table>();
    pk4->name = reader.text();
    pk4->stamp = reader.i64();
    pk4->size = reader.u64();
    // a name's length, a path's kind and length
    const auto count = reader.count(4 + 1 + 4);
    pk4->names.reserve(count);
    for (std::uint32_t k = 0; k < count && !reader.failed(); ++k)
    {
      auto name = reader.text();
      auto entryPath = reader.path();
      if (!reader.failed() && !pk4->index.emplace(name, std::move(entryPath)).second)
      {
        return fail(fmt::format("{} lists an entry twice", pk4->name), true);
      }
      pk4->names.push_back(std::move(name));
    }
    const auto path = pathFromUtf8(pk4->name);
    if (reader.failed() || !path)
    {
      return fail("a .pk4 directory is cut short", true);
    }
    pk4->path = key.base / *path;
    tables.pk4s.push_back(std::move(pk4));
  }
  if (reader.failed() || !reader.atEnd())
  {
    return fail("its tables do not fill it", true);
  }
  if (!tablesMatchKey(tables))
  {
    return fail("a table is not its key's", true);
  }
  return tables;
}

GameContainerIndex::GameContainerIndex(std::filesystem::path folder)
  : m_folder{std::move(folder)}
{
}

GameContainerIndex::~GameContainerIndex() = default;

std::unique_ptr<GameFileSource> GameContainerIndex::makeSource(
  const std::filesystem::path& gamePath, const std::atomic<bool>* cancel)
{
  auto source = std::make_unique<GameFileSource>(gamePath, cancel, this);
  // a cancelled scan stores nothing (P8.17): only a complete source's tables, and none
  // that changed while they were read
  auto tables = source->scannedTables();
  if (!tables || !source->complete() || (cancel && cancel->load()))
  {
    return source;
  }
  if (!tablesMatchKey(*tables))
  {
    return source;
  }
  auto writeSeconds = 0.0;
  auto fileBytes = std::uint64_t{0};
  auto warnings = std::vector<std::string>{};
  store(std::move(tables), writeSeconds, fileBytes, warnings);
  source->recordIndexWrite(writeSeconds, fileBytes, std::move(warnings));
  return source;
}

std::shared_ptr<const GameContainerTables> GameContainerIndex::find(
  const GameContainerKey& key,
  bool& fromFile,
  std::uint64_t& fileBytes,
  std::vector<std::string>& warnings)
{
  fromFile = false;
  fileBytes = 0;
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (const auto it = m_tables.find(key.base.native());
        it != m_tables.end() && it->second->key == key)
    {
      return it->second;
    }
  }
  if (m_folder.empty())
  {
    return nullptr;
  }

  const auto file = filePath(key.base);
  auto ec = std::error_code{};
  if (!std::filesystem::is_regular_file(file, ec))
  {
    return nullptr;
  }
  const auto display = pathToUtf8(file).value_or("the container index");
  return readGameFile(file) | kdl::transform([&](const std::string& bytes) {
           auto error = std::string{};
           auto damaged = false;
           auto tables = readContainerIndexFile(bytes, key, &error, &damaged);
           if (!tables)
           {
             if (damaged)
             {
               warnings.push_back(fmt::format(
                 "The lit preview's container index {} is damaged ({}); the game's "
                 "containers are read again",
                 display,
                 error));
             }
             return std::shared_ptr<const GameContainerTables>{};
           }
           fromFile = true;
           fileBytes = std::uint64_t(bytes.size());
           auto shared = std::make_shared<const GameContainerTables>(std::move(*tables));
           const auto lock = std::scoped_lock{m_mutex};
           m_tables[key.base.native()] = shared;
           return shared;
         })
         | kdl::transform_error([&](const auto& e) {
             warnings.push_back(fmt::format(
               "The lit preview's container index {} cannot be read ({}); the game's "
               "containers are read again",
               display,
               e.msg));
             return std::shared_ptr<const GameContainerTables>{};
           })
         | kdl::value();
}

void GameContainerIndex::store(
  std::shared_ptr<const GameContainerTables> tables,
  double& writeSeconds,
  std::uint64_t& fileBytes,
  std::vector<std::string>& warnings)
{
  const auto base = tables->key.base;
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_tables[base.native()] = tables;
  }
  if (m_folder.empty())
  {
    return;
  }

  // atomically: a temporary file in the folder, then a rename over the index file
  const auto start = Clock::now();
  const auto bytes = writeContainerIndexFile(*tables);
  const auto file = filePath(base);
  auto temporary = file;
  temporary += fmt::format(
    ".{}-{}.tmp",
    hexOf(std::uint64_t(std::hash<std::thread::id>{}(std::this_thread::get_id()))),
    hexOf(std::uint64_t(Clock::now().time_since_epoch().count())));
  const auto failed = [&](const std::string& why) {
    auto ec = std::error_code{};
    std::filesystem::remove(temporary, ec);
    warnings.push_back(fmt::format(
      "The lit preview's container index {} cannot be written ({}); the next Start reads "
      "the game's containers again",
      pathToUtf8(file).value_or("file"),
      why));
  };
  auto ec = std::error_code{};
  std::filesystem::create_directories(m_folder, ec);
  if (ec)
  {
    failed(ec.message());
    return;
  }
  {
    auto stream = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), std::streamsize(bytes.size()));
    stream.close();
    if (!stream)
    {
      failed("the temporary file was not written");
      return;
    }
  }
  std::filesystem::rename(temporary, file, ec);
  if (ec)
  {
    failed(ec.message());
    return;
  }
  writeSeconds = secondsSince(start);
  fileBytes = std::uint64_t(bytes.size());
}

const std::filesystem::path& GameContainerIndex::folder() const
{
  return m_folder;
}

std::filesystem::path GameContainerIndex::filePath(
  const std::filesystem::path& base) const
{
  // one file per base folder: a key that moved replaces its file
  const auto& native = base.native();
  const auto hash = payloadHash(
    reinterpret_cast<const char*>(native.data()),
    native.size() * sizeof(std::filesystem::path::value_type));
  return m_folder / fmt::format("containers-{}.idx", hexOf(hash));
}

void GameContainerIndex::dropMemoryForTesting()
{
  const auto lock = std::scoped_lock{m_mutex};
  m_tables.clear();
}

std::size_t GameContainerIndex::memoryEntriesForTesting() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_tables.size();
}

void GameContainerIndex::setAfterContainerForTesting(
  std::function<void(const std::string&)> hook)
{
  m_afterContainerForTesting = std::move(hook);
}

const std::function<void(const std::string&)>& GameContainerIndex::
  afterContainerForTesting() const
{
  return m_afterContainerForTesting;
}

} // namespace tb::lit
