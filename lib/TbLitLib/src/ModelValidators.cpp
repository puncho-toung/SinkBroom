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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Model_obj.cpp and idlib/Lexer.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "ModelValidators.h"

#include <fmt/format.h>

#include <array>
#include <climits>
#include <cstdint>
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

} // namespace tb::lit
