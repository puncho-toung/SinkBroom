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

#include "lit/EngineGeometry.h"
#include "lit/EngineLexer.h"
#include "lit/MaterialDecls.h"
#include "lit/NumberText.h"
#include "lit/ProcFile.h"

#include <fmt/format.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

std::uint32_t bitsOf(const float value)
{
  return std::bit_cast<std::uint32_t>(value);
}

bool sameFloat(const std::optional<float>& lhs, const std::optional<float>& rhs)
{
  return lhs.has_value() == rhs.has_value() && (!lhs || bitsOf(*lhs) == bitsOf(*rhs));
}

/**
 * The texts the comparison reads: the forms a map (fmt's shortest "{}" of doubles and
 * floats) and a .proc file (dmap's WriteFloat) have, doubles at float midpoints, random
 * digits with a dot and an exponent, integers past 32 bits, and edge cases, also texts
 * both refuse. White space and hexadecimal are outside lexerFloat's domain.
 */
std::vector<std::string> numberTexts(const std::size_t count, const std::uint64_t seed)
{
  auto texts = std::vector<std::string>{
    "0",
    "-0",
    "0.0",
    "-0.0",
    "1",
    "-1",
    "0.5",
    ".5",
    "-.5",
    "5.",
    "1e5",
    "1e+20",
    "1e-05",
    "1.5e",
    "1.5e-",
    "2e+2",
    "0.7312715351581573",
    "4294967295",
    "4294967296",
    "18446744073709551616",
    "007",
    "0123",
    "123.456f",
    "7u",
    "7ul",
    "1.0L",
    "3.4028235e+38",
    "1e39",
    "1e-46",
    "1e-308",
    "2.2250738585072014e-308",
    "0.1",
    "0.2",
    "0.3",
    "16.0004",
    "-16.0004",
    "8.25",
    "0.0039215689",
    "",
    "-",
    "+1",
    "1.2.3",
    "08",
    "05.3",
    "0e5",
    "1e5.5",
    "abc",
    "--1",
    "1-",
    "nan",
    "inf",
  };
  auto random = std::mt19937_64{seed};
  auto unit = std::uniform_real_distribution<double>{0.0, 1.0};
  const auto digit = [&] { return char('0' + int(random() % 10)); };
  while (texts.size() < count)
  {
    switch (texts.size() % 6)
    {
    case 0: {
      // a double of the serialiser, over many magnitudes
      const auto exponent = int(random() % 25) - 12;
      auto value = unit(random) * std::pow(10.0, exponent);
      texts.push_back(fmt::format("{}", random() % 2 ? -value : value));
      break;
    }
    case 1: {
      // a float of the serialiser, any finite one
      const auto value = std::bit_cast<float>(std::uint32_t(random()));
      texts.push_back(std::isfinite(value) ? fmt::format("{}", value) : "0");
      break;
    }
    case 2: {
      // what dmap writes
      const auto exponent = int(random() % 24) - 8;
      texts.push_back(
        engineProcFloatText(float((unit(random) - 0.5) * std::ldexp(1.0, exponent))));
      break;
    }
    case 3: {
      // a double halfway between two floats, where double rounding shows
      const auto value = float(unit(random) * 1000.0);
      const auto next = std::nextafter(value, 2000.0f);
      texts.push_back(fmt::format("{}", (double(value) + double(next)) * 0.5));
      break;
    }
    case 4: {
      // digits, a dot and digits, an exponent
      auto text = std::string{random() % 2 ? "-" : ""};
      for (auto i = std::uint64_t{0}; i < 1 + random() % 12; ++i)
      {
        text += digit();
      }
      if (random() % 3 != 0)
      {
        text += '.';
        for (auto i = std::uint64_t{0}; i < random() % 12; ++i)
        {
          text += digit();
        }
      }
      if (random() % 4 == 0)
      {
        text += 'e';
        if (random() % 2)
        {
          text += random() % 2 ? '-' : '+';
        }
        for (auto i = std::uint64_t{0}; i < 1 + random() % 2; ++i)
        {
          text += digit();
        }
      }
      texts.push_back(std::move(text));
      break;
    }
    default:
      // an integer of up to 13 digits, past 32 bits
      texts.push_back(std::to_string(random() % 10'000'000'000'000ull));
      break;
    }
  }
  return texts;
}

/**
 * The copied idLexer (engineLexFloat) and its mirror on every text, on a thread of its
 * own that runs the copied code as the parse thread does (C20); the texts they differ on.
 */
std::vector<std::string> lexerMismatches(const std::vector<std::string>& texts)
{
  auto mismatches = std::vector<std::string>{};
  auto thread = std::thread{[&] {
    for (const auto& text : texts)
    {
      if (!sameFloat(engineLexFloat(text), lexerFloat(text)))
      {
        mismatches.push_back(text);
      }
    }
    // the lexer's errors on the refused texts
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  }};
  thread.join();
  return mismatches;
}

// procFloat (the copied lexer) and procReadBack (the mirror) on random floats
std::size_t readBackMismatches(const std::size_t count, const std::uint64_t seed)
{
  auto mismatches = std::size_t{0};
  auto thread = std::thread{[&] {
    auto random = std::mt19937_64{seed};
    for (auto i = std::size_t{0}; i < count; ++i)
    {
      // half floats as st, positions, values near integers, any float
      const auto bits = std::uint32_t(random());
      auto value = std::bit_cast<float>(bits);
      switch (i % 4)
      {
      case 0:
        value = engineF16toF32(std::uint16_t(bits));
        break;
      case 1:
        value = float(int(bits % 20000) - 10000) / 8.0f + float(bits % 7) * 0.0001f;
        break;
      case 2:
        value = float(int(bits % 2000) - 1000) + (float(bits % 3) - 1.0f) * 0.0009f;
        break;
      default:
        break;
      }
      if (!std::isfinite(value))
      {
        continue;
      }
      if (bitsOf(procFloat(value)) != bitsOf(procReadBack(value)))
      {
        ++mismatches;
      }
    }
    static_cast<void>(drainEngineMessagesForThread(std::this_thread::get_id()));
  }};
  thread.join();
  return mismatches;
}

} // namespace

TEST_CASE("NumberText: parseFloatChars (R1.1)")
{
  const auto text = std::string{"-1.5e2x"};
  auto f = 0.0f;
  const auto* end = parseFloatChars(text.data(), text.data() + text.size(), f);
  REQUIRE(end == text.data() + 6);
  CHECK(f == -150.0f);
  auto d = 0.0;
  CHECK(parseFloatChars(text.data(), text.data() + text.size(), d) == end);
  CHECK(d == -150.0);

  // no number, or a leading '+': null, the value as it was
  const auto none = std::string{"+1"};
  auto unchanged = 7.0f;
  CHECK(parseFloatChars(none.data(), none.data() + none.size(), unchanged) == nullptr);
  CHECK(unchanged == 7.0f);
  // correctly rounded: the double of a float midpoint is not the float's double
  const auto mid = std::string{"0.7312715351581573"};
  CHECK(parseFloatChars(mid.data(), mid.data() + mid.size(), f) != nullptr);
  CHECK(parseFloatChars(mid.data(), mid.data() + mid.size(), d) != nullptr);
}

TEST_CASE("NumberText: lexerFloat is the copied idLexer's ParseFloat (R3.10)")
{
  // the review's case: a double at a float midpoint; the engine's digit arithmetic lands
  // above, a correctly rounded read below
  const auto engine = lexerFloat("0.7312715351581573");
  REQUIRE(engine);
  CHECK(bitsOf(*engine) == bitsOf(0.7312715649604797f));
  CHECK(bitsOf(*engine) != bitsOf(float(std::stod("0.7312715351581573"))));
  CHECK(lexerFloat("-0") == -0.0f);
  CHECK(std::signbit(*lexerFloat("-0")));
  CHECK(lexerFloat("4294967296") == 0.0f);
  CHECK(lexerFloat("007") == 7.0f);
  CHECK(lexerFloat("08") == std::nullopt);
  CHECK(lexerFloat(" 1") == std::nullopt);

  SECTION("equal to the copied lexer on 50,000 texts")
  {
    const auto texts = numberTexts(50'000, 1);
    const auto mismatches = lexerMismatches(texts);
    for (auto i = std::size_t{0}; i < std::min(mismatches.size(), std::size_t{10}); ++i)
    {
      UNSCOPED_INFO("differs: '" << mismatches[i] << "'");
    }
    CHECK(mismatches.empty());
  }

  SECTION("procReadBack equals procFloat (R3.9) on 20,000 floats")
  {
    CHECK(readBackMismatches(20'000, 2) == 0u);
  }
}

TEST_CASE("NumberText: lexerFloat on 1,000,000 texts", "[.][numbers]")
{
  const auto texts = numberTexts(1'000'000, 3);
  const auto start = std::chrono::steady_clock::now();
  const auto mismatches = lexerMismatches(texts);
  for (auto i = std::size_t{0}; i < std::min(mismatches.size(), std::size_t{10}); ++i)
  {
    UNSCOPED_INFO("differs: '" << mismatches[i] << "'");
  }
  CHECK(mismatches.empty());
  CHECK(readBackMismatches(1'000'000, 4) == 0u);
  std::cout << fmt::format(
    "lexerFloat against the copied idLexer: {} texts, {} mismatches, {:.1f} s\n",
    texts.size(),
    mismatches.size(),
    std::chrono::duration<double>{std::chrono::steady_clock::now() - start}.count());
}

} // namespace tb::lit
