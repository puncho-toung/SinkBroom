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

// The lexer facade (lit/EngineLexer.h) over the copied idLexer.

// clang-format off: precompiled.h comes first in every shim file
#include "precompiled.h"
#include "lit/EngineLexer.h"
// clang-format on

namespace tb::lit
{

struct EngineLexer::Impl
{
  std::string text;
  std::string name;
  idLexer lexer{LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE};
  bool failed = false;
};

EngineLexer::EngineLexer(const std::string_view text, const std::string_view name)
  : m_impl{std::make_unique<Impl>()}
{
  m_impl->text = std::string{text};
  m_impl->name = std::string{name};
  m_impl->lexer.LoadMemory(
    m_impl->text.c_str(), int(m_impl->text.size()), m_impl->name.c_str());
}

EngineLexer::~EngineLexer() = default;

bool EngineLexer::readToken(std::string& token)
{
  auto t = idToken{};
  if (!m_impl->lexer.ReadToken(&t))
  {
    return false;
  }
  token.assign(t.c_str(), std::size_t(t.Length()));
  return true;
}

bool EngineLexer::expectTokenString(const std::string_view text)
{
  const auto expected = std::string{text};
  if (!m_impl->lexer.ExpectTokenString(expected.c_str()))
  {
    m_impl->failed = true;
    return false;
  }
  return true;
}

bool EngineLexer::expectAnyToken(std::string& token)
{
  auto t = idToken{};
  if (!m_impl->lexer.ExpectAnyToken(&t))
  {
    m_impl->failed = true;
    return false;
  }
  token.assign(t.c_str(), std::size_t(t.Length()));
  return true;
}

int EngineLexer::parseInt()
{
  return m_impl->lexer.ParseInt();
}

float EngineLexer::parseFloat()
{
  auto error = false;
  const auto value = m_impl->lexer.ParseFloat(&error);
  m_impl->failed = m_impl->failed || error;
  return value;
}

bool EngineLexer::parse1DMatrix(const int count, float* values)
{
  if (!m_impl->lexer.Parse1DMatrix(count, values))
  {
    m_impl->failed = true;
    return false;
  }
  return true;
}

bool EngineLexer::skipBracedSection()
{
  if (!m_impl->lexer.SkipBracedSection())
  {
    m_impl->failed = true;
    return false;
  }
  return true;
}

bool EngineLexer::hadError() const
{
  return m_impl->failed || m_impl->lexer.HadError();
}

int EngineLexer::line() const
{
  return m_impl->lexer.GetLineNum();
}

std::optional<float> engineLexFloat(const std::string_view text)
{
  auto lexer = EngineLexer{text, "number"};
  const auto value = lexer.parseFloat();
  auto rest = std::string{};
  if (lexer.hadError() || lexer.readToken(rest))
  {
    return std::nullopt;
  }
  return value;
}

} // namespace tb::lit
