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

// A1.11: the private StackThread (src/StackThread.h), tested directly

#include "StackThread.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

constexpr auto StackBytes = std::size_t{8} * 1024 * 1024;

// a frame of most of the stack, written at both ends so that every page of it is touched
int deepFrame()
{
  volatile char frame[6 * 1024 * 1024];
  frame[0] = 1;
  frame[sizeof(frame) - 1] = 2;
  return frame[0] + frame[sizeof(frame) - 1];
}

} // namespace

TEST_CASE("StackThread")
{
  auto thread = StackThread{};
  CHECK_FALSE(thread.joinable());

  SECTION("runs the body on a thread of its own, which join waits for")
  {
    auto ran = std::thread::id{};
    REQUIRE(
      thread.start([&] { ran = std::this_thread::get_id(); }, StackBytes).is_success());
    CHECK(thread.joinable());
    thread.join();
    CHECK_FALSE(thread.joinable());
    CHECK(ran != std::thread::id{});
    CHECK(ran != std::this_thread::get_id());
  }

  SECTION("has the stack it was given, whatever the executable reserves (G13)")
  {
    auto result = 0;
    REQUIRE(thread.start([&] { result = deepFrame(); }, StackBytes).is_success());
    thread.join();
    CHECK(result == 3);
  }

  SECTION("an exception that leaves the body ends the thread, not the process (W.5)")
  {
    auto reason = std::string{};
    REQUIRE(thread
              .start(
                [] { throw std::runtime_error{"the body failed"}; },
                StackBytes,
                [&](const char* what) { reason = what; })
              .is_success());
    thread.join();
    CHECK(reason == "the body failed");
  }

  SECTION("an escape handler that throws is dropped")
  {
    REQUIRE(thread
              .start(
                [] { throw 7; },
                StackBytes,
                [](const char*) { throw std::runtime_error{"the handler failed"}; })
              .is_success());
    thread.join();
    CHECK_FALSE(thread.joinable());
  }

  thread.join();
}

} // namespace tb::lit
