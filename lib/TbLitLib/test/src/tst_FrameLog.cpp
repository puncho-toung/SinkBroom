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

#include "lit/FrameLog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>

namespace tb::lit
{
namespace
{

FrameLogRow exampleRow()
{
  auto row = FrameLogRow{};
  row.frameIndex = 12;
  row.cpuRecordMs = 0.15349;
  row.gpuClearMs = 0.0214;
  row.presentMode = "mailbox";
  row.swapchainGeneration = 2;
  row.framesSkipped = 3;
  row.width = 800;
  row.height = 600;
  return row;
}

std::string readFile(const std::filesystem::path& path)
{
  auto stream = std::ifstream{path, std::ios::binary};
  auto buffer = std::stringstream{};
  buffer << stream.rdbuf();
  return buffer.str();
}

} // namespace

TEST_CASE("FrameLog")
{
  SECTION("frameLogHeader")
  {
    CHECK(
      frameLogHeader()
      == "frame,cpu_record_ms,gpu_clear_ms,present_mode,swapchain_generation,frames_skipped,"
         "width,height\n");
  }

  SECTION("formatFrameLogRow")
  {
    CHECK(formatFrameLogRow(exampleRow()) == "12,0.153,0.021,mailbox,2,3,800,600\n");

    // a row without its GPU time has an empty field
    auto row = exampleRow();
    row.gpuClearMs.reset();
    row.presentMode = "headless";
    row.swapchainGeneration = 0;
    CHECK(formatFrameLogRow(row) == "12,0.153,,headless,0,3,800,600\n");

    // as many fields as the header
    const auto count = [](const std::string_view line) {
      return std::count(line.begin(), line.end(), ',');
    };
    CHECK(count(formatFrameLogRow(exampleRow())) == count(frameLogHeader()));
  }
}

TEST_CASE("FrameLogWriter")
{
  SECTION("open, append and close")
  {
    const auto path =
      std::filesystem::temp_directory_path() / "TbLitLibTest-frame-log.csv";
    std::filesystem::remove(path);

    auto writer = FrameLogWriter{};
    CHECK_FALSE(writer.open(path));
    CHECK(writer.isOpen());
    writer.append(exampleRow());
    auto second = exampleRow();
    second.frameIndex = 13;
    writer.append(second);
    writer.close();
    CHECK_FALSE(writer.isOpen());

    CHECK(
      readFile(path)
      == std::string{frameLogHeader()} + "12,0.153,0.021,mailbox,2,3,800,600\n"
           + "13,0.153,0.021,mailbox,2,3,800,600\n");

    // appending after close writes nothing
    writer.append(exampleRow());
    std::filesystem::remove(path);
  }

  SECTION("open with an empty path is no log")
  {
    auto writer = FrameLogWriter{};
    CHECK_FALSE(writer.open({}));
    CHECK_FALSE(writer.isOpen());
    writer.append(exampleRow());
    writer.close();
  }

  SECTION("open with an unopenable path is an error message")
  {
    auto writer = FrameLogWriter{};
    const auto error = writer.open(
      std::filesystem::temp_directory_path() / "no-such-directory-TbLitLibTest"
      / "log.csv");
    CHECK(error);
    CHECK_FALSE(writer.isOpen());
  }
}

} // namespace tb::lit
