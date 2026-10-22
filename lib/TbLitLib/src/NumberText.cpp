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
// GPL Source Code and RBDOOM-3-BFG (neo/idlib/Token.cpp and idlib/Lexer.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2012-2024 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General
// Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "lit/NumberText.h"

#if defined(__APPLE__)
#include "fast_float/fast_float.h"
#endif

#include <charconv>
#include <cstddef>
#include <system_error>

namespace tb::lit
{
namespace
{

template <typename T>
const char* parseChars(const char* first, const char* last, T& value)
{
#if defined(__APPLE__)
  const auto [end, error] = fast_float::from_chars(first, last, value);
#else
  const auto [end, error] = std::from_chars(first, last, value);
#endif
  return error == std::errc{} ? end : nullptr;
}

bool isDigit(const char c)
{
  return c >= '0' && c <= '9';
}

// Lexer.cpp:1002-1024: up to two integer suffixes
std::size_t skipIntegerSuffix(const std::string_view text, std::size_t p)
{
  for (auto i = 0; i < 2 && p < text.size(); ++i)
  {
    const auto c = text[p];
    if (c != 'l' && c != 'L' && c != 'u' && c != 'U')
    {
      break;
    }
    ++p;
  }
  return p;
}

// Token.cpp:69-120, NumberValue of a TT_FLOAT token without float exceptions
double floatTokenValue(const std::string_view token)
{
  auto floatvalue = 0.0;
  auto p = std::size_t{0};
  const auto at = [&](const std::size_t i) { return i < token.size() ? token[i] : '\0'; };
  while (at(p) && at(p) != '.' && at(p) != 'e')
  {
    floatvalue = floatvalue * 10.0 + double(at(p) - '0');
    ++p;
  }
  if (at(p) == '.')
  {
    ++p;
    for (auto m = 0.1; at(p) && at(p) != 'e'; ++p)
    {
      floatvalue = floatvalue + double(at(p) - '0') * m;
      m *= 0.1;
    }
  }
  if (at(p) == 'e')
  {
    ++p;
    auto div = false;
    if (at(p) == '-')
    {
      div = true;
      ++p;
    }
    else if (at(p) == '+')
    {
      ++p;
    }
    auto pow = 0;
    for (; at(p); ++p)
    {
      pow = pow * 10 + int(at(p) - '0');
    }
    auto m = 1.0;
    for (auto i = 0; i < pow; ++i)
    {
      m *= 10.0;
    }
    if (div)
    {
      floatvalue /= m;
    }
    else
    {
      floatvalue *= m;
    }
  }
  return floatvalue;
}

} // namespace

const char* parseFloatChars(const char* first, const char* last, float& value)
{
  return parseChars(first, last, value);
}

const char* parseFloatChars(const char* first, const char* last, double& value)
{
  return parseChars(first, last, value);
}

std::optional<float> lexerFloat(const std::string_view text)
{
  const auto at = [&](const std::size_t i) { return i < text.size() ? text[i] : '\0'; };
  auto p = std::size_t{0};

  // Lexer.cpp ParseFloat: a "-" punctuation token, then the number, negated as a float
  const auto negative = at(p) == '-';
  if (negative)
  {
    ++p;
  }
  // ReadToken: a number starts with a digit, or a '.' before a digit
  if (!isDigit(at(p)) && !(at(p) == '.' && isDigit(at(p + 1))))
  {
    return std::nullopt;
  }

  auto value = 0.0;
  if (at(p) == '0' && at(p + 1) != '.')
  {
    // ReadNumber's octal integer (Token.cpp:160-170); hexadecimal and binary are not read
    // here
    if (at(p + 1) == 'x' || at(p + 1) == 'X' || at(p + 1) == 'b' || at(p + 1) == 'B')
    {
      return std::nullopt;
    }
    auto intvalue = 0u;
    for (++p; at(p) >= '0' && at(p) <= '7'; ++p)
    {
      intvalue = (intvalue << 3) + unsigned(at(p) - '0');
    }
    p = skipIntegerSuffix(text, p);
    value = double(intvalue);
  }
  else
  {
    // ReadNumber: a decimal integer or a floating point number (one dot, or an 'e')
    const auto begin = p;
    auto dots = 0;
    for (; isDigit(at(p)) || at(p) == '.'; ++p)
    {
      dots += at(p) == '.' ? 1 : 0;
    }
    if (at(p) == 'e' && dots == 0)
    {
      dots = 1;
    }
    if (dots > 1)
    {
      // an IP address, refused without LEXFL_ALLOWIPADDRESSES
      return std::nullopt;
    }
    if (dots == 1)
    {
      if (at(p) == 'e')
      {
        ++p;
        if (at(p) == '-' || at(p) == '+')
        {
          ++p;
        }
        while (isDigit(at(p)))
        {
          ++p;
        }
      }
      else if (at(p) == '#')
      {
        // 1.#INF and the like: an error without LEXFL_ALLOWFLOATEXCEPTIONS
        return std::nullopt;
      }
      value = floatTokenValue(text.substr(begin, p - begin));
      // the precision suffixes are skipped, not part of the token
      if (at(p) == 'f' || at(p) == 'F' || at(p) == 'l' || at(p) == 'L')
      {
        ++p;
      }
    }
    else
    {
      // Token.cpp:123-131: TT_DECIMAL, an unsigned int
      auto intvalue = 0u;
      for (auto i = begin; i < p; ++i)
      {
        intvalue = intvalue * 10 + unsigned(text[i] - '0');
      }
      p = skipIntegerSuffix(text, p);
      value = double(intvalue);
    }
  }
  if (p != text.size())
  {
    return std::nullopt;
  }

  // GetFloatValue: ( float ) floatvalue
  const auto result = float(value);
  return negative ? -result : result;
}

} // namespace tb::lit
