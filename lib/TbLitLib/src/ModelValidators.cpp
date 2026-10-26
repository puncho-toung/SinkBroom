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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model_obj.cpp, renderer/Model_ase.cpp,
// renderer/Model.cpp and idlib/Lexer.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "ModelValidators.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace tb::lit
{
namespace
{

// the bytes the lexer's ReadWhiteSpace and ReadRestOfLine take for white space (<= ' '),
// NUL excepted (it ends the script, so the validator refuses it)
bool isBlank(const char c)
{
  return c > 0 && c <= ' ';
}

bool isDigit(const char c)
{
  return c >= '0' && c <= '9';
}

std::vector<std::string_view> tokensOf(const std::string_view line)
{
  auto tokens = std::vector<std::string_view>{};
  auto i = std::size_t{0};
  while (i < line.size())
  {
    while (i < line.size() && isBlank(line[i]))
    {
      ++i;
    }
    const auto start = i;
    while (i < line.size() && !isBlank(line[i]))
    {
      ++i;
    }
    if (i > start)
    {
      tokens.push_back(line.substr(start, i - start));
    }
  }
  return tokens;
}

// the digits from at, at least one; at moves past them
bool digits(const std::string_view text, std::size_t& at)
{
  const auto start = at;
  while (at < text.size() && isDigit(text[at]))
  {
    ++at;
  }
  return at > start;
}

/**
 * One number as ParseFloat reads it with the OBJ lexer's flags 0 (Lexer.cpp:1678-1718):
 * an optional '-' (ParseFloat's punctuation case), then a token ReadNumber reads as one
 * decimal TT_NUMBER (:804-960): digits with no leading 0 before a digit or 'e' (:818: a
 * 0 not followed by '.' starts an octal, hex or binary number), at most one '.', or a
 * '.' first with digits after it; an exponent 'e' (lowercase, :881-910) with an optional
 * sign and digits. Nothing may follow it in the token.
 */
bool isLexerNumber(std::string_view token)
{
  if (!token.empty() && token[0] == '-')
  {
    token.remove_prefix(1);
  }
  if (token.empty())
  {
    return false;
  }
  auto at = std::size_t{0};
  if (token[0] == '.')
  {
    ++at;
    if (!digits(token, at))
    {
      return false;
    }
  }
  else
  {
    if (token[0] == '0' && token.size() > 1 && token[1] != '.')
    {
      return false;
    }
    if (!digits(token, at))
    {
      return false;
    }
    if (at < token.size() && token[at] == '.')
    {
      ++at;
      while (at < token.size() && isDigit(token[at]))
      {
        ++at;
      }
    }
  }
  if (at < token.size() && token[at] == 'e')
  {
    ++at;
    if (at < token.size() && (token[at] == '-' || token[at] == '+'))
    {
      ++at;
    }
    if (!digits(token, at))
    {
      return false;
    }
  }
  return at == token.size();
}

// a positive decimal integer of at most 9 digits (sscanf's %d without overflow)
std::optional<std::size_t> positiveIndex(const std::string_view text)
{
  if (text.empty() || text.size() > 9 || text[0] == '0')
  {
    return std::nullopt;
  }
  auto value = std::size_t{0};
  for (const auto c : text)
  {
    if (!isDigit(c))
    {
      return std::nullopt;
    }
    value = value * 10 + std::size_t(c - '0');
  }
  return value;
}

// the rest of the line after the first token, as ReadRestOfLine leaves it (white space
// as ' ', stripped of ' ' at both ends, Lexer.cpp:1598-1628)
std::string restOfLine(const std::string_view line, const std::string_view first)
{
  const auto from = std::size_t(first.data() + first.size() - line.data());
  auto rest = std::string{};
  for (auto i = from; i < line.size(); ++i)
  {
    rest.push_back(line[i] <= ' ' ? ' ' : line[i]);
  }
  const auto begin = rest.find_first_not_of(' ');
  if (begin == std::string::npos)
  {
    return {};
  }
  const auto end = rest.find_last_not_of(' ');
  return rest.substr(begin, end - begin + 1);
}

} // namespace

std::optional<std::string> objFileProblem(const std::string_view bytes)
{
  if (bytes.empty())
  {
    return "the file is empty";
  }
  if (bytes.size() >= std::size_t(INT_MAX))
  {
    return "the file is 2 GB or larger";
  }
  if (bytes.find('\0') != std::string_view::npos)
  {
    return "the file holds a NUL byte";
  }

  auto counts = std::array<std::size_t, 3>{0, 0, 0}; // v, vt, vn
  auto corners = std::size_t{0};
  auto lineNumber = 0;
  auto at = std::size_t{0};
  while (at < bytes.size())
  {
    auto end = bytes.find('\n', at);
    if (end == std::string_view::npos)
    {
      end = bytes.size();
    }
    const auto line = bytes.substr(at, end - at);
    at = end + 1;
    ++lineNumber;

    const auto tokens = tokensOf(line);
    if (tokens.empty())
    {
      continue;
    }
    const auto first = tokens[0];
    const auto problem = [&](const std::string& what) {
      return fmt::format("line {}: {}", lineNumber, what);
    };

    // Model_obj.cpp:108-113: "#" skips the line; "##" is another punctuation, a token
    // :211 refuses
    if (first[0] == '#')
    {
      if (first.size() > 1 && first[1] == '#')
      {
        return problem("an unexpected token '##'");
      }
      continue;
    }
    if (first == "mtllib" || first == "s" || first == "l" || first == "o" || first == "g")
    {
      // :114-158: the rest of the line is read and dropped
      continue;
    }
    if (first == "usemtl")
    {
      // :159-165
      if (restOfLine(line, first).size() >= 256)
      {
        return problem("a usemtl name of 256 or more characters");
      }
      continue;
    }
    if (first == "v" || first == "vn" || first == "vt")
    {
      // :88-107, :166-173: three numbers (two for vt), ParseFloat each
      const auto wanted = first == "vt" ? std::size_t{2} : std::size_t{3};
      if (tokens.size() != wanted + 1)
      {
        return problem(
          fmt::format("'{}' takes {} numbers, not {}", first, wanted, tokens.size() - 1));
      }
      for (auto i = std::size_t{1}; i < tokens.size(); ++i)
      {
        if (!isLexerNumber(tokens[i]))
        {
          return problem(fmt::format(
            "'{}' is not a number the engine's lexer reads", std::string{tokens[i]}));
        }
      }
      ++counts[first == "v" ? 0 : first == "vt" ? 1 : 2];
      continue;
    }
    if (first == "f")
    {
      // :174-207: sscanf( "%d/%d/%d %d/%d/%d %d/%d/%d" ) must match nine, and each index
      // must be in the lists read so far
      if (tokens.size() != 4)
      {
        return problem(fmt::format(
          "a face takes three corners, not {} (quads and n-gons are not read)",
          tokens.size() - 1));
      }
      for (auto i = std::size_t{1}; i < tokens.size(); ++i)
      {
        const auto corner = tokens[i];
        const auto slash1 = corner.find('/');
        const auto slash2 =
          slash1 == std::string_view::npos ? slash1 : corner.find('/', slash1 + 1);
        if (
          slash2 == std::string_view::npos
          || corner.find('/', slash2 + 1) != std::string_view::npos)
        {
          return problem(
            fmt::format("the corner '{}' is not v/vt/vn", std::string{corner}));
        }
        const auto parts = std::array<std::string_view, 3>{
          corner.substr(0, slash1),
          corner.substr(slash1 + 1, slash2 - slash1 - 1),
          corner.substr(slash2 + 1)};
        for (std::size_t k = 0; k < 3; ++k)
        {
          const auto index = positiveIndex(parts[k]);
          if (!index)
          {
            return problem(fmt::format(
              "the corner '{}' holds an index that is not a positive number",
              std::string{corner}));
          }
          if (*index > counts[k])
          {
            return problem(fmt::format(
              "the corner '{}' names a {} past the {} read before it",
              std::string{corner},
              k == 0   ? "position"
              : k == 1 ? "texture coordinate"
                       : "normal",
              counts[k]));
          }
        }
      }
      corners += 3;
      if (corners > MaxObjCorners)
      {
        return problem(fmt::format(
          "more than {} face corners (the engine's 16-bit indexes)", MaxObjCorners));
      }
      continue;
    }
    return problem(fmt::format("an unexpected token '{}'", std::string{first}));
  }
  return std::nullopt;
}

namespace
{

// the C library's atof as the copied ASE reader calls it (Model_ase.cpp), held to a
// strict decimal subset: an optional sign, digits with at most one '.', an optional
// exponent; finite and at most 1e30 in size (so that the conversion's bounds and
// idVectorSubset's boxes stay finite)
std::optional<double> aseNumber(const std::string_view token)
{
  if (token.empty() || token.size() > 64)
  {
    return std::nullopt;
  }
  auto at = std::size_t{0};
  if (token[at] == '-' || token[at] == '+')
  {
    ++at;
  }
  const auto mantissa = at;
  auto digitsSeen = false;
  while (at < token.size() && isDigit(token[at]))
  {
    ++at;
    digitsSeen = true;
  }
  if (at < token.size() && token[at] == '.')
  {
    ++at;
    while (at < token.size() && isDigit(token[at]))
    {
      ++at;
      digitsSeen = true;
    }
  }
  if (!digitsSeen)
  {
    return std::nullopt;
  }
  if (at < token.size() && (token[at] == 'e' || token[at] == 'E'))
  {
    ++at;
    if (at < token.size() && (token[at] == '-' || token[at] == '+'))
    {
      ++at;
    }
    if (!digits(token, at))
    {
      return std::nullopt;
    }
  }
  if (at != token.size())
  {
    return std::nullopt;
  }
  auto value = 0.0;
  const auto* first = token.data() + mantissa;
  const auto parsed = std::from_chars(first, token.data() + token.size(), value);
  if (parsed.ec != std::errc{} || !std::isfinite(value) || std::abs(value) > 1e30)
  {
    return std::nullopt;
  }
  return token[0] == '-' ? -value : value;
}

// atoi as the copied reader calls it, held to a plain decimal integer of at most 9 digits
std::optional<long long> aseInteger(const std::string_view token)
{
  auto at = std::size_t{0};
  const auto negative = !token.empty() && token[0] == '-';
  if (!token.empty() && (token[0] == '-' || token[0] == '+'))
  {
    ++at;
  }
  if (at == token.size() || token.size() - at > 9)
  {
    return std::nullopt;
  }
  auto value = 0LL;
  for (; at < token.size(); ++at)
  {
    if (!isDigit(token[at]))
    {
      return std::nullopt;
    }
    value = value * 10 + (token[at] - '0');
  }
  return negative ? -value : value;
}

// ASE_GetToken's delimiters (Model_ase.cpp:75-82, :99-117): a char of value 32 or less,
// and char is signed in the engine's builds, so every byte from 0x80 up delimits too
bool aseDelimiter(const char c)
{
  return static_cast<signed char>(c) <= 32;
}

/**
 * The copied ASE reader walked without writing anything (aseFileProblem): the tokenizer,
 * ASE_ParseBracedBlock and every key function of Model_ase.cpp in the same order, with
 * the counts, lists and indexes each write and the conversion's reads depend on.
 */
class AseShadow
{
public:
  explicit AseShadow(const std::string_view text)
    : m_text{text}
  {
  }

  std::optional<std::string> run()
  {
    // ASE_Parse (:874-932)
    while (ok() && next(false))
    {
      const auto token = m_token;
      if (token == "*3DSMAX_ASCIIEXPORT" || token == "*COMMENT")
      {
        static_cast<void>(next(true));
      }
      else if (token == "*SCENE" || token == "*SHAPEOBJECT" || token == "*CAMERAOBJECT")
      {
        skipEnclosingBraces(token);
      }
      else if (token == "*GROUP")
      {
        static_cast<void>(next(false));
        braced("*GROUP", [&](const std::string_view t) {
          if (t == "*GEOMOBJECT")
          {
            geomObject();
          }
        });
      }
      else if (token == "*MATERIAL_LIST")
      {
        braced("*MATERIAL_LIST", [&](const std::string_view t) { materialList(t); });
      }
      else if (token == "*GEOMOBJECT")
      {
        geomObject();
      }
      else if (token == "{" || token == "}")
      {
        // the reader prints it and goes on (:925-928); the validator wants the braced
        // structure balanced
        fail(fmt::format("a '{}' outside any block", std::string{token}));
      }
    }
    if (ok())
    {
      finish();
    }
    return m_problem;
  }

private:
  struct Mesh
  {
    long long numVertexes = 0;
    long long numTVertexes = 0;
    long long numCVertexes = 0;
    long long numFaces = 0;
    long long numTVFaces = 0;
    long long numCVFaces = 0;
    // NUMVERTEX, NUMTVERTEX, NUMCVERTEX, NUMFACES declared
    std::array<bool, 4> declared = {};
    // VERTEX_LIST, TVERTLIST, CVERTLIST, FACE_LIST, TFACELIST, CFACELIST, NORMALS given
    std::array<bool, 7> list = {};
    bool colorsParsed = false;
    bool normalsParsed = false;
    // vertexNum as the reader stores it (A, C, B into 0, 2, 1) and tVertexNum
    std::vector<std::array<long long, 3>> faces;
    std::vector<std::array<long long, 3>> tfaces;
    std::array<std::array<double, 3>, 4> transform = {};
  };

  struct Object
  {
    Mesh mesh;
    bool meshSeen = false;
    long long materialRef = 0;
  };

  static constexpr auto MaxObjects = 1024;
  static constexpr auto MaxMaterials = 1024;
  static constexpr auto MaxCount = 1LL << 20;

  bool ok() const { return !m_problem; }

  void fail(const std::string& what)
  {
    if (!m_problem)
    {
      const auto line = 1 + std::count(m_text.begin(), m_text.begin() + long(m_at), '\n');
      m_problem = fmt::format("line {}: {}", line, what);
    }
  }

  // ASE_GetToken (:84-123): false at the end of the text, the token unchanged
  bool next(const bool restOfLine)
  {
    if (m_at == m_text.size())
    {
      return false;
    }
    while (m_at < m_text.size() && aseDelimiter(m_text[m_at]))
    {
      ++m_at;
    }
    const auto start = m_at;
    while (m_at < m_text.size())
    {
      const auto c = m_text[m_at];
      ++m_at;
      if ((aseDelimiter(c) && !restOfLine) || c == '\n' || c == '\r')
      {
        m_token = m_text.substr(start, m_at - 1 - start);
        return true;
      }
    }
    m_token = m_text.substr(start, m_at - start);
    return true;
  }

  // the next token of a key, which the file must still hold
  bool value(const std::string_view what)
  {
    if (!next(false))
    {
      fail(fmt::format("the file ends in {}", std::string{what}));
      return false;
    }
    return true;
  }

  std::optional<double> number(const std::string_view what)
  {
    if (!value(what))
    {
      return std::nullopt;
    }
    const auto n = aseNumber(m_token);
    if (!n)
    {
      fail(fmt::format(
        "{}: '{}' is not a finite decimal number",
        std::string{what},
        std::string{m_token}));
    }
    return n;
  }

  std::optional<long long> integer(const std::string_view what)
  {
    if (!value(what))
    {
      return std::nullopt;
    }
    const auto n = aseInteger(m_token);
    if (!n)
    {
      fail(fmt::format(
        "{}: '{}' is not a decimal integer", std::string{what}, std::string{m_token}));
    }
    return n;
  }

  // ASE_ParseBracedBlock (:125-159)
  template <typename Parser>
  void braced(const std::string_view what, Parser parser)
  {
    auto indent = 0;
    while (ok() && next(false))
    {
      const auto token = m_token;
      if (token == "{")
      {
        ++indent;
      }
      else if (token == "}")
      {
        --indent;
        if (indent == 0)
        {
          return;
        }
        if (indent < 0)
        {
          fail(fmt::format(
            "an unexpected '}}' before {}'s block (Model_ase.cpp:144)",
            std::string{what}));
          return;
        }
      }
      else if (indent > 0)
      {
        parser(token);
      }
    }
    if (ok())
    {
      fail(fmt::format("the file ends inside {}'s block", std::string{what}));
    }
  }

  // ASE_SkipEnclosingBraces (:161-184)
  void skipEnclosingBraces(const std::string_view what)
  {
    auto indent = 0;
    while (ok() && next(false))
    {
      if (m_token == "{")
      {
        ++indent;
      }
      else if (m_token == "}")
      {
        --indent;
        if (indent == 0)
        {
          return;
        }
        if (indent < 0)
        {
          fail(fmt::format(
            "an unexpected '}}' before {}'s block (Model_ase.cpp:180)",
            std::string{what}));
          return;
        }
      }
    }
    if (ok())
    {
      fail(fmt::format("the file ends inside {}'s block", std::string{what}));
    }
  }

  // ASE_KeyMATERIAL_LIST (:287-306), ASE_KeyMATERIAL (:253-285), ASE_KeyMAP_DIFFUSE
  // (:191-251)
  void materialList(const std::string_view token)
  {
    if (token == "*MATERIAL_COUNT")
    {
      static_cast<void>(value(token));
    }
    else if (token == "*MATERIAL")
    {
      if (++m_materials > MaxMaterials)
      {
        fail(fmt::format("more than {} materials", MaxMaterials));
        return;
      }
      braced("*MATERIAL", [&](const std::string_view t) { material(t); });
    }
  }

  void material(const std::string_view token)
  {
    if (token == "*MAP_DIFFUSE")
    {
      braced("*MAP_DIFFUSE", [&](const std::string_view t) { mapDiffuse(t); });
    }
    else if (token == "*MATERIAL_NAME")
    {
      materialPath(token);
    }
  }

  void mapDiffuse(const std::string_view token)
  {
    if (token == "*BITMAP")
    {
      materialPath(token);
    }
    else if (
      token == "*UVW_U_OFFSET" || token == "*UVW_V_OFFSET" || token == "*UVW_U_TILING"
      || token == "*UVW_V_TILING" || token == "*UVW_ANGLE")
    {
      static_cast<void>(number(token));
    }
  }

  // :195-216, :259-281: a path through OSPathToRelativePath and idStrStatic buffers
  void materialPath(const std::string_view token)
  {
    if (value(token) && m_token.size() >= 256)
    {
      fail(fmt::format("a {} of 256 or more characters", std::string{token}));
    }
  }

  // ASE_ParseGeomObject (:845-859), ASE_KeyGEOMOBJECT (:781-843)
  void geomObject()
  {
    if (int(m_objects.size()) >= MaxObjects)
    {
      fail(fmt::format(
        "more than {} objects (the conversion's _alloca, Model.cpp:1779)", MaxObjects));
      return;
    }
    m_objects.emplace_back();
    auto& object = m_objects.back();
    braced("*GEOMOBJECT", [&](const std::string_view token) {
      if (token == "*NODE_NAME" || token == "*NODE_PARENT")
      {
        static_cast<void>(next(true));
      }
      else if (token == "*NODE_TM" || token == "*TM_ANIMATION")
      {
        braced(token, [&](const std::string_view t) { nodeTm(object, t); });
      }
      else if (token == "*MESH")
      {
        if (object.meshSeen)
        {
          fail(
            "a second *MESH in one *GEOMOBJECT (Model_ase.cpp:813 clears the first's "
            "lists: they leak)");
          return;
        }
        object.meshSeen = true;
        // :806-817: the mesh cleared, its transform kept
        const auto transform = object.mesh.transform;
        object.mesh = Mesh{};
        object.mesh.transform = transform;
        mesh(object.mesh);
      }
      else if (token == "*MATERIAL_REF")
      {
        if (const auto ref = integer(token))
        {
          object.materialRef = *ref;
        }
      }
      else if (token == "*MESH_ANIMATION")
      {
        braced(token, [&](const std::string_view t) {
          if (t != "*MESH")
          {
            fail(fmt::format(
              "an unknown token '{}' in *MESH_ANIMATION (Model_ase.cpp:777)",
              std::string{t}));
            return;
          }
          // :763-774: a frame of its own, transform zero; never converted
          auto frame = Mesh{};
          mesh(frame);
        });
      }
      else if (
        token == "*PROP_MOTIONBLUR" || token == "*PROP_CASTSHADOW"
        || token == "*PROP_RECVSHADOW")
      {
        static_cast<void>(next(true));
      }
    });
  }

  // ASE_KeyNODE_TM (:308-344): the rows of the object's mesh transform
  void nodeTm(Object& object, const std::string_view token)
  {
    for (auto row = 0; row < 4; ++row)
    {
      if (token == fmt::format("*TM_ROW{}", row))
      {
        for (auto i = 0; i < 3 && ok(); ++i)
        {
          if (const auto n = number(token))
          {
            object.mesh.transform[std::size_t(row)][std::size_t(i)] = *n;
          }
        }
      }
    }
  }

  // ASE_KeyMESH (:635-756) and its lists, then the conversion's reads of the mesh
  void mesh(Mesh& m)
  {
    braced("*MESH", [&](const std::string_view token) { meshKey(m, token); });
    if (ok())
    {
      finishMesh(m);
    }
  }

  void count(Mesh& m, const std::string_view token, const int which, long long& target)
  {
    if (m.declared[std::size_t(which)])
    {
      fail(fmt::format("{} given twice in one *MESH", std::string{token}));
      return;
    }
    // the list it sizes (VERTEX_LIST, TVERTLIST, CVERTLIST, FACE_LIST)
    if (m.list[std::size_t(which)])
    {
      fail(fmt::format(
        "{} after its list (the list was allocated for the count before it)",
        std::string{token}));
      return;
    }
    const auto n = integer(token);
    if (!n)
    {
      return;
    }
    if (*n < 0 || *n > MaxCount)
    {
      fail(fmt::format("{} {} is outside 0..{}", std::string{token}, *n, MaxCount));
      return;
    }
    m.declared[std::size_t(which)] = true;
    target = *n;
  }

  // a list of the mesh, given once (a second allocation would leak the first)
  bool openList(Mesh& m, const std::string_view token, const int which)
  {
    if (m.list[std::size_t(which)])
    {
      fail(fmt::format(
        "{} given twice in one *MESH (the first list would leak)", std::string{token}));
      return false;
    }
    m.list[std::size_t(which)] = true;
    return true;
  }

  void entries(
    const std::string_view list,
    const long long seen,
    const long long expected,
    const std::string_view count,
    const std::string_view site)
  {
    if (ok() && seen != expected)
    {
      fail(fmt::format(
        "{} holds {} entries for its {} of {} ({})",
        std::string{list},
        seen,
        std::string{count},
        expected,
        std::string{site}));
    }
  }

  void meshKey(Mesh& m, const std::string_view token)
  {
    if (token == "*TIMEVALUE")
    {
      static_cast<void>(value(token));
    }
    else if (token == "*MESH_NUMVERTEX")
    {
      count(m, token, 0, m.numVertexes);
    }
    else if (token == "*MESH_NUMTVERTEX")
    {
      count(m, token, 1, m.numTVertexes);
    }
    else if (token == "*MESH_NUMCVERTEX")
    {
      count(m, token, 2, m.numCVertexes);
    }
    else if (token == "*MESH_NUMFACES")
    {
      count(m, token, 3, m.numFaces);
    }
    else if (token == "*MESH_NUMTVFACES")
    {
      if (const auto n = integer(token))
      {
        m.numTVFaces = *n;
        if (m.numTVFaces != m.numFaces)
        {
          fail("*MESH_NUMTVFACES != *MESH_NUMFACES (Model_ase.cpp:681-684)");
        }
      }
    }
    else if (token == "*MESH_NUMCVFACES")
    {
      if (const auto n = integer(token))
      {
        m.numCVFaces = *n;
        // :693: the engine compares numTVFaces, not numCVFaces
        if (m.numTVFaces != m.numFaces)
        {
          fail(
            "*MESH_NUMCVFACES while *MESH_NUMTVFACES != *MESH_NUMFACES (the engine's "
            "Error, Model_ase.cpp:693-696)");
        }
      }
    }
    else if (token == "*MESH_VERTEX_LIST")
    {
      if (openList(m, token, 0))
      {
        vertexList(m, token, "*MESH_VERTEX", m.numVertexes, 0);
      }
    }
    else if (token == "*MESH_TVERTLIST")
    {
      if (openList(m, token, 1))
      {
        vertexList(m, token, "*MESH_TVERT", m.numTVertexes, 1);
      }
    }
    else if (token == "*MESH_CVERTLIST")
    {
      if (openList(m, token, 2))
      {
        vertexList(m, token, "*MESH_VERTCOL", m.numCVertexes, 2);
      }
    }
    else if (token == "*MESH_FACE_LIST")
    {
      if (openList(m, token, 3))
      {
        faceList(m);
      }
    }
    else if (token == "*MESH_TFACELIST")
    {
      if (!m.list[3])
      {
        fail("*MESH_TFACELIST before *MESH_FACE_LIST (Model_ase.cpp:728-731)");
      }
      else if (openList(m, token, 4))
      {
        tfaceList(m);
      }
    }
    else if (token == "*MESH_CFACELIST")
    {
      if (!m.list[3])
      {
        fail("*MESH_CFACELIST before *MESH_FACE_LIST (Model_ase.cpp:738-741)");
      }
      else if (!m.list[2])
      {
        fail(
          "*MESH_CFACELIST before *MESH_CVERTLIST (Model_ase.cpp:465 reads a NULL list)");
      }
      else if (openList(m, token, 5))
      {
        cfaceList(m);
      }
    }
    else if (token == "*MESH_NORMALS")
    {
      if (!m.list[3])
      {
        fail(
          "*MESH_NORMALS before *MESH_FACE_LIST (Model_ase.cpp:748-750, then the faces "
          "are NULL)");
      }
      else if (openList(m, token, 6))
      {
        normals(m);
      }
    }
  }

  // ASE_KeyMESH_VERTEX_LIST (:346-374), ASE_KeyMESH_TVERTLIST (:478-516),
  // ASE_KeyMESH_CVERTLIST (:518-548): each entry written before its count check
  void vertexList(
    Mesh& m,
    const std::string_view list,
    const std::string_view entry,
    const long long expected,
    const int which)
  {
    auto seen = 0LL;
    braced(list, [&](const std::string_view token) {
      if (which == 2)
      {
        // :522: any token of the list
        m.colorsParsed = true;
      }
      if (token != entry)
      {
        fail(fmt::format(
          "an unknown token '{}' in {} (Model_ase.cpp:372, :514, :546)",
          std::string{token},
          std::string{list}));
        return;
      }
      if (++seen > expected)
      {
        fail(fmt::format(
          "{} holds more than its {} entries (Model_ase.cpp:355-368: written before the "
          "count check)",
          std::string{list},
          expected));
        return;
      }
      // the entry's number, then three values (the third of a tvert is read, not used)
      static_cast<void>(value(entry));
      // (number() takes at most 64 characters: a tvert's strncpy keeps 79, :490-499)
      for (auto i = 0; i < 3 && ok(); ++i)
      {
        static_cast<void>(number(entry));
      }
    });
    entries(list, seen, expected, "count", "Model_ase.cpp:355-368, :501-510, :529-541");
  }

  // ASE_KeyMESH_FACE_LIST (:376-419): faces[currentFace] with no check at all
  void faceList(Mesh& m)
  {
    braced("*MESH_FACE_LIST", [&](const std::string_view token) {
      if (token != "*MESH_FACE")
      {
        fail(fmt::format(
          "an unknown token '{}' in *MESH_FACE_LIST (Model_ase.cpp:417)",
          std::string{token}));
        return;
      }
      if (std::ssize(m.faces) >= m.numFaces)
      {
        fail(fmt::format(
          "*MESH_FACE_LIST holds more than its *MESH_NUMFACES {} (Model_ase.cpp:380-413 "
          "writes past the faces)",
          m.numFaces));
        return;
      }
      auto face = std::array<long long, 3>{};
      // the face's number, then label and value three times: A into 0, B into 2, C into 1
      static_cast<void>(value(token));
      for (const auto slot : {0, 2, 1})
      {
        static_cast<void>(value(token));
        if (const auto n = integer("a *MESH_FACE corner"))
        {
          face[std::size_t(slot)] = *n;
        }
      }
      static_cast<void>(next(true));
      m.faces.push_back(face);
    });
    entries(
      "*MESH_FACE_LIST",
      std::ssize(m.faces),
      m.numFaces,
      "*MESH_NUMFACES",
      "Model_ase.cpp:721");
  }

  // ASE_KeyTFACE_LIST (:421-448): faces[currentFace] with no check; a, c, b into 0, 1, 2
  void tfaceList(Mesh& m)
  {
    braced("*MESH_TFACELIST", [&](const std::string_view token) {
      if (token != "*MESH_TFACE")
      {
        fail(fmt::format(
          "an unknown token '{}' in *MESH_TFACELIST (Model_ase.cpp:446)",
          std::string{token}));
        return;
      }
      if (std::ssize(m.tfaces) >= m.numFaces)
      {
        fail(fmt::format(
          "*MESH_TFACELIST holds more than its {} faces (Model_ase.cpp:438-440 writes "
          "past "
          "them)",
          m.numFaces));
        return;
      }
      static_cast<void>(value(token));
      auto read = std::array<long long, 3>{};
      for (auto& n : read)
      {
        if (const auto v = integer("a *MESH_TFACE corner"))
        {
          n = *v;
        }
      }
      m.tfaces.push_back({read[0], read[2], read[1]});
    });
    entries(
      "*MESH_TFACELIST",
      std::ssize(m.tfaces),
      m.numFaces,
      "faces",
      "Model_ase.cpp:438-440");
  }

  // ASE_KeyCFACE_LIST (:450-476): cvertexes[a] unchecked (:465), faces[currentFace] too
  void cfaceList(Mesh& m)
  {
    auto seen = 0LL;
    braced("*MESH_CFACELIST", [&](const std::string_view token) {
      if (token != "*MESH_CFACE")
      {
        fail(fmt::format(
          "an unknown token '{}' in *MESH_CFACELIST (Model_ase.cpp:474)",
          std::string{token}));
        return;
      }
      if (++seen > m.numFaces)
      {
        fail(fmt::format(
          "*MESH_CFACELIST holds more than its {} faces (Model_ase.cpp:465-467 writes "
          "past them)",
          m.numFaces));
        return;
      }
      static_cast<void>(value(token));
      for (auto i = 0; i < 3 && ok(); ++i)
      {
        const auto a = integer("a *MESH_CFACE corner");
        if (a && (*a < 0 || *a >= m.numCVertexes))
        {
          fail(fmt::format(
            "a *MESH_CFACE colour index {} outside the {} *MESH_VERTCOL "
            "(Model_ase.cpp:465)",
            *a,
            m.numCVertexes));
        }
      }
    });
    entries("*MESH_CFACELIST", seen, m.numFaces, "faces", "Model_ase.cpp:465-467");
  }

  // ASE_KeyMESH_NORMALS (:550-633): the face normal of each face in order, then a
  // vertex normal for each of its three corners
  void normals(Mesh& m)
  {
    auto current = 0LL;
    auto corners = std::array<bool, 3>{true, true, true};
    const auto faceDone = [&] {
      if (current > 0 && !(corners[0] && corners[1] && corners[2]))
      {
        fail(fmt::format(
          "face {} has a corner without a *MESH_VERTEXNORMAL (the engine's normal would "
          "be uninitialised)",
          current - 1));
      }
    };
    const auto normal = [&](const std::string_view what) {
      auto n = std::array<double, 3>{};
      for (auto& c : n)
      {
        if (const auto v = number(what))
        {
          c = *v;
        }
      }
      if (!ok())
      {
        return;
      }
      // :583-587, :627-631: transformed by the mesh's rows, then normalised
      auto length = 0.0;
      for (std::size_t j = 0; j < 3; ++j)
      {
        const auto t =
          n[0] * m.transform[0][j] + n[1] * m.transform[1][j] + n[2] * m.transform[2][j];
        length += t * t;
      }
      if (!(length > 1e-20) || !std::isfinite(length))
      {
        fail(fmt::format(
          "a {} of zero length after the mesh's *NODE_TM (its Normalize divides by zero)",
          std::string{what}));
      }
    };
    braced("*MESH_NORMALS", [&](const std::string_view token) {
      // :556: any token of the block
      m.normalsParsed = true;
      if (token == "*MESH_FACENORMAL")
      {
        const auto num = integer(token);
        if (!num)
        {
          return;
        }
        if (*num >= m.numFaces || *num < 0 || *num != current)
        {
          fail(fmt::format(
            "*MESH_FACENORMAL {} out of order or range (Model_ase.cpp:566-574)", *num));
          return;
        }
        faceDone();
        normal(token);
        corners = {false, false, false};
        ++current;
      }
      else if (token == "*MESH_VERTEXNORMAL")
      {
        const auto num = integer(token);
        if (!num)
        {
          return;
        }
        if (*num >= m.numVertexes || *num < 0)
        {
          fail(fmt::format(
            "*MESH_VERTEXNORMAL {} outside the {} vertices (Model_ase.cpp:599-602)",
            *num,
            m.numVertexes));
          return;
        }
        if (current == 0)
        {
          fail(
            "a *MESH_VERTEXNORMAL before any *MESH_FACENORMAL (Model_ase.cpp:604 reads "
            "faces[-1])");
          return;
        }
        const auto& face = m.faces[std::size_t(current - 1)];
        auto v = std::size_t{0};
        while (v < 3 && face[v] != *num)
        {
          ++v;
        }
        if (v >= 3)
        {
          fail(fmt::format(
            "*MESH_VERTEXNORMAL {} is no corner of face {} (Model_ase.cpp:614-618)",
            *num,
            current - 1));
          return;
        }
        normal(token);
        corners[v] = true;
      }
    });
    if (ok())
    {
      faceDone();
      if (current != m.numFaces)
      {
        fail(fmt::format(
          "*MESH_NORMALS holds {} face normals for {} faces", current, m.numFaces));
      }
    }
  }

  // what the conversion reads of a mesh (Model.cpp:1855-2050)
  void finishMesh(Mesh& m)
  {
    if (m.numVertexes > 0 && !m.list[0])
    {
      fail(
        "*MESH_NUMVERTEX without its *MESH_VERTEX_LIST (the conversion reads NULL "
        "vertices)");
    }
    else if (m.numTVertexes > 0 && !m.list[1])
    {
      fail(
        "*MESH_NUMTVERTEX without its *MESH_TVERTLIST (the conversion reads NULL "
        "coordinates)");
    }
    else if (m.numFaces > 0 && !m.list[3])
    {
      fail("*MESH_NUMFACES without its *MESH_FACE_LIST");
    }
    if (!ok())
    {
      return;
    }
    for (std::size_t f = 0; f < m.faces.size(); ++f)
    {
      for (const auto v : m.faces[f])
      {
        if (v < 0 || v >= m.numVertexes)
        {
          fail(fmt::format(
            "face {} names vertex {} of {} (Model.cpp:1959-1962, then vRemap[v] reads "
            "past "
            "the list)",
            f,
            v,
            m.numVertexes));
          return;
        }
      }
    }
    // Model.cpp:1966: the coordinates are used when the counts agree
    if (m.numFaces > 0 && m.numTVFaces == m.numFaces && m.numTVertexes != 0)
    {
      if (!m.list[4])
      {
        fail(
          "texture coordinates without a *MESH_TFACELIST (the engine's corners would be "
          "uninitialised)");
        return;
      }
      for (std::size_t f = 0; f < m.tfaces.size(); ++f)
      {
        for (const auto tv : m.tfaces[f])
        {
          if (tv < 0 || tv >= m.numTVertexes)
          {
            fail(fmt::format(
              "face {} names texture vertex {} of {} (Model.cpp:1970-1974, then tvRemap "
              "reads past the list)",
              f,
              tv,
              m.numTVertexes));
            return;
          }
        }
      }
    }
    if (m.numFaces > 0 && m.colorsParsed && !m.list[5])
    {
      fail(
        "*MESH_CVERTLIST without a *MESH_CFACELIST (the engine's vertex colours would "
        "be uninitialised)");
    }
  }

  // the conversion's reads of the objects (Model.cpp:1779-1860, :2015-2058)
  void finish()
  {
    auto corners = 0LL;
    for (std::size_t i = 0; i < m_objects.size(); ++i)
    {
      const auto& object = m_objects[i];
      if (
        object.materialRef < 0 || (m_materials > 0 && object.materialRef >= m_materials))
      {
        fail(fmt::format(
          "object {}'s *MATERIAL_REF {} names none of the {} materials (Model.cpp:1803, "
          ":1815, :2058)",
          i,
          object.materialRef,
          m_materials));
        return;
      }
      corners += 3 * object.mesh.numFaces;
    }
    if (corners > static_cast<long long>(MaxObjCorners))
    {
      fail(fmt::format(
        "more than {} face corners (the conversion's 16-bit indexes, Model.cpp:2033)",
        MaxObjCorners));
    }
  }

  std::string_view m_text;
  std::size_t m_at = 0;
  std::string_view m_token;
  std::optional<std::string> m_problem;
  long long m_materials = 0;
  std::deque<Object> m_objects;
};

} // namespace

std::optional<std::string> aseFileProblem(const std::string_view bytes)
{
  if (bytes.size() >= std::size_t(INT_MAX))
  {
    return "the file is 2 GB or larger";
  }
  if (bytes.find('\0') != std::string_view::npos)
  {
    return "the file holds a NUL byte (ASE_Parse reads to the first)";
  }
  // ASE_GetToken writes into a 1024-character buffer with no bound (Model_ase.cpp:55,
  // :105-120): a token, or the rest of a line, is at most a line
  auto lineNumber = 1;
  auto lineLength = std::size_t{0};
  for (const auto c : bytes)
  {
    if (c == '\n' || c == '\r')
    {
      lineNumber += c == '\n' ? 1 : 0;
      lineLength = 0;
      continue;
    }
    if (++lineLength > 1000)
    {
      return fmt::format(
        "line {}: longer than 1,000 characters (ASE_GetToken's 1,024-character buffer, "
        "Model_ase.cpp:55)",
        lineNumber);
    }
  }
  return AseShadow{bytes}.run();
}

} // namespace tb::lit
