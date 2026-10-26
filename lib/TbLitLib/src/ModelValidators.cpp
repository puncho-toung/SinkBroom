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
// renderer/Model_lwo.cpp, renderer/Model.cpp and idlib/Lexer.cpp):
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
#include <cstring>
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
  // LP7.2: OBJ_Parse's objects (Model_obj.cpp:136-158, :215-228): one at each o or g that
  // follows a face of the current object, one at the end
  auto objects = std::size_t{0};
  auto objectHasFaces = false;
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
      // :114-158: the rest of the line is read and dropped; o and g close the object
      if ((first == "o" || first == "g") && std::exchange(objectHasFaces, false))
      {
        ++objects;
      }
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
        // LP7.2 (step 1): what the engine does with another count. A number past the
        // ones ParseFloat takes is the next ReadToken's, which no keyword matches: :211's
        // FatalError "Unknown or unexpected token" (a "vt u v w" line, sponza's); one too
        // few reads the next line's keyword as a number (ParseFloat's Error)
        return problem(fmt::format(
          "'{}' takes {} numbers, not {}: {}",
          first,
          wanted,
          tokens.size() - 1,
          tokens.size() > wanted + 1
            ? "the engine reads the extra number as the next keyword and stops "
              "(Model_obj.cpp:211, FatalError \"Unknown or unexpected token\")"
            : "the engine reads the next line's keyword as a number and stops "
              "(Lexer.cpp ParseFloat's Error)"));
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
      objectHasFaces = true;
      if (corners > MaxObjSourceCorners)
      {
        return problem(fmt::format(
          "more than {} face corners (the preview's memory guard; the engine's limit is "
          "65536 vertices per finished surface)",
          MaxObjSourceCorners));
      }
      continue;
    }
    return problem(fmt::format("an unexpected token '{}'", std::string{first}));
  }
  if (objects + (objectHasFaces ? 1 : 0) > MaxObjObjects)
  {
    return fmt::format(
      "more than {} objects (the conversion's _alloca of one int per object, "
      "Model.cpp:1417; the preview's guard)",
      MaxObjObjects);
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

namespace
{

constexpr std::uint32_t lwid(const char a, const char b, const char c, const char d)
{
  return (std::uint32_t(std::uint8_t(a)) << 24) | (std::uint32_t(std::uint8_t(b)) << 16)
         | (std::uint32_t(std::uint8_t(c)) << 8) | std::uint32_t(std::uint8_t(d));
}

std::string lwidText(const std::uint32_t id)
{
  auto text = std::string{};
  for (const auto shift : {24, 16, 8, 0})
  {
    const auto c = char((id >> shift) & 0xFF);
    text.push_back(c >= ' ' && c <= '~' ? c : '?');
  }
  return text;
}

/**
 * The copied LWO2 reader walked without allocating (lwoFileProblem): lwGetObject's chunk
 * loop (Model_lwo.cpp:1775-2067) and the chunk readers it calls, by the sizes the file
 * declares, with the counts and indexes their post-processing (:2019-2045) and the
 * conversion (Model.cpp:2109-2529) depend on.
 */
class LwoWalk
{
public:
  explicit LwoWalk(const std::string_view file)
    : m_file{file}
  {
  }

  std::optional<std::string> run()
  {
    if (m_file.size() < 12)
    {
      return "shorter than an IFF FORM header (12 bytes)";
    }
    if (u4(0) != lwid('F', 'O', 'R', 'M'))
    {
      return "not an IFF FORM";
    }
    const auto type = u4(8);
    if (type == lwid('L', 'W', 'O', 'B'))
    {
      return "LightWave 5 objects (LWOB) are not read by the preview";
    }
    if (type != lwid('L', 'W', 'O', '2'))
    {
      return fmt::format("not an LWO2 object (FORM type '{}')", lwidText(type));
    }
    const auto formSize = std::size_t(u4(4));
    if (formSize < 4 || formSize > m_file.size() - 8)
    {
      return fmt::format(
        "the FORM's size {} passes the file's {} bytes", formSize, m_file.size());
    }
    const auto end = formSize + 8;
    if (end == 12)
    {
      return "an LWO2 FORM without chunks";
    }
    m_layers.emplace_back();

    // :1849-2010: chunk after chunk until the FORM's end, each its padded size
    auto at = std::size_t{12};
    while (ok() && at < end)
    {
      if (end - at < 8)
      {
        fail(at, "a chunk header past the FORM's end");
        break;
      }
      const auto id = u4(at);
      const auto size = std::size_t(u4(at + 4));
      const auto padded = size + (size & 1);
      if (padded > end - at - 8)
      {
        fail(
          at,
          fmt::format(
            "chunk '{}': its size {} passes the FORM's end (:1854 reads past it)",
            lwidText(id),
            size));
        break;
      }
      chunk(id, at + 8, size);
      at += 8 + padded;
    }
    if (ok())
    {
      finish();
    }
    return m_problem;
  }

private:
  struct Layer
  {
    std::size_t points = 0;
    std::size_t pointOffset = 0;
    std::size_t polygons = 0;
    std::size_t polygonOffset = 0;
    std::size_t triangles = 0;
    // the largest VMAP point index and VMAD polygon index, for the end
    std::optional<std::size_t> vmapPoint;
    std::optional<std::size_t> vmadPolygon;
  };

  static constexpr auto MaxLayers = std::size_t{256};
  static constexpr auto MaxPoints = std::size_t{1} << 20;
  static constexpr auto MaxPolygons = std::size_t{1} << 20;
  static constexpr auto MaxFirstLayerPolygons = std::size_t{32767};
  static constexpr auto MaxTags = std::size_t{256};
  static constexpr auto MaxSurfaces = std::size_t{256};
  static constexpr auto MaxVmapDimension = std::size_t{16};

  bool ok() const { return !m_problem; }

  void fail(const std::size_t at, const std::string& what)
  {
    if (!m_problem)
    {
      m_problem = fmt::format("byte {}: {}", at, what);
    }
  }

  std::uint32_t u1(const std::size_t at) const { return std::uint8_t(m_file[at]); }
  std::uint32_t u2(const std::size_t at) const { return (u1(at) << 8) | u1(at + 1); }
  std::uint32_t u4(const std::size_t at) const { return (u2(at) << 16) | u2(at + 2); }

  float f4(const std::size_t at) const
  {
    const auto bits = u4(at);
    auto value = 0.0f;
    std::memcpy(&value, &bits, 4);
    return value;
  }

  // a VX index (:1390-1442): 2 bytes, or 4 when the first is 0xFF; its size in size
  std::optional<std::size_t> vx(
    const std::size_t at, const std::size_t end, std::size_t& size) const
  {
    if (at + 2 > end)
    {
      return std::nullopt;
    }
    if (u1(at) != 0xFF)
    {
      size = 2;
      return std::size_t(u2(at));
    }
    if (at + 4 > end)
    {
      return std::nullopt;
    }
    size = 4;
    return std::size_t((u1(at + 1) << 16) | (u1(at + 2) << 8) | u1(at + 3));
  }

  // an S0 string (:1468-1525, :1678-1705) terminated before end: what it takes, padded to
  // even (2 for an empty one), and its length; nullopt when its NUL is not before end
  std::optional<std::size_t> s0(
    const std::size_t at, const std::size_t end, std::size_t& length) const
  {
    const auto nul = m_file.find('\0', at);
    if (nul == std::string_view::npos || nul >= end)
    {
      return std::nullopt;
    }
    length = nul - at;
    const auto taken = length + 1;
    const auto padded = length == 0 ? std::size_t{2} : taken + (taken & 1);
    if (at + padded > end)
    {
      return std::nullopt;
    }
    return padded;
  }

  bool finite(const float value, const float bound = 1e30f) const
  {
    return std::isfinite(value) && std::abs(value) <= bound;
  }

  Layer& layer() { return m_layers.back(); }

  void chunk(const std::uint32_t id, const std::size_t data, const std::size_t size)
  {
    const auto end = data + size;
    const auto name = lwidText(id);
    // the readers that parse a chunk's body take its padded size: refused odd (no pad
    // inside what they parse)
    const auto even = [&] {
      if (size & 1)
      {
        fail(data, fmt::format("chunk '{}' of odd size {}", name, size));
        return false;
      }
      return true;
    };
    if (id == lwid('L', 'A', 'Y', 'R'))
    {
      // :1862-1897: a new layer after the first LAYR
      if (m_nlayers > 0)
      {
        if (m_layers.size() >= MaxLayers)
        {
          fail(data, fmt::format("more than {} layers", MaxLayers));
          return;
        }
        m_layers.emplace_back();
      }
      ++m_nlayers;
      auto length = std::size_t{0};
      if (!even() || size < 18 || !s0(data + 16, end, length))
      {
        fail(data, "a LAYR chunk without its index, flags, pivot and a terminated name");
      }
    }
    else if (id == lwid('P', 'N', 'T', 'S'))
    {
      points(data, size);
    }
    else if (id == lwid('P', 'O', 'L', 'S'))
    {
      if (even())
      {
        polygons(data, size);
      }
    }
    else if (id == lwid('V', 'M', 'A', 'P') || id == lwid('V', 'M', 'A', 'D'))
    {
      if (even())
      {
        vmap(data, size, id == lwid('V', 'M', 'A', 'D'));
      }
    }
    else if (id == lwid('P', 'T', 'A', 'G'))
    {
      if (even())
      {
        polygonTags(data, size);
      }
    }
    else if (id == lwid('T', 'A', 'G', 'S'))
    {
      if (even())
      {
        tags(data, size);
      }
    }
    else if (id == lwid('B', 'B', 'O', 'X'))
    {
      if (size < 24)
      {
        fail(data, "a BBOX chunk shorter than its six floats");
      }
    }
    else if (id == lwid('E', 'N', 'V', 'L'))
    {
      if (even())
      {
        envelope(data, size);
      }
    }
    else if (id == lwid('C', 'L', 'I', 'P'))
    {
      if (even())
      {
        clip(data, size);
      }
    }
    else if (id == lwid('S', 'U', 'R', 'F'))
    {
      if (even())
      {
        surface(data, size);
      }
    }
  }

  // lwGetPoints (:3031-3078): twelve bytes a point, appended to the layer's
  void points(const std::size_t data, const std::size_t size)
  {
    if (size < 12 || size % 12 != 0)
    {
      fail(data, fmt::format("a PNTS chunk of {} bytes: not points of 12 bytes", size));
      return;
    }
    for (auto at = data; at < data + size; at += 4)
    {
      if (!finite(f4(at)))
      {
        fail(at, "a point coordinate that is not finite or larger than 1e30");
        return;
      }
    }
    auto& l = layer();
    l.pointOffset = l.points;
    l.points += size / 12;
    if (l.points > MaxPoints)
    {
      fail(data, fmt::format("more than {} points in a layer", MaxPoints));
    }
  }

  // lwGetPolygons (:3184-3270): the polygons exactly fill the chunk (:3214-3253 read past
  // it otherwise), each vertex index within the points (:3295 "FIXME", :3330, :3353 and
  // Model.cpp:2381 index unchecked); at least one (:3153's pol[0] of a zero-polygon
  // chunk)
  void polygons(const std::size_t data, const std::size_t size)
  {
    if (size < 6)
    {
      fail(data, "a POLS chunk without a polygon (:3153 writes pol[0] of an empty list)");
      return;
    }
    const auto end = data + size;
    auto& l = layer();
    auto at = data + 4;
    auto count = std::size_t{0};
    while (at < end)
    {
      if (at + 2 > end)
      {
        fail(at, "a polygon record past the POLS chunk (:3216 reads past it)");
        return;
      }
      const auto nv = u2(at) & 0x03FF;
      at += 2;
      for (auto j = 0u; j < nv; ++j)
      {
        auto taken = std::size_t{0};
        const auto index = vx(at, end, taken);
        if (!index)
        {
          fail(at, "a polygon's vertex index past the POLS chunk (:3220, :3248)");
          return;
        }
        if (*index + l.pointOffset >= l.points)
        {
          fail(
            at,
            fmt::format(
              "a polygon's vertex {} past the layer's {} points (:3295, :3330, "
              "Model.cpp:2381)",
              *index + l.pointOffset,
              l.points));
          return;
        }
        at += taken;
      }
      ++count;
      l.triangles += nv == 3 ? 1 : 0;
    }
    l.polygonOffset = l.polygons;
    l.polygons += count;
    m_anyPolygons = true;
    if (l.polygons > MaxPolygons)
    {
      fail(data, fmt::format("more than {} polygons in a layer", MaxPolygons));
    }
  }

  // lwGetVMap (:4975-5085): the type, dimension and name, then the records exactly
  // filling the chunk (:5017-5074 read past it otherwise), at least one (lwFreeVMap's
  // val[0], :4957); the indexes checked at the end (lwGetPointVMaps :5110, :5133;
  // lwGetPolyVMaps :5174, :5207)
  void vmap(const std::size_t data, const std::size_t size, const bool perpoly)
  {
    const auto end = data + size;
    auto length = std::size_t{0};
    const auto header = size >= 8 ? s0(data + 6, end, length) : std::nullopt;
    if (!header)
    {
      fail(data, "a VMAP/VMAD chunk without its type, dimension and a terminated name");
      return;
    }
    const auto type = u4(data);
    const auto dim = std::size_t(u2(data + 4));
    if (dim > MaxVmapDimension)
    {
      fail(data, fmt::format("a vmap of dimension {}", dim));
      return;
    }
    // Model.cpp:2252 reads a TXUV's val[k][1], :2412 and :2430 an RGBA's val[i][0..3]
    const auto txuv = type == lwid('T', 'X', 'U', 'V');
    const auto rgba = type == lwid('R', 'G', 'B', 'A');
    if ((txuv && dim < 2) || (rgba && dim < 4))
    {
      fail(
        data,
        fmt::format(
          "a {} vmap of dimension {} (the conversion reads past its values)",
          lwidText(type),
          dim));
      return;
    }
    auto& l = layer();
    auto at = data + 6 + *header;
    auto records = std::size_t{0};
    while (at < end)
    {
      auto taken = std::size_t{0};
      const auto point = vx(at, end, taken);
      if (!point)
      {
        fail(at, "a vmap record past its chunk (:5017-5022 read past it)");
        return;
      }
      at += taken;
      if (perpoly)
      {
        const auto polygon = vx(at, end, taken);
        if (!polygon)
        {
          fail(at, "a vmad record past its chunk");
          return;
        }
        at += taken;
        l.vmadPolygon = std::max(l.vmadPolygon.value_or(0), *polygon);
      }
      else
      {
        l.vmapPoint = std::max(l.vmapPoint.value_or(0), *point);
      }
      if (end - at < 4 * dim)
      {
        fail(at, "a vmap record's values past its chunk (:5072 reads past it)");
        return;
      }
      for (std::size_t j = 0; j < dim; ++j, at += 4)
      {
        const auto value = f4(at);
        if (
          (txuv && !finite(value))
          || (rgba && j < 4 && !(value >= 0.0f && value <= 1.0f)))
        {
          fail(
            at,
            rgba ? "an RGBA vmap value outside 0..1 (Model.cpp:2412: x 255 into a byte)"
                 : "a TXUV value that is not finite or larger than 1e30");
          return;
        }
      }
      ++records;
    }
    if (records == 0)
    {
      fail(data, "a vmap without records (:4957 reads val[0] of an empty list)");
    }
  }

  // lwGetPolygonTags (:3615-3660): SURF, PART and SMGP pairs exactly filling the chunk,
  // each polygon within the layer's (:3640-3656 write pol[i] unchecked); a SURF tag
  // index is checked at the end (lwResolvePolySurfaces, :3411)
  void polygonTags(const std::size_t data, const std::size_t size)
  {
    if (size < 4)
    {
      fail(data, "a PTAG chunk without its type");
      return;
    }
    const auto type = u4(data);
    const auto surfaces = type == lwid('S', 'U', 'R', 'F');
    if (!surfaces && type != lwid('P', 'A', 'R', 'T') && type != lwid('S', 'M', 'G', 'P'))
    {
      // :3630-3634: skipped
      return;
    }
    const auto end = data + size;
    const auto& l = layer();
    auto at = data + 4;
    while (at < end)
    {
      auto taken = std::size_t{0};
      const auto polygon = vx(at, end, taken);
      if (!polygon)
      {
        fail(at, "a PTAG record past its chunk");
        return;
      }
      at += taken;
      const auto tag = vx(at, end, taken);
      if (!tag)
      {
        fail(at, "a PTAG record past its chunk");
        return;
      }
      at += taken;
      if (*polygon + l.polygonOffset >= l.polygons)
      {
        fail(
          at,
          fmt::format(
            "a PTAG polygon {} past the layer's {} polygons (:3647 writes pol[i])",
            *polygon + l.polygonOffset,
            l.polygons));
        return;
      }
      if (surfaces)
      {
        m_surfaceTag = std::max(m_surfaceTag.value_or(0), *tag + m_tagOffset);
      }
    }
  }

  // lwGetTags (:3540-3605): strings exactly filling the chunk, each terminated in it
  // (:3565 and sgetS0's strlen, :1689, read past it otherwise), none empty (NULL: then
  // strcmp and strlen of NULL, :3397, :3422)
  void tags(const std::size_t data, const std::size_t size)
  {
    const auto end = data + size;
    auto at = data;
    auto count = std::size_t{0};
    while (at < end)
    {
      auto length = std::size_t{0};
      const auto taken = s0(at, end, length);
      if (!taken)
      {
        fail(at, "a tag not terminated in its TAGS chunk (:3565 reads past it)");
        return;
      }
      if (length == 0)
      {
        fail(at, "an empty tag (sgetS0's NULL, then strcmp of NULL at :3397)");
        return;
      }
      if (length >= 256)
      {
        fail(at, "a tag of 256 or more characters");
        return;
      }
      at += *taken;
      ++count;
    }
    m_tagOffset = m_tags;
    m_tags += count;
    if (m_tags > MaxTags)
    {
      fail(data, fmt::format("more than {} tags", MaxTags));
    }
  }

  // sub-chunks (an ID and a U2 size, padded) exactly filling [at, end); each passed to
  // visit; false when one passes the end
  template <typename Visit>
  bool subChunks(std::size_t at, const std::size_t end, const char* what, Visit visit)
  {
    if (end - at < 6)
    {
      fail(
        at, fmt::format("{} without a sub-chunk (its reader reads one past it)", what));
      return false;
    }
    while (ok() && at < end)
    {
      if (end - at < 6)
      {
        fail(at, fmt::format("a sub-chunk header past the end of {}", what));
        return false;
      }
      const auto id = u4(at);
      const auto size = std::size_t(u2(at + 4));
      const auto padded = size + (size & 1);
      if (padded > end - at - 6)
      {
        fail(at, fmt::format("{}'s sub-chunk '{}' passes its end", what, lwidText(id)));
        return false;
      }
      visit(id, at + 6, size);
      at += 6 + padded;
    }
    return ok();
  }

  // a sub-chunk that allocates seen twice in one block: the first allocation leaks
  bool once(
    std::vector<std::uint32_t>& seen, const std::uint32_t id, const std::size_t at)
  {
    if (std::ranges::find(seen, id) != seen.end())
    {
      fail(
        at,
        fmt::format(
          "a second '{}' sub-chunk in one block (the first's string leaks)",
          lwidText(id)));
      return false;
    }
    seen.push_back(id);
    return true;
  }

  // lwGetEnvelope (:417-591): the index, then sub-chunks to the chunk's end; NAME once
  void envelope(const std::size_t data, const std::size_t size)
  {
    const auto end = data + size;
    auto taken = std::size_t{0};
    if (!vx(data, end, taken))
    {
      fail(data, "an ENVL chunk without its index");
      return;
    }
    auto seen = std::vector<std::uint32_t>{};
    subChunks(
      data + taken,
      end,
      "an ENVL chunk",
      [&](const std::uint32_t id, const std::size_t at, std::size_t) {
        if (id == lwid('N', 'A', 'M', 'E'))
        {
          once(seen, id, at);
        }
      });
  }

  // lwGetClip (:119-352): the index, then the source sub-chunk and the rest to the end
  void clip(const std::size_t data, const std::size_t size)
  {
    if (size < 4)
    {
      fail(data, "a CLIP chunk without its index");
      return;
    }
    subChunks(
      data + 4, data + size, "a CLIP chunk", [](std::uint32_t, std::size_t, std::size_t) {
      });
  }

  // lwGetSurface (:4638-4901): the name (non-empty: strcmp at :3397, FindMaterial) and
  // the source name, then sub-chunks to the chunk's end
  void surface(const std::size_t data, const std::size_t size)
  {
    if (++m_surfaces > MaxSurfaces)
    {
      fail(data, fmt::format("more than {} SURF chunks", MaxSurfaces));
      return;
    }
    const auto end = data + size;
    auto length = std::size_t{0};
    const auto name = s0(data, end, length);
    if (!name || length == 0 || length >= 256)
    {
      fail(
        data,
        "a SURF chunk without a terminated name of 1 to 255 characters (getS0's NULL, "
        "then "
        "strcmp of NULL at :3397)");
      return;
    }
    const auto source = s0(data + *name, end, length);
    if (!source)
    {
      fail(data, "a SURF chunk without a terminated source name");
      return;
    }
    subChunks(
      data + *name + *source,
      end,
      "a SURF chunk",
      [&](const std::uint32_t id, const std::size_t at, const std::size_t sz) {
        if (id == lwid('C', 'O', 'L', 'R'))
        {
          // Model.cpp:2392-2395: the colour x 255 into bytes
          if (sz < 12)
          {
            fail(at, "a COLR sub-chunk shorter than its three floats");
            return;
          }
          for (auto i = 0; i < 3; ++i)
          {
            const auto c = f4(at + 4 * std::size_t(i));
            if (!(c >= 0.0f && c <= 1.0f))
            {
              fail(
                at,
                "a surface colour outside 0..1 (Model.cpp:2392-2394: x 255 into a byte)");
              return;
            }
          }
        }
        else if (id == lwid('B', 'L', 'O', 'K'))
        {
          block(at, sz);
        }
      });
  }

  // a SURF's BLOK (:4785-4818): a texture (lwGetTexture :4360-4410) or a shader
  // (lwGetShader :4420-4524); any other kind is skipped by its size
  void block(const std::size_t data, const std::size_t size)
  {
    const auto end = data + size;
    if (size < 4)
    {
      fail(data, "a BLOK without its type");
      return;
    }
    const auto type = u4(data);
    if (
      type == lwid('I', 'M', 'A', 'P') || type == lwid('P', 'R', 'O', 'C')
      || type == lwid('G', 'R', 'A', 'D'))
    {
      if (size < 6)
      {
        fail(data, "a texture BLOK without its header size");
        return;
      }
      // lwGetTHeader (:3790-3884): the ordinal (non-empty: compare_textures' strcmp,
      // :4538), then sub-chunks to the header's end
      const auto headerSize = std::size_t(u2(data + 4));
      const auto header = data + 6;
      if (headerSize > end - header)
      {
        fail(data, "a texture BLOK's header passes the block");
        return;
      }
      auto length = std::size_t{0};
      const auto ordinal = s0(header, header + headerSize, length);
      if (!ordinal || length == 0)
      {
        fail(
          data,
          "a texture BLOK without a terminated, non-empty ordinal (:4538's strcmp)");
        return;
      }
      if (!subChunks(
            header + *ordinal,
            header + headerSize,
            "a texture header",
            [](std::uint32_t, std::size_t, std::size_t) {}))
      {
        return;
      }
      // the image map, procedural or gradient to the block's end; TMAP's own sub-chunks
      auto seen = std::vector<std::uint32_t>{};
      subChunks(
        header + headerSize,
        end,
        "a texture BLOK",
        [&](const std::uint32_t id, const std::size_t at, const std::size_t sz) {
          if (id == lwid('T', 'M', 'A', 'P'))
          {
            auto tmapSeen = std::vector<std::uint32_t>{};
            subChunks(
              at,
              at + sz,
              "a TMAP",
              [&](const std::uint32_t tid, const std::size_t tat, std::size_t) {
                if (tid == lwid('O', 'R', 'E', 'F'))
                {
                  once(tmapSeen, tid, tat);
                }
              });
          }
          else if (
            (type == lwid('I', 'M', 'A', 'P') && id == lwid('V', 'M', 'A', 'P'))
            || (type == lwid('P', 'R', 'O', 'C') && id == lwid('F', 'U', 'N', 'C'))
            || (type == lwid('G', 'R', 'A', 'D') && (id == lwid('P', 'N', 'A', 'M') || id == lwid('I', 'N', 'A', 'M') || id == lwid('F', 'K', 'E', 'Y') || id == lwid('I', 'K', 'E', 'Y'))))
          {
            once(seen, id, at);
          }
        });
    }
    else if (type == lwid('S', 'H', 'D', 'R'))
    {
      shader(data + 4, end);
    }
  }

  // lwGetShader (:4420-4524) as it reads: the header size and the ordinal (non-empty:
  // compare_shaders' strcmp, :4544), then its header loop, which never ends on a
  // zero-size sub-chunk that is not ENAB (:4444-4458: hsz -= 0); refused unless it
  // reaches ENAB (of 2 bytes); then sub-chunks to the block's end, FUNC once
  void shader(const std::size_t data, const std::size_t end)
  {
    if (end - data < 2)
    {
      fail(data, "a shader BLOK without its header size");
      return;
    }
    auto remaining = static_cast<long long>(u2(data));
    auto length = std::size_t{0};
    const auto ordinal = s0(data + 2, end, length);
    if (!ordinal || length == 0)
    {
      fail(
        data, "a shader BLOK without a terminated, non-empty ordinal (:4544's strcmp)");
      return;
    }
    // :4439-4458: the first header, then while hsz > 0 its size off hsz, ENAB ends it,
    // another is skipped and the next header read
    auto at = data + 2 + *ordinal;
    if (end - at < 6)
    {
      fail(at, "a shader BLOK's header sub-chunk past the block");
      return;
    }
    auto id = u4(at);
    auto size = std::size_t(u2(at + 4));
    at += 6;
    auto enabled = false;
    while (remaining > 0)
    {
      const auto padded = size + (size & 1);
      remaining -= static_cast<long long>(padded);
      if (id == lwid('E', 'N', 'A', 'B'))
      {
        if (size != 2 || end - at < 2)
        {
          fail(at, "a shader BLOK's ENAB of other than 2 bytes");
          return;
        }
        at += 2;
        enabled = true;
        break;
      }
      if (padded == 0)
      {
        fail(
          at,
          "a zero-size sub-chunk in a shader BLOK's header (:4447: the loop never ends)");
        return;
      }
      if (padded > end - at || end - at - padded < 6)
      {
        fail(at, "a shader BLOK's header sub-chunk passes the block");
        return;
      }
      at += padded;
      id = u4(at);
      size = std::size_t(u2(at + 4));
      at += 6;
    }
    if (!enabled)
    {
      fail(at, "a shader BLOK's header without ENAB (lwGetShader reads on misaligned)");
      return;
    }
    auto seen = std::vector<std::uint32_t>{};
    subChunks(
      at,
      end,
      "a shader BLOK",
      [&](const std::uint32_t id, const std::size_t sat, std::size_t) {
        if (id == lwid('F', 'U', 'N', 'C'))
        {
          once(seen, id, sat);
        }
      });
  }

  // the post-processing (:2019-2045) and the conversion's reads (Model.cpp:2109-2529)
  void finish()
  {
    const auto first = m_layers.front();
    if (m_anyPolygons && m_tags == 0)
    {
      fail(
        0,
        "polygons without a TAGS chunk (lwGetVertNormals reads a NULL surface, :3468)");
      return;
    }
    if (m_surfaceTag && *m_surfaceTag >= m_tags)
    {
      fail(
        0,
        fmt::format(
          "a PTAG surface tag {} past the {} tags (lwResolvePolySurfaces' off-by-one, "
          ":3411)",
          *m_surfaceTag,
          m_tags));
      return;
    }
    for (std::size_t i = 0; i < m_layers.size(); ++i)
    {
      const auto& l = m_layers[i];
      if (l.vmapPoint && *l.vmapPoint >= l.points)
      {
        fail(
          0,
          fmt::format(
            "layer {}: a VMAP point {} past its {} points (:5110, :5133)",
            i,
            *l.vmapPoint,
            l.points));
        return;
      }
      if (l.vmadPolygon && *l.vmadPolygon >= l.polygons)
      {
        fail(
          0,
          fmt::format(
            "layer {}: a VMAD polygon {} past its {} polygons (:5174, :5207)",
            i,
            *l.vmadPolygon,
            l.polygons));
        return;
      }
    }
    // Model.cpp:2212-2216: without points it returns after adding the surfaces, which
    // FinishSurfaces then finds without geometry (:1241, UNSAFE after the Error)
    if ((m_surfaces > 0 || m_anyPolygons) && first.points == 0)
    {
      fail(0, "surfaces without points in the first layer (Model.cpp:2212-2216)");
      return;
    }
    if (first.polygons > MaxFirstLayerPolygons)
    {
      fail(
        0,
        fmt::format("more than {} polygons in the first layer", MaxFirstLayerPolygons));
      return;
    }
    if (3 * first.triangles > MaxObjCorners)
    {
      fail(
        0,
        fmt::format(
          "more than {} triangle corners (the conversion's 16-bit indexes, "
          "Model.cpp:2477)",
          MaxObjCorners));
    }
  }

  std::string_view m_file;
  std::optional<std::string> m_problem;
  std::vector<Layer> m_layers;
  std::size_t m_nlayers = 0;
  std::size_t m_tags = 0;
  std::size_t m_tagOffset = 0;
  std::size_t m_surfaces = 0;
  bool m_anyPolygons = false;
  std::optional<std::size_t> m_surfaceTag;
};

} // namespace

std::optional<std::string> lwoFileProblem(const std::string_view bytes)
{
  if (bytes.size() >= std::size_t(INT_MAX))
  {
    return "the file is 2 GB or larger";
  }
  return LwoWalk{bytes}.run();
}

} // namespace tb::lit
