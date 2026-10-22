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

#pragma once

// The copied idLexer behind a std-only facade, for the .proc and .bproc readers (D64). It
// compiles as C++17 and C++20.
//
// Threading (C20): the parse thread only, as every copied file and decl code. The copied
// lexer fills its static punctuation tables on first use (mirror.json M-ID-STATIC-12),
// and its warnings and errors go to the sink of D63 for the calling thread, which the
// parse thread drains after each work item.

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{

class EngineLexer
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  /**
   * idLexer::LoadMemory of text with the flags InitFromMap reads a .proc with
   * (RenderWorld_load.cpp: LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE). name is the
   * file's name in the lexer's messages. The text is copied.
   */
  EngineLexer(std::string_view text, std::string_view name);
  ~EngineLexer();

  EngineLexer(const EngineLexer&) = delete;
  EngineLexer& operator=(const EngineLexer&) = delete;

  // idLexer's functions of the same names; false or 0 where the lexer fails
  bool readToken(std::string& token);
  bool expectTokenString(std::string_view text);
  bool expectAnyToken(std::string& token);
  int parseInt();
  float parseFloat();
  bool parse1DMatrix(int count, float* values);
  bool skipBracedSection();
  // idLexer::HadError, or a failed expectation
  bool hadError() const;
  int line() const;
};

// the float the copied idLexer makes of a number's text (ParseFloat), or null when the
// text is not one number; parse thread (C20)
std::optional<float> engineLexFloat(std::string_view text);

} // namespace tb::lit
