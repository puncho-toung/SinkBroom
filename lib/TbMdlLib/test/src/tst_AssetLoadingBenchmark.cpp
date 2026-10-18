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

/*
 * A hidden benchmark of model and material loading with a real game. It measures what
 * audit package 6 of 2026-10-17 is about (D-01, C-01, C-02, D-03) and is meant to be run
 * before and after the fixes, in a Release build:
 *
 * - M1: one loadShaderFiles over the game's material scripts;
 * - M2: loading a map, the synchronous part (what blocks the GUI thread) and the time
 *   until its models have loaded; parsing the map's entity definition file, a part of
 *   the synchronous load, is timed again on its own;
 * - M3: loading the models of an FGD's point entity definitions, as the entity browser
 *   does, and counting the texture resources that the models' skins use;
 * - M4: the "Could not load default texture" messages during M2 and M3.
 *
 * Every test case is hidden and skips unless these environment variables are set:
 *   TB_BENCH_GAME_PATH    the game path (the folder that contains base/)
 *   TB_BENCH_GAME_CONFIG  the game's GameConfig.cfg; if it lies in the resources folder
 *                         (resources/games/<game>/GameConfig.cfg), resources/defaults
 *                         is mounted as the app does, which provides the default texture
 *   TB_BENCH_MAP          M2: one map, or several separated by ';'
 *   TB_BENCH_FGD          M3: the entity definition file
 *   TB_BENCH_MODEL_LIMIT  M3, optional: load only the first N distinct model paths
 *   TB_BENCH_REPEAT       optional: the number of runs per measurement (default 3)
 *
 * Resources are pumped as the app does it, with ResourceManager::process, a task runner
 * that runs the loaders on a kdl::task_manager with one worker per logical core, and a
 * no-op GL for the uploads, with a 1 ms sleep between ticks.
 *
 * Results are printed as lines "BENCH <name> <value> <unit>" (medians, minimums and
 * maximums over the runs) and "BENCHRUN <name> <run> <value> <unit>" (single runs).
 */

#include "base/Logger.h"
#include "base/SimpleParserStatus.h"
#include "fs/DiskIO.h"
#include "fs/File.h"
#include "fs/PathInfo.h"
#include "fs/Reader.h"
#include "gl/Material.h"
#include "gl/Resource.h"
#include "gl/ResourceManager.h"
#include "gl/TestGl.h"
#include "gl/Texture.h"
#include "gl/TextureResource.h"
#include "mdl/AssetUtils.h"
#include "mdl/BrushNode.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionFileSpec.h"
#include "mdl/EntityModel.h"
#include "mdl/EntityModelManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EnvironmentConfig.h"
#include "mdl/GameConfig.h"
#include "mdl/GameFileSystem.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/LoadEntityDefinitions.h"
#include "mdl/LoadShaders.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Assets.h"
#include "mdl/ModelDefinition.h"
#include "mdl/ModelSpecification.h"
#include "mdl/ParseGameConfig.h"
#include "mdl/PatchNode.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"
#include "kd/result.h"
#include "kd/string_format.h"
#include "kd/string_utils.h"
#include "kd/task_manager.h"

#include "vm/bbox.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
// clang-format on
#endif

#include <catch2/catch_test_macros.hpp>

namespace tb::mdl
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr auto DefaultTextureMessage = std::string_view{"Could not load default texture"};

double millisecondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::optional<std::string> environmentVariable(const char* name)
{
#ifdef _WIN32
  char* buffer = nullptr;
  auto length = size_t{0};
  if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
  {
    return std::nullopt;
  }
  auto value = std::string{buffer};
  std::free(buffer);
#else
  const auto* buffer = std::getenv(name);
  if (buffer == nullptr)
  {
    return std::nullopt;
  }
  auto value = std::string{buffer};
#endif
  return value.empty() ? std::nullopt : std::optional{std::move(value)};
}

std::optional<size_t> environmentNumber(const char* name)
{
  if (const auto str = environmentVariable(name))
  {
    return kdl::str_to_size(*str);
  }
  return std::nullopt;
}

/** The private bytes of this process, or 0 where they cannot be measured. */
size_t privateBytes()
{
#ifdef _WIN32
  auto counters = PROCESS_MEMORY_COUNTERS_EX{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(
        GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        sizeof(counters)))
  {
    return counters.PrivateUsage;
  }
#endif
  return 0;
}

double toMiB(const size_t bytes)
{
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::string formatValue(const double value)
{
  auto str = std::ostringstream{};
  str << std::fixed << std::setprecision(2) << value;
  return str.str();
}

void report(const std::string& name, const double value, const std::string_view unit)
{
  std::cout << "BENCH " << name << " " << formatValue(value) << " " << unit << std::endl;
}

void reportCount(const std::string& name, const size_t value, const std::string_view unit)
{
  std::cout << "BENCH " << name << " " << value << " " << unit << std::endl;
}

void reportRun(
  const std::string& name,
  const size_t run,
  const double value,
  const std::string_view unit)
{
  std::cout << "BENCHRUN " << name << " " << run << " " << formatValue(value) << " "
            << unit << std::endl;
}

void reportNote(const std::string& note)
{
  std::cout << "BENCHNOTE " << note << std::endl;
}

/** The values of one quantity over the runs of a measurement. */
class Samples
{
private:
  std::string m_name;
  std::string m_unit;
  bool m_isCount;
  std::vector<double> m_values;

public:
  Samples(std::string name, std::string unit, const bool isCount)
    : m_name{std::move(name)}
    , m_unit{std::move(unit)}
    , m_isCount{isCount}
  {
  }

  void add(const size_t run, const double value)
  {
    m_values.push_back(value);
    reportRun(m_name, run, value, m_unit);
  }

  /**
   * Prints the median, the minimum and the maximum, except for a count that was the same
   * in every run, which is printed once.
   */
  void print() const
  {
    if (m_values.empty())
    {
      return;
    }

    auto sorted = m_values;
    std::ranges::sort(sorted);
    const auto n = sorted.size();
    const auto median =
      n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;

    if (m_isCount && sorted.front() == sorted.back())
    {
      reportCount(m_name, static_cast<size_t>(sorted.front()), m_unit);
      return;
    }

    report(m_name + ".median", median, m_unit);
    report(m_name + ".min", sorted.front(), m_unit);
    report(m_name + ".max", sorted.back(), m_unit);
  }
};

/** A set of named samples that prints them in the order they were first added. */
class SampleTable
{
private:
  std::vector<Samples> m_samples;
  std::map<std::string, size_t> m_index;

  Samples& samples(const std::string& name, const std::string& unit, const bool isCount)
  {
    if (const auto it = m_index.find(name); it != m_index.end())
    {
      return m_samples[it->second];
    }
    m_index.emplace(name, m_samples.size());
    return m_samples.emplace_back(name, unit, isCount);
  }

public:
  void time(const std::string& name, const size_t run, const double milliseconds)
  {
    samples(name, "ms", false).add(run, milliseconds);
  }

  void mebibytes(const std::string& name, const size_t run, const size_t bytes)
  {
    samples(name, "MiB", false).add(run, toMiB(bytes));
  }

  void count(
    const std::string& name,
    const size_t run,
    const size_t value,
    const std::string& unit = "count")
  {
    samples(name, unit, true).add(run, static_cast<double>(value));
  }

  void print() const
  {
    for (const auto& samples : m_samples)
    {
      samples.print();
    }
  }
};

/** Counts messages and keeps the first few distinct errors. Loaders log from workers. */
class BenchLogger : public Logger
{
private:
  std::atomic<size_t> m_warnings = 0;
  std::atomic<size_t> m_errors = 0;
  std::atomic<size_t> m_defaultTextureMessages = 0;
  std::atomic<size_t> m_defaultTextureFileMessages = 0;

  mutable std::mutex m_mutex;
  std::vector<std::string> m_firstErrors;

public:
  size_t warnings() const { return m_warnings; }
  size_t errors() const { return m_errors; }

  /** All "Could not load default texture" messages. */
  size_t defaultTextureMessages() const { return m_defaultTextureMessages; }

  /**
   * The ones with a reason ("Could not load default texture: ..."), which mean that the
   * file could not be read, as opposed to the recursion guard having been hit.
   */
  size_t defaultTextureFileMessages() const { return m_defaultTextureFileMessages; }

  std::vector<std::string> firstErrors() const
  {
    const auto lock = std::lock_guard{m_mutex};
    return m_firstErrors;
  }

private:
  void doLog(const LogLevel level, const std::string_view message) override
  {
    if (message.find(DefaultTextureMessage) != std::string_view::npos)
    {
      ++m_defaultTextureMessages;
      if (
        message.find(std::string{DefaultTextureMessage} + ":") != std::string_view::npos)
      {
        ++m_defaultTextureFileMessages;
      }
    }

    switch (level)
    {
    case LogLevel::Warn:
      ++m_warnings;
      break;
    case LogLevel::Error: {
      ++m_errors;
      const auto lock = std::lock_guard{m_mutex};
      if (
        m_firstErrors.size() < 20
        && std::ranges::find(m_firstErrors, message) == m_firstErrors.end())
      {
        m_firstErrors.emplace_back(message);
      }
      break;
    }
    case LogLevel::Debug:
    case LogLevel::Info:
      break;
    }
  }
};

/** Advances the resources of a resource manager the way the app's timer does. */
class ResourcePump
{
private:
  gl::ResourceManager& m_resourceManager;
  kdl::task_manager& m_taskManager;
  gl::TestGl m_gl;
  size_t m_failedResources = 0;
  std::vector<std::string> m_firstFailures;

public:
  ResourcePump(gl::ResourceManager& resourceManager, kdl::task_manager& taskManager)
    : m_resourceManager{resourceManager}
    , m_taskManager{taskManager}
  {
  }

  size_t failedResources() const { return m_failedResources; }
  const std::vector<std::string>& firstFailures() const { return m_firstFailures; }

  template <typename Done, typename OnTick>
  void until(const Done& done, const OnTick& onTick)
  {
    const auto taskRunner = gl::TaskRunner{
      [&](gl::Task task) { return m_taskManager.run_task(std::move(task)); }};
    const auto processContext = gl::ProcessContext{
      m_gl, [&](const gl::ResourceId&, const std::string& error) {
        ++m_failedResources;
        if (
          m_firstFailures.size() < 20
          && std::ranges::find(m_firstFailures, error) == m_firstFailures.end())
        {
          m_firstFailures.push_back(error);
        }
      }};

    while (!done())
    {
      m_resourceManager.process(taskRunner, processContext);
      onTick();
      if (!done())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
    }
  }

  template <typename Done>
  void until(const Done& done)
  {
    until(done, []() {});
  }

  void untilIdle()
  {
    until([&]() { return !m_resourceManager.needsProcessing(); });
  }
};

struct RequestedModel
{
  std::filesystem::path path;
  const EntityModel* model = nullptr;
};

bool isDone(const RequestedModel& requestedModel)
{
  return requestedModel.model == nullptr
         || !requestedModel.model->dataResource().needsProcessing();
}

bool allDone(const std::vector<RequestedModel>& requestedModels)
{
  return std::ranges::all_of(requestedModels, isDone);
}

std::string extensionOf(const std::filesystem::path& path)
{
  const auto extension = kdl::str_to_lower(path.extension().string());
  return extension.empty() ? std::string{"none"} : extension.substr(1);
}

struct ModelStats
{
  size_t requested = 0;
  size_t loaded = 0;
  size_t failed = 0;
  size_t pending = 0;
  size_t objRequested = 0;
  size_t objLoaded = 0;
  size_t surfaces = 0;
  size_t skins = 0;
  size_t distinctTextureResources = 0;
  // width * height * 4 summed over the distinct texture resources: the RGBA8 size of what
  // GL would hold for them (one mip level, as uploaded)
  size_t distinctTextureBytes = 0;
  std::map<std::string, size_t> loadedByExtension;
  std::map<std::string, size_t> failedByExtension;
};

ModelStats collectModelStats(const std::vector<RequestedModel>& requestedModels)
{
  auto stats = ModelStats{};
  auto textureResources = std::unordered_set<const gl::TextureResource*>{};

  for (const auto& requestedModel : requestedModels)
  {
    const auto extension = extensionOf(requestedModel.path);
    const auto isObj = extension == "obj";

    ++stats.requested;
    stats.objRequested += isObj ? 1 : 0;

    const auto* model = requestedModel.model;
    if (const auto* modelData = model ? model->data() : nullptr)
    {
      ++stats.loaded;
      stats.objLoaded += isObj ? 1 : 0;
      ++stats.loadedByExtension[extension];

      for (const auto& surface : modelData->surfaces())
      {
        ++stats.surfaces;
        for (size_t i = 0; i < surface.skinCount(); ++i)
        {
          if (const auto* skin = surface.skin(i))
          {
            ++stats.skins;
            const auto& textureResource = skin->textureResource();
            if (textureResources.insert(&textureResource).second)
            {
              if (const auto* texture = skin->texture())
              {
                stats.distinctTextureBytes += texture->width() * texture->height() * 4;
              }
            }
          }
        }
      }
    }
    else if (isDone(requestedModel))
    {
      ++stats.failed;
      ++stats.failedByExtension[extension];
    }
    else
    {
      ++stats.pending;
    }
  }

  stats.distinctTextureResources = textureResources.size();
  return stats;
}

void addModelStats(
  SampleTable& table,
  const std::string& prefix,
  const size_t run,
  const ModelStats& stats)
{
  table.count(prefix + ".models_requested", run, stats.requested);
  table.count(prefix + ".models_loaded", run, stats.loaded);
  table.count(prefix + ".models_failed", run, stats.failed);
  table.count(prefix + ".models_pending", run, stats.pending);
  table.count(prefix + ".obj_requested", run, stats.objRequested);
  table.count(prefix + ".obj_loaded", run, stats.objLoaded);
  table.count(prefix + ".surfaces", run, stats.surfaces);
  table.count(prefix + ".skins", run, stats.skins);
  table.count(
    prefix + ".distinct_texture_resources", run, stats.distinctTextureResources);
  table.mebibytes(prefix + ".distinct_texture_rgba8", run, stats.distinctTextureBytes);
  for (const auto& [extension, count] : stats.loadedByExtension)
  {
    table.count(prefix + ".loaded." + extension, run, count);
  }
  for (const auto& [extension, count] : stats.failedByExtension)
  {
    table.count(prefix + ".failed." + extension, run, count);
  }
}

void addLoggerStats(
  SampleTable& table,
  const std::string& prefix,
  const size_t run,
  const BenchLogger& logger,
  const size_t defaultTextureMessagesBefore = 0,
  const size_t defaultTextureFileMessagesBefore = 0)
{
  table.count(
    prefix + ".default_texture_msgs",
    run,
    logger.defaultTextureMessages() - defaultTextureMessagesBefore);
  table.count(
    prefix + ".default_texture_msgs_with_reason",
    run,
    logger.defaultTextureFileMessages() - defaultTextureFileMessagesBefore);
  table.count(prefix + ".log_warnings", run, logger.warnings());
  table.count(prefix + ".log_errors", run, logger.errors());
}

void reportFirstMessages(
  const std::string& prefix, const BenchLogger& logger, const ResourcePump& pump)
{
  for (const auto& error : logger.firstErrors())
  {
    reportNote(prefix + " log error: " + error);
  }
  for (const auto& failure : pump.firstFailures())
  {
    reportNote(prefix + " resource failure: " + failure);
  }
}

/** The game, read from the environment, or nullopt if a variable is missing. */
struct BenchGame
{
  std::filesystem::path gamePath;
  GameInfo gameInfo;
  EnvironmentConfig environmentConfig;
  MapFormat mapFormat = MapFormat::Unknown;
  vm::bbox3d worldBounds;
  size_t repeat = 3;
};

std::optional<BenchGame> readBenchGame()
{
  const auto gamePath = environmentVariable("TB_BENCH_GAME_PATH");
  const auto gameConfigPath = environmentVariable("TB_BENCH_GAME_CONFIG");
  if (!gamePath || !gameConfigPath)
  {
    return std::nullopt;
  }

  const auto configPath = std::filesystem::path{*gameConfigPath};
  auto gameConfig = fs::Disk::openFile(configPath) | kdl::and_then([&](auto file) {
                      auto reader = file->reader().buffer();
                      return parseGameConfig(reader.stringView(), configPath);
                    });
  if (!gameConfig.is_success())
  {
    FAIL("Could not read the game config " << configPath);
  }

  auto environmentConfig = EnvironmentConfig{};
  // resources/games/<game>/GameConfig.cfg -> resources/defaults, which holds the default
  // texture, as SystemPaths::findResourceDirectories("defaults") gives it to the app
  const auto defaultsPath =
    configPath.parent_path().parent_path().parent_path() / "defaults";
  const auto hasDefaults =
    fs::Disk::pathInfo(defaultsPath / "assets") == fs::PathInfo::Directory;
  if (hasDefaults)
  {
    environmentConfig.defaultAssetFolderPaths.push_back(defaultsPath);
  }

  auto game = BenchGame{
    .gamePath = std::filesystem::path{*gamePath},
    .gameInfo = makeGameInfo(std::move(gameConfig) | kdl::value()),
    .environmentConfig = std::move(environmentConfig),
  };

  const auto& config = game.gameInfo.gameConfig;
  game.mapFormat = !config.fileFormats.empty()
                     ? formatFromName(config.fileFormats.front().format)
                     : MapFormat::Standard;

  // as MapDocument::worldBoundsForGame
  const auto defaultWorldBounds = vm::bbox3d{8192.0};
  game.worldBounds = config.softMapBounds
                       ? vm::merge(defaultWorldBounds, *config.softMapBounds)
                       : defaultWorldBounds;

  game.repeat = std::max(environmentNumber("TB_BENCH_REPEAT").value_or(3), size_t{1});

  reportCount("setup.default_assets_mounted", hasDefaults ? 1 : 0, "flag");
  reportCount("setup.repeat", game.repeat, "runs");
  reportCount("setup.logical_cores", std::thread::hardware_concurrency(), "count");

  return game;
}

std::vector<RequestedModel> collectMapModels(const Map& map)
{
  auto result = std::vector<RequestedModel>{};
  auto seen = std::unordered_set<const EntityModel*>{};

  map.worldNode().accept(kdl::overload(
    [](auto&& thisLambda, const WorldNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const LayerNode& node) { node.visitChildren(thisLambda); },
    [](auto&& thisLambda, const GroupNode& node) { node.visitChildren(thisLambda); },
    [&](const EntityNode& node) {
      if (const auto* model = node.entity().model(); model && seen.insert(model).second)
      {
        result.push_back(RequestedModel{model->name(), model});
      }
    },
    [](const BrushNode&) {},
    [](const PatchNode&) {}));

  return result;
}

/**
 * Parses the map's entity definition file again and returns how long that took, or
 * nullopt if the map has no such file. This is a part of the synchronous map load.
 */
std::optional<double> timeEntityDefinitionFileParse(const Map& map)
{
  const auto spec = entityDefinitionFile(map);
  if (!spec)
  {
    return std::nullopt;
  }

  const auto& gameConfig = map.gameInfo().gameConfig;
  const auto path = spec->type == EntityDefinitionFileSpec::Type::Builtin
                      ? gameConfig.findConfigFile(spec->path)
                      : spec->path;
  if (!path.is_absolute())
  {
    return std::nullopt;
  }

  auto logger = NullLogger{};
  auto status = SimpleParserStatus{logger};
  const auto start = Clock::now();
  const auto definitions =
    loadEntityDefinitions(path, gameConfig.entityConfig.defaultColor, status);
  const auto elapsed = millisecondsSince(start);

  return definitions.is_success() ? std::optional{elapsed} : std::nullopt;
}

/** The distinct model paths of the point entity definitions, in definition order. */
std::vector<std::filesystem::path> collectDefinitionModelPaths(
  const std::vector<EntityDefinition>& definitions, Logger& logger)
{
  auto result = std::vector<std::filesystem::path>{};
  auto seen = std::unordered_set<std::string>{};

  for (const auto& definition : definitions)
  {
    if (definition.pointEntityDefinition)
    {
      const auto spec = safeGetModelSpecification(logger, definition.name, [&]() {
        return definition.pointEntityDefinition->modelDefinition
          .defaultModelSpecification();
      });
      if (!spec.path.empty() && seen.insert(spec.path.generic_string()).second)
      {
        result.push_back(spec.path);
      }
    }
  }

  return result;
}

} // namespace

TEST_CASE("AssetLoadingBenchmark M1 parse", "[.][benchmark]")
{
  const auto game = readBenchGame();
  if (!game)
  {
    SKIP("TB_BENCH_GAME_PATH and TB_BENCH_GAME_CONFIG are not set");
  }

  auto logger = BenchLogger{};
  auto fileSystem = GameFileSystem{};
  fileSystem.initialize(
    game->environmentConfig, game->gameInfo.gameConfig, game->gamePath, {}, logger);
  auto taskManager = kdl::task_manager{};

  auto table = SampleTable{};
  for (size_t run = 1; run <= game->repeat; ++run)
  {
    const auto start = Clock::now();
    auto result = loadShaderFiles(
      fileSystem, game->gameInfo.gameConfig.materialConfig, taskManager, logger);
    const auto elapsed = millisecondsSince(start);

    REQUIRE(result.is_success());
    const auto& loadedShaders = result.value();
    table.time("m1.parse", run, elapsed);
    table.count("m1.shaders", run, loadedShaders.shaders.size());
    table.count("m1.files", run, loadedShaders.fileCount);
    table.count("m1.unterminated", run, loadedShaders.unterminatedCount);
  }

  table.print();
}

TEST_CASE("AssetLoadingBenchmark M2 map load", "[.][benchmark]")
{
  const auto mapPaths = environmentVariable("TB_BENCH_MAP");
  const auto game = readBenchGame();
  if (!game || !mapPaths)
  {
    SKIP("TB_BENCH_GAME_PATH, TB_BENCH_GAME_CONFIG and TB_BENCH_MAP are not set");
  }

  for (const auto& mapPathStr : kdl::str_split(*mapPaths, ";"))
  {
    const auto mapPath = std::filesystem::path{mapPathStr};
    const auto prefix = "m2." + mapPath.stem().string();
    reportNote(prefix + " map: " + mapPath.generic_string());

    auto table = SampleTable{};
    for (size_t run = 1; run <= game->repeat; ++run)
    {
      auto taskManager = kdl::task_manager{};
      auto resourceManager = gl::ResourceManager{};
      auto logger = BenchLogger{};
      auto pump = ResourcePump{resourceManager, taskManager};

      const auto privateBefore = privateBytes();

      // what blocks the GUI thread: reading the world, then the Map constructor
      // (shaders for the models, entity definitions, model resources, materials)
      const auto start = Clock::now();
      auto mapResult = Map::loadMap(
        game->environmentConfig,
        game->gameInfo,
        game->gamePath,
        game->mapFormat,
        game->worldBounds,
        mapPath,
        taskManager,
        resourceManager,
        logger);
      const auto syncElapsed = millisecondsSince(start);

      REQUIRE(mapResult.is_success());
      auto map = std::move(mapResult) | kdl::value();

      const auto requestedModels = collectMapModels(*map);

      // the models load while the material browser's textures load too, as in the app
      const auto asyncStart = Clock::now();
      pump.until([&]() { return allDone(requestedModels); });
      const auto modelsElapsed = millisecondsSince(asyncStart);

      pump.untilIdle();
      const auto idleElapsed = millisecondsSince(asyncStart);

      table.time(prefix + ".sync", run, syncElapsed);
      table.time(prefix + ".models_after_sync", run, modelsElapsed);
      table.time(prefix + ".all_resources_after_sync", run, idleElapsed);
      addModelStats(table, prefix, run, collectModelStats(requestedModels));
      addLoggerStats(table, prefix, run, logger);
      table.count(prefix + ".resource_failures", run, pump.failedResources());
      table.count(
        prefix + ".resources", run, resourceManager.resources().size(), "count");
      table.mebibytes(prefix + ".private_before", run, privateBefore);
      table.mebibytes(prefix + ".private_after", run, privateBytes());
      if (const auto fgdParseElapsed = timeEntityDefinitionFileParse(*map))
      {
        table.time(prefix + ".part_fgd_parse", run, *fgdParseElapsed);
      }

      if (run == 1)
      {
        reportFirstMessages(prefix, logger, pump);
      }

      map.reset();
    }

    table.print();
  }
}

TEST_CASE("AssetLoadingBenchmark M3 entity browser", "[.][benchmark]")
{
  const auto fgdPath = environmentVariable("TB_BENCH_FGD");
  const auto game = readBenchGame();
  if (!game || !fgdPath)
  {
    SKIP("TB_BENCH_GAME_PATH, TB_BENCH_GAME_CONFIG and TB_BENCH_FGD are not set");
  }

  const auto modelLimit = environmentNumber("TB_BENCH_MODEL_LIMIT");

  auto definitionLogger = BenchLogger{};
  auto status = SimpleParserStatus{definitionLogger};
  auto definitions = loadEntityDefinitions(
    std::filesystem::path{*fgdPath},
    game->gameInfo.gameConfig.entityConfig.defaultColor,
    status);
  REQUIRE(definitions.is_success());

  auto modelPaths = collectDefinitionModelPaths(definitions.value(), definitionLogger);
  const auto distinctModelPaths = modelPaths.size();
  if (modelLimit && *modelLimit < modelPaths.size())
  {
    modelPaths.resize(*modelLimit);
  }

  reportNote("m3 fgd: " + std::filesystem::path{*fgdPath}.generic_string());
  reportCount("m3.definitions", definitions.value().size(), "count");
  reportCount("m3.distinct_model_paths", distinctModelPaths, "count");
  reportCount("m3.model_limit", modelLimit.value_or(0), "count");

  auto table = SampleTable{};
  for (size_t run = 1; run <= game->repeat; ++run)
  {
    auto taskManager = kdl::task_manager{};
    auto resourceManager = gl::ResourceManager{};
    auto logger = BenchLogger{};
    auto pump = ResourcePump{resourceManager, taskManager};

    auto mapResult = Map::createMap(
      game->environmentConfig,
      game->gameInfo,
      game->gamePath,
      game->mapFormat,
      game->worldBounds,
      taskManager,
      resourceManager,
      logger);
    REQUIRE(mapResult.is_success());
    auto map = std::move(mapResult) | kdl::value();

    // let the material browser's textures load first, so that only models load below
    const auto settleStart = Clock::now();
    pump.untilIdle();
    table.time("m3.new_map_settle", run, millisecondsSince(settleStart));

    const auto defaultTextureMessagesBefore = logger.defaultTextureMessages();
    const auto defaultTextureFileMessagesBefore = logger.defaultTextureFileMessages();
    const auto privateBefore = privateBytes();
    auto privatePeak = privateBefore;

    const auto start = Clock::now();
    const auto& entityModelManager = map->entityModelManager();
    auto requestedModels = std::vector<RequestedModel>{};
    requestedModels.reserve(modelPaths.size());
    for (const auto& modelPath : modelPaths)
    {
      requestedModels.push_back(
        RequestedModel{modelPath, entityModelManager.model(modelPath)});
    }
    const auto requestElapsed = millisecondsSince(start);

    pump.until(
      [&]() { return allDone(requestedModels); },
      [&]() { privatePeak = std::max(privatePeak, privateBytes()); });
    const auto elapsed = millisecondsSince(start);
    const auto privateAfter = privateBytes();

    table.time("m3.load", run, elapsed);
    table.time("m3.request_calls", run, requestElapsed);
    addModelStats(table, "m3", run, collectModelStats(requestedModels));
    addLoggerStats(
      table,
      "m3",
      run,
      logger,
      defaultTextureMessagesBefore,
      defaultTextureFileMessagesBefore);
    table.count("m3.resource_failures", run, pump.failedResources());
    table.mebibytes("m3.private_before", run, privateBefore);
    table.mebibytes("m3.private_peak", run, privatePeak);
    table.mebibytes("m3.private_after", run, privateAfter);

    if (run == 1)
    {
      reportFirstMessages("m3", logger, pump);
    }

    pump.untilIdle();
    map.reset();
  }

  table.print();
}

} // namespace tb::mdl
