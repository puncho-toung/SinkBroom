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

#include <optional>
#include <string_view>

namespace tb::lit
{

/**
 * R1.1: the floating-point number at the start of [first, last), in std::from_chars'
 * syntax (the "C" locale's, whatever the process locale: no leading white space, no '+'),
 * correctly rounded to the type. Apple's libc++ has no floating-point from_chars, so
 * there it is fast_float::from_chars, as in KdLib. Returns the end of the number, or null
 * when no number starts at first (value is then unchanged), also for one out of range.
 * Pure; any thread.
 */
const char* parseFloatChars(const char* first, const char* last, float& value);
const char* parseFloatChars(const char* first, const char* last, double& value);

/**
 * R3.10: idLexer::ParseFloat of text that holds one number and nothing else, as a map or
 * a .proc file has it: an optional '-' (a punctuation token, negated after the cast),
 * then one number token as idLexer::ReadNumber reads it (decimal digits with at most one
 * '.', an optional exponent e[+-]digits, an optional f/F/l/L suffix; an integer with up
 * to two l/L/u/U suffixes; an octal integer after a leading '0'), whose value is
 * idToken's NumberValue in double arithmetic (Token.cpp:69-131, :160-170: digits
 * accumulated as v * 10 + d, the fraction as v + d * m with m *= 0.1, the exponent as a
 * product of tens; an integer in 32 unsigned bits), and GetFloatValue's float of it. Null
 * for anything else (white space, hexadecimal, an IP address, a float exception, more
 * text after the number).
 *
 * A transcription in plain C++, so that the GUI thread and the CPU pool can read numbers
 * as the engine does without the copied lexer, which only the parse thread runs (C20);
 * the tests prove it equal to the copied idLexer.
 */
std::optional<float> lexerFloat(std::string_view text);

} // namespace tb::lit
