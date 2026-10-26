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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/gltfParser.cpp, idlib/gltfProperties.h,
// idlib/MapFile_gltf.cpp and renderer/Model_gltf.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2022-2023 Harrie van Ginneken
//   Copyright (C) 2012-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

// LP7.5 (C51 (3), D136): the glTF validator (ModelValidators.h, gltfFileProblem)

#include "ModelValidators.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{
namespace
{

// gltfParser.cpp:37-38
constexpr auto ChunkJson = std::uint32_t{0x4E4F534A};
constexpr auto ChunkBin = std::uint32_t{0x004E4942};

// the JSON's nesting the reader takes (its recursion), and the node tree's depth (the
// copied code's recursions over children: SetNodeParent, ProcessNode_r, GetAllMeshes,
// GetAnimationIds, ResolveNodeMatrix's walk to the root)
constexpr auto MaxJsonDepth = 64;
constexpr auto MaxNodeDepth = std::size_t{128};

/**
 * A JSON value as the validator reads it: RFC 8259's grammar, narrowed to what the
 * copied idLexer (LEXFL_ALLOWPATHNAMES | LEXFL_ALLOWMULTICHARLITERALS |
 * LEXFL_NOSTRINGESCAPECHARS, gltfParser.cpp:1155) tokenises the same way: strings
 * without a backslash (the lexer keeps no escape) or a control character, numbers with a
 * lower-case exponent only (Lexer.cpp:881-910 reads no 'E'), no byte order mark, no NUL
 * (the script's end). An object keeps its members in the file's order (the parser
 * appends what it reads in that order: a primitive's first attribute sizes its vertices,
 * MapFile_gltf.cpp:103-107).
 */
struct Json
{
  enum class Type
  {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object,
  };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  // a number written without a fraction or an exponent, and its value when it fits an
  // int (the parser's gltfItem_integer: idToken::GetIntValue, an int)
  bool integer = false;
  long long integerValue = 0;
  std::string text;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;
  // where the value starts in the JSON
  std::size_t at = 0;

  const Json* member(const std::string_view key) const
  {
    for (const auto& [name, value] : members)
    {
      if (name == key)
      {
        return &value;
      }
    }
    return nullptr;
  }
};

class JsonReader
{
public:
  explicit JsonReader(const std::string_view text)
    : m_text{text}
  {
  }

  std::optional<std::string> read(Json& root)
  {
    if (
      m_text.size() >= 3 && std::uint8_t(m_text[0]) == 0xEF
      && std::uint8_t(m_text[1]) == 0xBB && std::uint8_t(m_text[2]) == 0xBF)
    {
      return "the JSON starts with a byte order mark";
    }
    skipBlank();
    if (auto problem = value(root, 0))
    {
      return problem;
    }
    skipBlank();
    if (m_at != m_text.size())
    {
      return fmt::format(
        "the JSON at byte {}: more after the root object (the parser reads one, "
        "gltfParser.cpp:2013-2069)",
        m_at);
    }
    if (root.type != Json::Type::Object)
    {
      return "the JSON's root is not an object (gltfParser.cpp:2016 expects '{')";
    }
    return std::nullopt;
  }

private:
  std::string_view m_text;
  std::size_t m_at = 0;

  std::string where() const { return fmt::format("the JSON at byte {}", m_at); }

  void skipBlank()
  {
    while (
      m_at < m_text.size()
      && (m_text[m_at] == ' ' || m_text[m_at] == '\t' || m_text[m_at] == '\n' || m_text[m_at] == '\r'))
    {
      ++m_at;
    }
  }

  bool literal(const std::string_view word)
  {
    if (m_text.substr(m_at, word.size()) == word)
    {
      m_at += word.size();
      return true;
    }
    return false;
  }

  std::optional<std::string> value(Json& out, const int depth)
  {
    if (depth > MaxJsonDepth)
    {
      return fmt::format("{}: nested deeper than {}", where(), MaxJsonDepth);
    }
    skipBlank();
    out.at = m_at;
    if (m_at >= m_text.size())
    {
      return fmt::format("{}: the JSON ends early", where());
    }
    const auto c = m_text[m_at];
    if (c == '{')
    {
      return object(out, depth);
    }
    if (c == '[')
    {
      return array(out, depth);
    }
    if (c == '"')
    {
      out.type = Json::Type::String;
      return string(out.text);
    }
    if (c == '-' || (c >= '0' && c <= '9'))
    {
      return number(out);
    }
    if (literal("true"))
    {
      out.type = Json::Type::Boolean;
      out.boolean = true;
      return std::nullopt;
    }
    if (literal("false"))
    {
      out.type = Json::Type::Boolean;
      return std::nullopt;
    }
    if (literal("null"))
    {
      out.type = Json::Type::Null;
      return std::nullopt;
    }
    return fmt::format("{}: not a JSON value", where());
  }

  std::optional<std::string> object(Json& out, const int depth)
  {
    out.type = Json::Type::Object;
    ++m_at;
    skipBlank();
    if (m_at < m_text.size() && m_text[m_at] == '}')
    {
      ++m_at;
      return std::nullopt;
    }
    while (true)
    {
      skipBlank();
      if (m_at >= m_text.size() || m_text[m_at] != '"')
      {
        return fmt::format("{}: an object's key is not a string", where());
      }
      auto key = std::string{};
      if (auto problem = string(key))
      {
        return problem;
      }
      if (out.member(key))
      {
        return fmt::format(
          "{}: the key \"{}\" twice in one object (the parser would read it twice)",
          where(),
          key);
      }
      skipBlank();
      if (m_at >= m_text.size() || m_text[m_at] != ':')
      {
        return fmt::format("{}: no ':' after a key", where());
      }
      ++m_at;
      auto item = Json{};
      if (auto problem = value(item, depth + 1))
      {
        return problem;
      }
      out.members.emplace_back(std::move(key), std::move(item));
      skipBlank();
      if (m_at < m_text.size() && m_text[m_at] == ',')
      {
        ++m_at;
        continue;
      }
      if (m_at < m_text.size() && m_text[m_at] == '}')
      {
        ++m_at;
        return std::nullopt;
      }
      return fmt::format("{}: an object not closed", where());
    }
  }

  std::optional<std::string> array(Json& out, const int depth)
  {
    out.type = Json::Type::Array;
    ++m_at;
    skipBlank();
    if (m_at < m_text.size() && m_text[m_at] == ']')
    {
      ++m_at;
      return std::nullopt;
    }
    while (true)
    {
      auto item = Json{};
      if (auto problem = value(item, depth + 1))
      {
        return problem;
      }
      out.items.push_back(std::move(item));
      skipBlank();
      if (m_at < m_text.size() && m_text[m_at] == ',')
      {
        ++m_at;
        continue;
      }
      if (m_at < m_text.size() && m_text[m_at] == ']')
      {
        ++m_at;
        return std::nullopt;
      }
      return fmt::format("{}: an array not closed", where());
    }
  }

  std::optional<std::string> string(std::string& out)
  {
    ++m_at;
    const auto start = m_at;
    while (m_at < m_text.size() && m_text[m_at] != '"')
    {
      const auto c = std::uint8_t(m_text[m_at]);
      if (c == '\\')
      {
        return fmt::format(
          "{}: a string with a backslash (the parser's lexer keeps no escape, "
          "LEXFL_NOSTRINGESCAPECHARS)",
          where());
      }
      if (c < 0x20)
      {
        return fmt::format("{}: a control character in a string", where());
      }
      ++m_at;
    }
    if (m_at >= m_text.size())
    {
      return fmt::format("{}: a string not closed", where());
    }
    out.assign(m_text.substr(start, m_at - start));
    ++m_at;
    return std::nullopt;
  }

  std::optional<std::string> number(Json& out)
  {
    out.type = Json::Type::Number;
    const auto start = m_at;
    const auto digits = [&] {
      const auto from = m_at;
      while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
      {
        ++m_at;
      }
      return m_at - from;
    };
    if (m_text[m_at] == '-')
    {
      ++m_at;
    }
    const auto intStart = m_at;
    const auto intDigits = digits();
    if (intDigits == 0)
    {
      return fmt::format("{}: a '-' without digits", where());
    }
    if (intDigits > 1 && m_text[intStart] == '0')
    {
      return fmt::format(
        "{}: an integer with a leading zero (the lexer reads it as octal)", where());
    }
    auto whole = true;
    if (m_at < m_text.size() && m_text[m_at] == '.')
    {
      whole = false;
      ++m_at;
      if (digits() == 0)
      {
        return fmt::format("{}: a '.' without digits", where());
      }
    }
    if (m_at < m_text.size() && m_text[m_at] == 'E')
    {
      return fmt::format(
        "{}: an upper-case exponent (the lexer reads 'e' only, Lexer.cpp:881-910)",
        where());
    }
    if (m_at < m_text.size() && m_text[m_at] == 'e')
    {
      whole = false;
      ++m_at;
      if (m_at < m_text.size() && (m_text[m_at] == '-' || m_text[m_at] == '+'))
      {
        ++m_at;
      }
      if (digits() == 0)
      {
        return fmt::format("{}: an exponent without digits", where());
      }
    }
    const auto text = std::string{m_text.substr(start, m_at - start)};
    out.number = std::strtod(text.c_str(), nullptr);
    if (!std::isfinite(out.number))
    {
      return fmt::format("{}: a number out of range", where());
    }
    out.integer = whole && text.size() <= 11;
    if (out.integer)
    {
      out.integerValue = std::stoll(text);
      out.integer = out.integerValue >= INT_MIN && out.integerValue <= INT_MAX;
    }
    return std::nullopt;
  }
};

// gltfParser.cpp:133-142: a component type's bytes (GetComponentTypeEnum's sizeInBytes)
std::size_t componentBytes(const long long componentType)
{
  switch (componentType)
  {
  case 5120:
  case 5121:
    return 1;
  case 5122:
  case 5123:
    return 2;
  case 5125:
  case 5126:
    return 4;
  case 5130:
    return 8;
  default:
    return 0;
  }
}

std::size_t componentCount(const std::string_view type)
{
  return type == "SCALAR" ? 1
         : type == "VEC2" ? 2
         : type == "VEC3" ? 3
         : type == "VEC4" ? 4
         : type == "MAT2" ? 4
         : type == "MAT3" ? 9
         : type == "MAT4" ? 16
                          : 0;
}

/**
 * How ConvertFromMeshGltf reads one attribute (MapFile_gltf.cpp:93-358): the bytes it
 * reads per element (read), and its seek after each one when the view has a byteStride
 * (stride - seekBack): s_meshAttributeMap's elementSize times the component's bytes
 * (gltfParser.cpp:81-101), which is TANGENT's 3 against its 4 read; the indices read one
 * component and seek stride - typeSize (:52-61).
 */
struct AttributeRead
{
  // the semantic as the parser maps it, "" for one the conversion ignores
  std::string_view semantic;
  std::size_t components = 0;
  std::size_t elementSize = 0;
  // the component types the conversion takes for it (it reads others as raw bytes)
  std::vector<long long> componentTypes;
};

std::optional<AttributeRead> attributeRead(const std::string_view name)
{
  // gltfParser.cpp:104-117 GetAttributeEnum compares with Icmp: the parser maps any case;
  // the validator takes the specification's spelling only
  if (name == "POSITION")
  {
    return AttributeRead{"POSITION", 3, 3, {5126}};
  }
  if (name == "NORMAL")
  {
    return AttributeRead{"NORMAL", 3, 3, {5126}};
  }
  if (name == "TANGENT")
  {
    return AttributeRead{"TANGENT", 4, 3, {5126}};
  }
  if (name == "TEXCOORD_0")
  {
    return AttributeRead{"TEXCOORD_0", 2, 2, {5126}};
  }
  if (name == "WEIGHTS_0")
  {
    return AttributeRead{"WEIGHTS_0", 4, 4, {5126}};
  }
  if (name == "COLOR_0")
  {
    // :285-346: typeSize 4 (float), 2 (unsigned short) or else 1 (unsigned byte, raw)
    return AttributeRead{"COLOR_0", 4, 4, {5126, 5123, 5121}};
  }
  if (name == "JOINTS_0")
  {
    // :349-383: typeSize 2 or else 1
    return AttributeRead{"JOINTS_0", 4, 4, {5123, 5121}};
  }
  return std::nullopt;
}

// gltfParser.cpp:81-101: s_meshAttributeMap's names, compared with Icmp
bool mappedSemantic(const std::string_view name)
{
  static constexpr auto names = std::array<std::string_view, 17>{
    "POSITION",
    "NORMAL",
    "TANGENT",
    "TEXCOORD_0",
    "TEXCOORD_1",
    "TEXCOORD_2",
    "TEXCOORD_3",
    "TEXCOORD_4",
    "TEXCOORD_5",
    "TEXCOORD_6",
    "TEXCOORD_7",
    "COLOR_0",
    "COLOR_1",
    "COLOR_2",
    "COLOR_3",
    "WEIGHTS_0",
    "JOINTS_0"};
  return std::ranges::any_of(names, [&](const std::string_view known) {
    return known.size() == name.size()
           && std::equal(known.begin(), known.end(), name.begin(), [](char a, char b) {
                const auto lower = [](char c) {
                  return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                };
                return lower(a) == lower(b);
              });
  });
}

class GltfCheck
{
public:
  GltfCheck(
    const Json& root, const bool binary, const std::string_view bin, GltfFileFacts& facts)
    : m_root{root}
    , m_binary{binary}
    , m_bin{bin}
    , m_facts{facts}
  {
  }

  std::optional<std::string> run()
  {
    for (const auto& check : {
           &GltfCheck::topLevel,
           &GltfCheck::buffers,
           &GltfCheck::images,
           &GltfCheck::bufferViews,
           &GltfCheck::accessors,
           &GltfCheck::nodes,
           &GltfCheck::scenes,
           &GltfCheck::meshes,
           &GltfCheck::materials,
           &GltfCheck::skins,
           &GltfCheck::animations,
           &GltfCheck::extensions,
         })
    {
      if (auto problem = (this->*check)())
      {
        return problem;
      }
    }
    return std::nullopt;
  }

private:
  const Json& m_root;
  bool m_binary;
  std::string_view m_bin;
  GltfFileFacts& m_facts;

  // the root's arrays (empty when absent)
  const std::vector<Json>& list(const std::string_view key) const
  {
    static const auto none = std::vector<Json>{};
    const auto* value = m_root.member(key);
    return value && value->type == Json::Type::Array ? value->items : none;
  }

  std::size_t count(const std::string_view key) const { return list(key).size(); }

  // an optional integer field of an object: nullopt when absent; refused when not an
  // integer in 0..limit-1 (an index) or 0..INT_MAX (limit 0)
  static std::optional<std::string> index(
    const Json& object,
    const std::string_view key,
    const std::size_t limit,
    const std::string& what,
    std::optional<std::size_t>& out)
  {
    out.reset();
    const auto* value = object.member(key);
    if (!value)
    {
      return std::nullopt;
    }
    if (value->type != Json::Type::Number || !value->integer || value->integerValue < 0)
    {
      return fmt::format(
        "{}: \"{}\" is not a non-negative integer (the parser's gltfItem_integer reads "
        "its token's integer value)",
        what,
        key);
    }
    if (limit > 0 && std::size_t(value->integerValue) >= limit)
    {
      return fmt::format(
        "{}: \"{}\" {} names none of the {} there are (the copied code indexes it "
        "unchecked)",
        what,
        key,
        value->integerValue,
        limit);
    }
    out = std::size_t(value->integerValue);
    return std::nullopt;
  }

  // a required one
  static std::optional<std::string> requiredIndex(
    const Json& object,
    const std::string_view key,
    const std::size_t limit,
    const std::string& what,
    std::size_t& out,
    const std::string_view why)
  {
    auto found = std::optional<std::size_t>{};
    if (auto problem = index(object, key, limit, what, found))
    {
      return problem;
    }
    if (!found)
    {
      return fmt::format("{}: no \"{}\" ({})", what, key, why);
    }
    out = *found;
    return std::nullopt;
  }

  // an array of indices
  static std::optional<std::string> indexArray(
    const Json& object,
    const std::string_view key,
    const std::size_t limit,
    const std::string& what,
    std::vector<std::size_t>& out)
  {
    out.clear();
    const auto* value = object.member(key);
    if (!value)
    {
      return std::nullopt;
    }
    if (value->type != Json::Type::Array)
    {
      return fmt::format("{}: \"{}\" is not an array", what, key);
    }
    for (const auto& item : value->items)
    {
      if (item.type != Json::Type::Number || !item.integer || item.integerValue < 0)
      {
        return fmt::format(
          "{}: \"{}\" holds what is not a non-negative integer (gltfItem_integer_array, "
          "gltfParser.cpp:627-658)",
          what,
          key);
      }
      if (std::size_t(item.integerValue) >= limit)
      {
        return fmt::format(
          "{}: \"{}\" names {} of the {} there are (indexed unchecked)",
          what,
          key,
          item.integerValue,
          limit);
      }
      out.push_back(std::size_t(item.integerValue));
    }
    return std::nullopt;
  }

  // gltfParser.cpp:692-776: a vec2/3/4, quat or mat4 field, its count of numbers exact
  // (after the FatalError for another count the parser reads val[0..n-1] past the list)
  static std::optional<std::string> numbers(
    const Json& object,
    const std::string_view key,
    const std::size_t n,
    const std::string& what)
  {
    const auto* value = object.member(key);
    if (!value)
    {
      return std::nullopt;
    }
    if (
      value->type != Json::Type::Array || value->items.size() != n
      || !std::ranges::all_of(
        value->items, [](const Json& item) { return item.type == Json::Type::Number; }))
    {
      return fmt::format(
        "{}: \"{}\" is not {} numbers (the parser reads {} values past what it got, "
        "gltfParser.cpp:692-776)",
        what,
        key,
        n,
        n);
    }
    return std::nullopt;
  }

  static std::optional<std::string> isObjectList(
    const Json& root, const std::string_view key)
  {
    const auto* value = root.member(key);
    if (!value)
    {
      return std::nullopt;
    }
    if (
      value->type != Json::Type::Array
      || !std::ranges::all_of(
        value->items, [](const Json& item) { return item.type == Json::Type::Object; }))
    {
      return fmt::format("\"{}\" is not an array of objects", key);
    }
    return std::nullopt;
  }

  // ---- the checks, each named for the site it keeps away ----

  std::optional<std::string> topLevel()
  {
    // gltfParser.cpp:1722-1849: ResolveProp's names (Icmp); any other is the FatalError
    // "UnImplemented GLTF property" (:1846), after which its value is never read and the
    // parse ends early
    static constexpr auto known = std::array<std::string_view, 18>{
      "asset",
      "cameras",
      "scene",
      "scenes",
      "nodes",
      "materials",
      "meshes",
      "textures",
      "images",
      "accessors",
      "bufferViews",
      "samplers",
      "buffers",
      "animations",
      "skins",
      "extensions",
      "extensionsUsed",
      "extensionsRequired"};
    for (const auto& [key, value] : m_root.members)
    {
      if (std::ranges::find(known, key) == known.end())
      {
        return fmt::format(
          "a top-level \"{}\" (the parser's FatalError for an unknown property, "
          "gltfParser.cpp:1846, ends the parse early)",
          key);
      }
      if (
        key != "asset" && key != "scene" && key != "extensions"
        && value.type != Json::Type::Array)
      {
        return fmt::format("\"{}\" is not an array", key);
      }
    }
    for (const auto* key :
         {"cameras",
          "scenes",
          "nodes",
          "materials",
          "meshes",
          "textures",
          "images",
          "accessors",
          "bufferViews",
          "samplers",
          "buffers",
          "animations",
          "skins"})
    {
      if (auto problem = isObjectList(m_root, key))
      {
        return problem;
      }
    }
    m_facts.nodes = count("nodes");
    m_facts.meshes = count("meshes");
    m_facts.materials = count("materials");
    m_facts.animations = count("animations");
    m_facts.skins = count("skins");
    if (
      m_facts.nodes > MaxGltfNodes || m_facts.meshes > MaxGltfMeshes
      || m_facts.materials > MaxGltfMaterials || count("accessors") > MaxGltfAccessors
      || count("bufferViews") > MaxGltfAccessors || m_facts.animations > MaxGltfMeshes
      || m_facts.skins > MaxGltfMeshes)
    {
      return fmt::format(
        "more nodes, meshes, materials, accessors, views, animations or skins than the "
        "preview reads ({} nodes, {} meshes, {} accessors)",
        m_facts.nodes,
        m_facts.meshes,
        count("accessors"));
    }
    return std::nullopt;
  }

  std::optional<std::string> buffers()
  {
    const auto& all = list("buffers");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& buffer = all[i];
      const auto what = fmt::format("buffer {}", i);
      if (buffer.member("uri"))
      {
        // gltfParser.cpp:414-455: gltfItem_uri::Convert reads the uri through the file
        // system, relative to the game's search path (never the file's folder); a file
        // it cannot open (OpenFileRead's NULL) or a data: URI is a NULL dereference at
        // :432
        return fmt::format(
          "glTF with external buffers is not read by the preview ({} names a uri)", what);
      }
      if (!m_binary)
      {
        // a .gltf buffer without a uri has no chunk: GetData( index ) reads a NULL
        // chunk table (gltfParser.cpp:388-411, MapFile_gltf.cpp:51)
        return fmt::format("{} of a .gltf has no uri, so no data", what);
      }
      if (i > 0)
      {
        // a .glb has one BIN chunk: data[] holds it at 0 alone
        return fmt::format(
          "{}: a .glb's buffers past the first have no chunk (GetData( {} ) reads past "
          "the one BIN chunk, gltfParser.cpp:388-411)",
          what,
          i);
      }
      auto length = std::size_t{0};
      if (
        auto problem = requiredIndex(
          buffer, "byteLength", 0, what, length, "the specification requires it"))
      {
        return problem;
      }
      if (length > m_bin.size())
      {
        return fmt::format(
          "{}: byteLength {} is past its BIN chunk of {} bytes",
          what,
          length,
          m_bin.size());
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> images()
  {
    const auto& all = list("images");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto what = fmt::format("image {}", i);
      if (all[i].member("uri"))
      {
        // gltfParser.cpp:1396-1438: an image's uri goes through gltfItem_uri::Convert too
        // (the file system's read during the parse, :414-455)
        return fmt::format(
          "glTF with external buffers is not read by the preview ({} names a uri)", what);
      }
      auto view = std::optional<std::size_t>{};
      if (auto problem = index(all[i], "bufferView", count("bufferViews"), what, view))
      {
        return problem;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> bufferViews()
  {
    const auto& all = list("bufferViews");
    const auto& buffers = list("buffers");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& view = all[i];
      const auto what = fmt::format("bufferView {}", i);
      auto buffer = std::size_t{0};
      auto length = std::size_t{0};
      auto offset = std::optional<std::size_t>{};
      auto stride = std::optional<std::size_t>{};
      if (
        auto problem = requiredIndex(
          view, "buffer", buffers.size(), what, buffer, "GetData( -1 ), :404"))
      {
        return problem;
      }
      if (
        auto problem = requiredIndex(
          view, "byteLength", 0, what, length, "the specification requires it"))
      {
        return problem;
      }
      if (auto problem = index(view, "byteOffset", 0, what, offset))
      {
        return problem;
      }
      if (auto problem = index(view, "byteStride", 0, what, stride))
      {
        return problem;
      }
      if (stride && (*stride < 4 || *stride > 252 || *stride % 4 != 0))
      {
        return fmt::format(
          "{}: byteStride {} is not a multiple of 4 in 4..252", what, *stride);
      }
      const auto bufferLength =
        std::size_t(buffers[buffer].member("byteLength")->integerValue);
      if (offset.value_or(0) + length > bufferLength)
      {
        return fmt::format(
          "{}: bytes {}..{} are past its buffer of {}",
          what,
          offset.value_or(0),
          offset.value_or(0) + length,
          bufferLength);
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> accessors()
  {
    const auto& all = list("accessors");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& accessor = all[i];
      const auto what = fmt::format("accessor {}", i);
      auto view = std::optional<std::size_t>{};
      auto offset = std::optional<std::size_t>{};
      auto componentType = std::size_t{0};
      auto elements = std::size_t{0};
      if (auto problem = index(accessor, "bufferView", count("bufferViews"), what, view))
      {
        return problem;
      }
      if (auto problem = index(accessor, "byteOffset", 0, what, offset))
      {
        return problem;
      }
      if (
        auto problem = requiredIndex(
          accessor,
          "componentType",
          0,
          what,
          componentType,
          "GetComponentTypeEnum leaves typeSize uninitialised, gltfParser.cpp:1472"))
      {
        return problem;
      }
      if (componentBytes(static_cast<long long>(componentType)) == 0)
      {
        return fmt::format(
          "{}: componentType {} is none the parser maps (typeSize uninitialised, "
          "gltfParser.cpp:144-158, :1472)",
          what,
          componentType);
      }
      if (
        auto problem = requiredIndex(
          accessor, "count", 0, what, elements, "the specification requires it"))
      {
        return problem;
      }
      const auto* type = accessor.member("type");
      if (!type || type->type != Json::Type::String || componentCount(type->text) == 0)
      {
        return fmt::format("{}: no type of SCALAR, VEC2-4 or MAT2-4", what);
      }
      if (const auto* normalized = accessor.member("normalized");
          normalized && normalized->type != Json::Type::Boolean)
      {
        return fmt::format(
          "{}: \"normalized\" is not true or false (gltfParser.h:220's FatalError)",
          what);
      }
      for (const auto* key : {"min", "max"})
      {
        if (
          const auto* bound = accessor.member(key);
          bound
          && (bound->type != Json::Type::Array || !std::ranges::all_of(bound->items, [](const Json& item) {
                return item.type == Json::Type::Number;
              })))
        {
          return fmt::format("{}: \"{}\" is not an array of numbers", what, key);
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> nodes()
  {
    const auto& all = list("nodes");
    auto parents = std::vector<std::optional<std::size_t>>(all.size());
    auto children = std::vector<std::vector<std::size_t>>(all.size());
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& node = all[i];
      const auto what = fmt::format("node {}", i);
      auto found = std::optional<std::size_t>{};
      if (auto problem = index(node, "mesh", count("meshes"), what, found))
      {
        return problem;
      }
      if (auto problem = index(node, "skin", count("skins"), what, found))
      {
        return problem;
      }
      if (auto problem = index(node, "camera", count("cameras"), what, found))
      {
        return problem;
      }
      if (auto problem = indexArray(node, "children", all.size(), what, children[i]))
      {
        return problem;
      }
      for (const auto& [key, n] : std::array<std::pair<const char*, std::size_t>, 4>{
             {{"matrix", 16}, {"rotation", 4}, {"scale", 3}, {"translation", 3}}})
      {
        if (auto problem = numbers(node, key, n, what))
        {
          return problem;
        }
      }
      for (const auto child : children[i])
      {
        if (parents[child])
        {
          return fmt::format(
            "node {} is a child of nodes {} and {} (not a tree)",
            child,
            *parents[child],
            i);
        }
        parents[child] = i;
      }
    }
    // the tree: no cycle, and depths the copied recursions take (every node is a root or
    // has one parent, so a node that no root reaches is on a cycle)
    auto depth = std::vector<std::size_t>(all.size(), 0);
    auto reached = std::size_t{0};
    auto stack = std::vector<std::size_t>{};
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      if (parents[i])
      {
        continue;
      }
      depth[i] = 1;
      stack.push_back(i);
      while (!stack.empty())
      {
        const auto at = stack.back();
        stack.pop_back();
        ++reached;
        if (depth[at] > MaxNodeDepth)
        {
          return fmt::format(
            "node {} is {} deep, more than the preview's {} (the copied recursions over "
            "children)",
            at,
            depth[at],
            MaxNodeDepth);
        }
        for (const auto child : children[at])
        {
          depth[child] = depth[at] + 1;
          stack.push_back(child);
        }
      }
    }
    if (reached != all.size())
    {
      return "the nodes' children make a cycle (SetNodeParent, gltfParser.cpp:2188-2195, "
             "and ProcessNode_r, Model_gltf.cpp:132-135, recurse without end)";
    }
    return std::nullopt;
  }

  std::optional<std::string> scenes()
  {
    const auto& all = list("scenes");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      auto roots = std::vector<std::size_t>{};
      if (
        auto problem =
          indexArray(all[i], "nodes", count("nodes"), fmt::format("scene {}", i), roots))
      {
        return problem;
      }
    }
    // Model_gltf.cpp:523-527: InitFromFile reads SceneList()[ DefaultScene() ], whose
    // value gltfData's constructor leaves uninitialised when the file has no "scene"
    const auto* scene = m_root.member("scene");
    if (!scene)
    {
      return "no \"scene\" (InitFromFile reads SceneList()[ DefaultScene() ] "
             "uninitialised, Model_gltf.cpp:523-527)";
    }
    if (
      scene->type != Json::Type::Number || !scene->integer || scene->integerValue < 0
      || std::size_t(scene->integerValue) >= all.size())
    {
      return fmt::format(
        "\"scene\" names none of the {} scenes (Model_gltf.cpp:526)", all.size());
    }
    return std::nullopt;
  }

  // the engine's read of one accessor's elements inside its view (MapFile_gltf.cpp:51-61,
  // :103-358): the reads stay within the view, and so within the BIN chunk
  std::optional<std::string> window(
    const std::size_t accessorIndex,
    const std::size_t read,
    const std::size_t seekBack,
    const std::string& what,
    std::size_t& viewStart,
    std::size_t& step) const
  {
    const auto& accessor = list("accessors")[accessorIndex];
    if (accessor.member("sparse"))
    {
      return fmt::format(
        "{}: accessor {} is sparse (the conversion reads its base values alone)",
        what,
        accessorIndex);
    }
    const auto* viewIndex = accessor.member("bufferView");
    if (!viewIndex)
    {
      return fmt::format(
        "{}: accessor {} has no bufferView (BufferViewList()[ -1 ], "
        "MapFile_gltf.cpp:39, :99)",
        what,
        accessorIndex);
    }
    const auto& view = list("bufferViews")[std::size_t(viewIndex->integerValue)];
    const auto viewLength = std::size_t(view.member("byteLength")->integerValue);
    const auto* viewOffset = view.member("byteOffset");
    const auto* accessorOffset = accessor.member("byteOffset");
    const auto* strideValue = view.member("byteStride");
    const auto stride = strideValue ? std::size_t(strideValue->integerValue) : 0;
    const auto offset = accessorOffset ? std::size_t(accessorOffset->integerValue) : 0;
    const auto elements = std::size_t(accessor.member("count")->integerValue);
    if (stride != 0 && stride < read)
    {
      return fmt::format(
        "{}: accessor {}'s view's byteStride {} is below its elements' {} bytes",
        what,
        accessorIndex,
        stride,
        read);
    }
    step = stride == 0 ? read : stride - seekBack + read;
    const auto extent = elements == 0 ? 0 : (elements - 1) * step + read;
    if (offset + extent > viewLength)
    {
      return fmt::format(
        "{}: accessor {} reads bytes {}..{} of its view of {} (past it)",
        what,
        accessorIndex,
        offset,
        offset + extent,
        viewLength);
    }
    viewStart = (viewOffset ? std::size_t(viewOffset->integerValue) : 0) + offset;
    return std::nullopt;
  }

  std::optional<std::string> meshes()
  {
    const auto& all = list("meshes");
    auto primitives = std::size_t{0};
    for (std::size_t m = 0; m < all.size(); ++m)
    {
      const auto* list = all[m].member("primitives");
      if (!list)
      {
        continue;
      }
      if (
        list->type != Json::Type::Array
        || !std::ranges::all_of(
          list->items, [](const Json& item) { return item.type == Json::Type::Object; }))
      {
        return fmt::format("mesh {}: \"primitives\" is not an array of objects", m);
      }
      for (std::size_t p = 0; p < list->items.size(); ++p)
      {
        if (++primitives > MaxGltfPrimitives)
        {
          return fmt::format("more than {} primitives", MaxGltfPrimitives);
        }
        if (
          auto problem =
            primitive(list->items[p], fmt::format("mesh {} primitive {}", m, p)))
        {
          return problem;
        }
      }
    }
    m_facts.primitives = primitives;
    return std::nullopt;
  }

  std::optional<std::string> primitive(const Json& primitive, const std::string& what)
  {
    const auto& accessors = list("accessors");
    auto material = std::optional<std::size_t>{};
    if (auto problem = index(primitive, "material", count("materials"), what, material))
    {
      return problem;
    }
    auto mode = std::optional<std::size_t>{};
    if (auto problem = index(primitive, "mode", 0, what, mode))
    {
      return problem;
    }
    if (mode && *mode != 4)
    {
      // ConvertFromMeshGltf ignores the mode and reads every three indices as a triangle
      return fmt::format(
        "{}: mode {} is not triangles (the conversion reads every primitive as "
        "triangles, MapFile_gltf.cpp:66-82)",
        what,
        *mode);
    }
    const auto* attributes = primitive.member("attributes");
    if (
      !attributes || attributes->type != Json::Type::Object
      || attributes->members.empty())
    {
      return fmt::format("{}: no attributes", what);
    }
    auto vertices = std::optional<std::size_t>{};
    auto hasPosition = false;
    for (const auto& [name, value] : attributes->members)
    {
      if (
        value.type != Json::Type::Number || !value.integer || value.integerValue < 0
        || std::size_t(value.integerValue) >= accessors.size())
      {
        return fmt::format(
          "{}: attribute {} names no accessor (indexed unchecked, MapFile_gltf.cpp:96)",
          what,
          name);
      }
      const auto accessorIndex = std::size_t(value.integerValue);
      const auto& accessor = accessors[accessorIndex];
      const auto elements = std::size_t(accessor.member("count")->integerValue);
      // :103-107: the vertices are sized by the first attribute's count; a later one with
      // more writes past them
      if (vertices && *vertices != elements)
      {
        return fmt::format(
          "{}: attribute {} has {} elements, the first attribute {} (verts sized by the "
          "first, MapFile_gltf.cpp:103-107)",
          what,
          name,
          elements,
          *vertices);
      }
      vertices = elements;
      const auto* viewIndex = accessor.member("bufferView");
      if (!viewIndex)
      {
        return fmt::format(
          "{}: attribute {}'s accessor {} has no bufferView (BufferViewList()[ -1 ], "
          "MapFile_gltf.cpp:99)",
          what,
          name,
          accessorIndex);
      }
      const auto read = attributeRead(name);
      if (!read && mappedSemantic(name))
      {
        // GetAttributeEnum maps any case (Icmp, gltfParser.cpp:104-117): the validator
        // takes the specification's upper case only
        return fmt::format(
          "{}: attribute {} is a semantic in another case than the specification's",
          what,
          name);
      }
      if (!read)
      {
        // the conversion builds the attribute's window and reads nothing of it
        continue;
      }
      hasPosition = hasPosition || read->semantic == "POSITION";
      const auto componentType = accessor.member("componentType")->integerValue;
      const auto type = accessor.member("type")->text;
      if (
        std::ranges::find(read->componentTypes, componentType)
          == read->componentTypes.end()
        || componentCount(type) != read->components)
      {
        return fmt::format(
          "{}: attribute {} is {} of component type {}, which the conversion misreads "
          "(MapFile_gltf.cpp:93-358 reads it as {} components of the types {})",
          what,
          name,
          type,
          componentType,
          read->components,
          fmt::join(read->componentTypes, ", "));
      }
      const auto bytes = componentBytes(componentType);
      auto start = std::size_t{0};
      auto step = std::size_t{0};
      if (
        auto problem = window(
          accessorIndex,
          read->components * bytes,
          read->elementSize * bytes,
          fmt::format("{} attribute {}", what, name),
          start,
          step))
      {
        return problem;
      }
    }
    if (!hasPosition)
    {
      return fmt::format(
        "{}: no POSITION (the vertices' positions would be uninitialised heap, "
        "MapFile_gltf.cpp:103-107)",
        what);
    }
    if (*vertices > MaxGltfPrimitiveVerts)
    {
      return fmt::format(
        "{}: {} vertices, more than the {} the preview takes (16-bit triIndex_t, "
        "Model_gltf.cpp:99-108)",
        what,
        *vertices,
        MaxGltfPrimitiveVerts);
    }
    m_facts.vertices += *vertices;

    // the indices (MapFile_gltf.cpp:37-82)
    auto indicesIndex = std::size_t{0};
    if (
      auto problem = requiredIndex(
        primitive,
        "indices",
        accessors.size(),
        what,
        indicesIndex,
        "AccessorList()[ -1 ], MapFile_gltf.cpp:39: the preview reads indexed "
        "primitives only"))
    {
      return problem;
    }
    const auto& indices = accessors[indicesIndex];
    const auto componentType = indices.member("componentType")->integerValue;
    if (
      (componentType != 5121 && componentType != 5123 && componentType != 5125)
      || indices.member("type")->text != "SCALAR")
    {
      return fmt::format(
        "{}: the indices are {} of component type {}, not an unsigned byte, short or int "
        "scalar (the conversion reads their bytes into a uint, MapFile_gltf.cpp:53-60)",
        what,
        indices.member("type")->text,
        componentType);
    }
    const auto count = std::size_t(indices.member("count")->integerValue);
    if (count % 3 != 0)
    {
      return fmt::format(
        "{}: {} indices, not a multiple of 3 (the conversion reads indices[ i + 2 ] and "
        "writes polygons[] past their ends, MapFile_gltf.cpp:66-82)",
        what,
        count);
    }
    const auto bytes = componentBytes(componentType);
    auto start = std::size_t{0};
    auto step = std::size_t{0};
    if (
      auto problem =
        window(indicesIndex, bytes, bytes, fmt::format("{} indices", what), start, step))
    {
      return problem;
    }
    // every index names a vertex (R_RangeCheckIndexes' Error, tr_trisurf.cpp:572-589, in
    // FinishSurfaces)
    for (std::size_t i = 0; i < count; ++i)
    {
      const auto at = start + i * step;
      auto value = std::uint32_t{0};
      std::memcpy(&value, m_bin.data() + at, bytes);
      if (value >= *vertices)
      {
        return fmt::format(
          "{}: index {} is {}, past its {} vertices (R_RangeCheckIndexes, "
          "tr_trisurf.cpp:572-589)",
          what,
          i,
          value,
          *vertices);
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> materials()
  {
    const auto& all = list("materials");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& material = all[i];
      const auto what = fmt::format("material {}", i);
      if (const auto* name = material.member("name"))
      {
        if (name->type != Json::Type::String)
        {
          return fmt::format("{}: its name is not a string", what);
        }
        if (name->text.size() >= MaxGltfMaterialName)
        {
          return fmt::format(
            "{}: its name is {} characters, not under {} (FindMaterial's canonical "
            "name)",
            what,
            name->text.size(),
            MaxGltfMaterialName);
        }
      }
      if (auto problem = numbers(material, "emissiveFactor", 3, what))
      {
        return problem;
      }
      if (const auto* pbr = material.member("pbrMetallicRoughness");
          pbr && pbr->type == Json::Type::Object)
      {
        if (auto problem = numbers(*pbr, "baseColorFactor", 4, what))
        {
          return problem;
        }
        if (auto problem = textureInfos(*pbr, what))
        {
          return problem;
        }
      }
      if (auto problem = textureInfos(material, what))
      {
        return problem;
      }
      if (const auto* extensions = material.member("extensions");
          extensions && extensions->type == Json::Type::Object)
      {
        if (const auto* gloss = extensions->member("KHR_materials_pbrSpecularGlossiness");
            gloss && gloss->type == Json::Type::Object)
        {
          if (auto problem = numbers(*gloss, "diffuseFactor", 4, what))
          {
            return problem;
          }
          if (auto problem = numbers(*gloss, "specularFactor", 3, what))
          {
            return problem;
          }
          if (auto problem = textureInfos(*gloss, what))
          {
            return problem;
          }
        }
      }
    }
    return std::nullopt;
  }

  // a texture info's KHR_texture_transform offset and scale (gltfParser.cpp:975-996)
  static std::optional<std::string> textureInfos(
    const Json& object, const std::string& what)
  {
    for (const auto& [key, value] : object.members)
    {
      if (value.type != Json::Type::Object)
      {
        continue;
      }
      const auto* extensions = value.member("extensions");
      if (!extensions || extensions->type != Json::Type::Object)
      {
        continue;
      }
      if (const auto* transform = extensions->member("KHR_texture_transform");
          transform && transform->type == Json::Type::Object)
      {
        for (const auto* field : {"offset", "scale"})
        {
          if (auto problem = numbers(*transform, field, 2, what + " " + key))
          {
            return problem;
          }
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> skins()
  {
    const auto& all = list("skins");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& skin = all[i];
      const auto what = fmt::format("skin {}", i);
      auto joints = std::vector<std::size_t>{};
      auto skeleton = std::optional<std::size_t>{};
      auto matrices = std::optional<std::size_t>{};
      if (auto problem = indexArray(skin, "joints", count("nodes"), what, joints))
      {
        return problem;
      }
      if (auto problem = index(skin, "skeleton", count("nodes"), what, skeleton))
      {
        return problem;
      }
      if (
        auto problem =
          index(skin, "inverseBindMatrices", count("accessors"), what, matrices))
      {
        return problem;
      }
      if (const auto* name = skin.member("name"))
      {
        // gltfParser.cpp:2160-2167: a skin without a skeleton looks its name up, and
        // GetNode asserts the name's first character (gltfProperties.h:925)
        if (name->type != Json::Type::String || (!skeleton && name->text.empty()))
        {
          return fmt::format(
            "{}: an empty or non-string name without a skeleton (GetNode's assert, "
            "gltfProperties.h:925)",
            what);
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> animations()
  {
    const auto& all = list("animations");
    for (std::size_t i = 0; i < all.size(); ++i)
    {
      const auto& animation = all[i];
      const auto what = fmt::format("animation {}", i);
      const auto* samplers = animation.member("samplers");
      const auto* channels = animation.member("channels");
      const auto samplerCount =
        samplers && samplers->type == Json::Type::Array ? samplers->items.size() : 0;
      if (samplers)
      {
        if (samplers->type != Json::Type::Array)
        {
          return fmt::format("{}: \"samplers\" is not an array", what);
        }
        for (const auto& sampler : samplers->items)
        {
          if (sampler.type != Json::Type::Object)
          {
            return fmt::format("{}: a sampler is not an object", what);
          }
          auto found = std::optional<std::size_t>{};
          for (const auto* key : {"input", "output"})
          {
            if (auto problem = index(sampler, key, count("accessors"), what, found))
            {
              return problem;
            }
          }
        }
      }
      if (channels)
      {
        if (channels->type != Json::Type::Array)
        {
          return fmt::format("{}: \"channels\" is not an array", what);
        }
        for (const auto& channel : channels->items)
        {
          if (channel.type != Json::Type::Object)
          {
            return fmt::format("{}: a channel is not an object", what);
          }
          auto found = std::optional<std::size_t>{};
          if (auto problem = index(channel, "sampler", samplerCount, what, found))
          {
            return problem;
          }
          if (const auto* target = channel.member("target");
              target && target->type == Json::Type::Object)
          {
            // GetAnimationIds reads nodes[ channel->target.node ] (gltfProperties.h)
            if (auto problem = index(*target, "node", count("nodes"), what, found))
            {
              return problem;
            }
          }
        }
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> extensions()
  {
    // gltfParser.cpp:1043-1093: KHR_lights_punctual's lights, their colour a vec3
    const auto* extensions = m_root.member("extensions");
    if (!extensions)
    {
      return std::nullopt;
    }
    if (extensions->type != Json::Type::Object)
    {
      return "\"extensions\" is not an object";
    }
    if (const auto* lights = extensions->member("KHR_lights_punctual");
        lights && lights->type == Json::Type::Object)
    {
      if (const auto* list = lights->member("lights");
          list && list->type == Json::Type::Array)
      {
        for (const auto& light : list->items)
        {
          if (light.type == Json::Type::Object)
          {
            if (auto problem = numbers(light, "color", 3, "a KHR_lights_punctual light"))
            {
              return problem;
            }
          }
        }
      }
    }
    return std::nullopt;
  }
};

std::uint32_t little32(const std::string_view bytes, const std::size_t at)
{
  auto value = std::uint32_t{0};
  std::memcpy(&value, bytes.data() + at, 4);
  return value;
}

} // namespace

std::optional<std::string> gltfFileProblem(
  const std::string_view bytes, const bool binary, GltfFileFacts* facts)
{
  auto local = GltfFileFacts{};
  auto& found = facts ? *facts : local;
  found = GltfFileFacts{};
  if (bytes.size() >= std::size_t(INT_MAX))
  {
    return "the file is 2 GB or larger";
  }
  auto json = bytes;
  auto bin = std::string_view{};
  if (binary)
  {
    // gltfParser.cpp:1929-2011 loadGLB: the 12-byte header, then chunks
    if (bytes.size() < 20)
    {
      return fmt::format(
        "{} bytes, under the 20 of a .glb (gltfParser.cpp:1939's FatalError)",
        bytes.size());
    }
    // :1945-1956: the magic, compared with Icmp; the validator takes "glTF" only
    if (bytes.substr(0, 4) != std::string_view{"glTF"})
    {
      return "the magic is not glTF (gltfParser.cpp:1955's Error)";
    }
    if (little32(bytes, 4) != 2)
    {
      return fmt::format(
        "version {}, not 2 (the parser reads it and goes on; the preview takes 2 only)",
        little32(bytes, 4));
    }
    const auto length = std::size_t(little32(bytes, 8));
    if (length != bytes.size())
    {
      // :1964: the chunk loop runs on the declared length, unsigned: a longer one reads
      // short chunks and underflows, a shorter one leaves bytes unread
      return fmt::format(
        "the header's length {} is not the file's {} bytes (gltfParser.cpp:1964-2005)",
        length,
        bytes.size());
    }
    auto at = std::size_t{12};
    auto chunks = 0;
    while (at < length)
    {
      if (length - at < 8)
      {
        return fmt::format("byte {}: a chunk header cut short", at);
      }
      const auto chunkLength = std::size_t(little32(bytes, at));
      const auto chunkType = little32(bytes, at + 4);
      if (chunkLength > length - at - 8)
      {
        return fmt::format(
          "byte {}: a chunk of {} bytes past the file (gltfParser.cpp:1984-1987's "
          "FatalError on the short read)",
          at,
          chunkLength);
      }
      if (chunkLength % 4 != 0)
      {
        return fmt::format(
          "byte {}: a chunk of {} bytes, not padded to 4", at, chunkLength);
      }
      const auto data = bytes.substr(at + 8, chunkLength);
      if (chunks == 0)
      {
        if (chunkType != ChunkJson)
        {
          return "the first chunk is not JSON (gltfParser.cpp:1994-1997's FatalError)";
        }
        json = data;
      }
      else if (chunks == 1)
      {
        if (chunkType != ChunkBin)
        {
          return fmt::format(
            "byte {}: the second chunk is not BIN (the parser would take it as one)", at);
        }
        bin = data;
      }
      else
      {
        // :2002-2005: bytes after the second chunk are the FatalError "corrupt glb
        // file", after which the loop reads on (past the 32 chunks of data[], :400)
        return fmt::format(
          "byte {}: a third chunk (gltfParser.cpp:2002-2005's FatalError, then the "
          "chunk table's 32 entries)",
          at);
      }
      ++chunks;
      at += 8 + chunkLength;
    }
    if (chunks == 0)
    {
      return "no JSON chunk";
    }
    found.binLength = bin.size();
  }
  if (json.find('\0') != std::string_view::npos)
  {
    return "the JSON holds a NUL byte (the lexer's script ends there)";
  }
  auto root = Json{};
  if (auto problem = JsonReader{json}.read(root))
  {
    return problem;
  }
  return GltfCheck{root, binary, bin, found}.run();
}

} // namespace tb::lit
