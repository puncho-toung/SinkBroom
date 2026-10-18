/*
 Copyright (C) 2010 Kristian Duske

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

#include "Matchers.h"
#include "TestEnvironment.h"
#include "base/Logger.h"
#include "fs/DiskFileSystem.h"
#include "fs/TestEnvironment.h"
#include "mdl/CatchConfig.h"
#include "mdl/LoadImageTexture.h"
#include "mdl/MaterialUtils.h"

#include "kd/result.h"
#include "kd/result_io.h" // IWYU pragma: keep

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace tb::mdl
{
namespace
{

// Keeps every message; the model loaders log on several threads.
class CollectingLogger : public Logger
{
private:
  mutable std::mutex m_mutex;
  std::vector<std::string> m_messages;

public:
  std::vector<std::string> messages() const
  {
    const auto lock = std::lock_guard{m_mutex};
    return m_messages;
  }

private:
  void doLog(const LogLevel, const std::string_view message) override
  {
    const auto lock = std::lock_guard{m_mutex};
    m_messages.emplace_back(message);
  }
};

} // namespace

TEST_CASE("MaterialUtils")
{
  SECTION("getMaterialNameFromPathSuffix")
  {
    using T = std::tuple<size_t, std::filesystem::path, std::string>;

    const auto [prefixLength, path, expectedResult] = GENERATE(values<T>({
      {1, "", ""},
      {1, "textures", ""},
      {1, "textures/e1m1", "e1m1"},
      {1, "textures/e1m1/haha", "e1m1/haha"},
      {1, "textures/e1m1/haha.jpg", "e1m1/haha"},
      {1, "textures/nesting/e1m1/haha.jpg", "nesting/e1m1/haha"},
      {2, "textures/nesting/e1m1/haha.jpg", "e1m1/haha"},
      {3, "/textures/nesting/e1m1/haha.jpg", "e1m1/haha"},
    }));

    CAPTURE(prefixLength, path);

    CHECK(getMaterialNameFromPathSuffix(path, prefixLength) == expectedResult);
  }

  SECTION("findMaterialFile")
  {
    auto env = fs::TestEnvironment{};
    env.createDirectory("textures");
    env.createFile("textures/test.png", "");
    env.createFile("textures/test.jpg", "");
    env.createFile("textures/other.txt", "");
    env.createFile("textures/other.png", "");
    env.createFile("textures/mixed.skin", "");
    env.createFile("textures/mixed.jpg", "");

    const auto extensions = std::vector<std::filesystem::path>{".png", ".jpg"};

    auto diskFS = fs::DiskFileSystem{env.dir()};
    // The parent directory does not exist, so there is nothing to search.
    CHECK(findMaterialFile(diskFS, "asdf/test.png", extensions).is_error());
    // The directory exists, but it contains no file with a matching base name.
    CHECK(findMaterialFile(diskFS, "textures/missing.png", extensions).is_error());
    CHECK(
      findMaterialFile(diskFS, "textures/test.png", extensions)
      == Result<std::filesystem::path>{std::filesystem::path{"textures/test.png"}});
    CHECK_THAT(
      findMaterialFile(diskFS, "textures/test.tga", extensions),
      MatchesAnyOf(std::vector{
        Result<std::filesystem::path>{std::filesystem::path{"textures/test.png"}},
        Result<std::filesystem::path>{std::filesystem::path{"textures/test.jpg"}},
      }));
    CHECK(
      findMaterialFile(diskFS, "textures/other.png", extensions)
      == Result<std::filesystem::path>{std::filesystem::path{"textures/other.png"}});
    CHECK(
      findMaterialFile(diskFS, "textures/mixed.skin", extensions)
      == Result<std::filesystem::path>{std::filesystem::path{"textures/mixed.jpg"}});
  }

  SECTION("loadDefaultTexture")
  {
    auto fs =
      fs::DiskFileSystem{getFixtureRoot() / "test/mdl/MaterialUtils/loadDefaultMaterial"};

    SECTION("loads the default image on several threads at once")
    {
      // the model loaders call this on their worker threads
      constexpr auto threadCount = size_t(8);
      constexpr auto loadsPerThread = size_t(16);

      auto logger = CollectingLogger{};
      auto waitingCount = std::atomic<size_t>{0};
      auto loadedCounts = std::vector<size_t>(threadCount, 0u);

      auto threads = std::vector<std::thread>{};
      for (size_t i = 0; i < threadCount; ++i)
      {
        threads.emplace_back([&, i]() {
          // start together, so that the loads overlap
          ++waitingCount;
          while (waitingCount < threadCount)
          {
            std::this_thread::yield();
          }

          for (size_t j = 0; j < loadsPerThread; ++j)
          {
            // the placeholder that a failed load returns is 32x32 too, but has no image
            const auto texture = loadDefaultTexture(fs, logger);
            if (
              texture.width() == 32u && texture.height() == 32u
              && texture.buffersIfLoaded().size() == 1u)
            {
              ++loadedCounts[i];
            }
          }
        });
      }
      for (auto& thread : threads)
      {
        thread.join();
      }

      CHECK(logger.messages() == std::vector<std::string>{});
      CHECK(loadedCounts == std::vector<size_t>(threadCount, loadsPerThread));
    }
  }

  SECTION("loadDefaultMaterial")
  {
    auto logger = NullLogger{};
    auto fs =
      fs::DiskFileSystem{getFixtureRoot() / "test/mdl/MaterialUtils/loadDefaultMaterial"};

    auto material = loadDefaultMaterial(fs, "some_name", logger);
    CHECK(material.name() == "some_name");
  }

  SECTION("makeReadTextureErrorHandler")
  {
    auto logger = NullLogger{};
    auto diskFS = fs::DiskFileSystem{
      getFixtureRoot() / "test/mdl/MaterialUtils/makeReadTextureErrorHandler"};

    const auto file = diskFS.openFile("textures/corruptPngTest.png") | kdl::value();
    auto reader = file->reader().buffer();
    auto result = loadImageTexture(reader);
    REQUIRE(result.is_error());

    const auto defaultTexture =
      std::move(result) | kdl::or_else(makeReadTextureErrorHandler(diskFS, logger))
      | kdl::value();
    CHECK(defaultTexture.width() == 32);
    CHECK(defaultTexture.height() == 32);
  }
}

} // namespace tb::mdl
