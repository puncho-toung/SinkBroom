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

#include "TestPreferenceStore.h"
#include "base/PreferenceManager.h"
#include "gl/FontManager.h"
#include "gl/MockGl.h"
#include "gl/OrthographicCamera.h"
#include "gl/PerspectiveCamera.h"
#include "gl/ShaderManager.h"
#include "gl/Shaders.h"
#include "gl/TestUtils.h"
#include "gl/VboManager.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/GameConfig.h"
#include "mdl/Map.h"
#include "mdl/MapFixture.h"
#include "mdl/Map_NodeVisibility.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Selection.h"
#include "render/LightVolumeRenderer.h"
#include "render/RenderBatch.h"
#include "render/RenderContext.h"

#include "kd/filesystem_utils.h"
#include "kd/result.h"

#include "vm/vec.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace tb::render
{
namespace
{

// creates the global PreferenceManager instance and destroys it again when it goes out
// of scope, so no instance leaks into other test cases
struct PreferenceManagerInstance
{
  PreferenceManagerInstance()
  {
    PreferenceManager::createInstance(std::make_unique<TestPreferenceStore>(), true);
  }

  ~PreferenceManagerInstance() { PreferenceManager::destroyInstance(); }
};

std::string capName(const GLenum cap)
{
  switch (cap)
  {
  case GL_DEPTH_TEST:
    return "GL_DEPTH_TEST";
  case GL_CULL_FACE:
    return "GL_CULL_FACE";
  default:
    return std::to_string(cap);
  }
}

std::string modeName(const GLenum mode)
{
  switch (mode)
  {
  case GL_TRIANGLES:
    return "GL_TRIANGLES";
  case GL_LINES:
    return "GL_LINES";
  case GL_FRONT:
    return "GL_FRONT";
  case GL_BACK:
    return "GL_BACK";
  case GL_CW:
    return "GL_CW";
  default:
    return std::to_string(mode);
  }
}

/**
 * Records the state changes and draws that the light volume renderer issues, and counts
 * the buffers it allocates and uploads.
 */
struct GlRecorder
{
  std::vector<std::string> events;
  std::size_t generatedBuffers = 0;
  std::size_t bufferUploads = 0;

  void install(gl::MockGl& gl)
  {
    gl::installVboSupport(gl);
    gl::installShaderCompileSupport(gl);

    gl.onGenBuffers = [&, generate = gl.onGenBuffers](const GLsizei n, GLuint* buffers) {
      generatedBuffers += std::size_t(n);
      generate(n, buffers);
    };
    gl.onBufferData = [&](GLenum, GLsizeiptr, const GLvoid*, GLenum) { ++bufferUploads; };

    gl.onUseProgram = [](GLuint) {};
    gl.onGetIntegerv = [](const GLenum pname, GLint* params) {
      if (pname == GL_CURRENT_PROGRAM)
      {
        *params = GLint{1};
      }
    };

    gl.onMatrixMode = [](GLenum) {};
    gl.onLoadMatrixd = [](const GLdouble*) {};
    gl.onLoadMatrixf = [](const GLfloat*) {};

    gl.onVertexPointer = [](GLint, GLenum, GLsizei, const GLvoid*) {};
    gl.onColorPointer = [](GLint, GLenum, GLsizei, const GLvoid*) {};
    gl.onEnableClientState = [](GLenum) {};
    gl.onDisableClientState = [](GLenum) {};
    gl.onPolygonMode = [](GLenum, GLenum) {};

    gl.onPushAttrib = [&](const GLbitfield mask) {
      events.push_back("pushAttrib " + std::to_string(mask));
    };
    gl.onPopAttrib = [&]() { events.push_back("popAttrib"); };
    gl.onEnable = [&](const GLenum cap) { events.push_back("enable " + capName(cap)); };
    gl.onDisable = [&](const GLenum cap) { events.push_back("disable " + capName(cap)); };
    gl.onDepthMask = [&](const GLboolean flag) {
      events.push_back(flag ? "depthMask GL_TRUE" : "depthMask GL_FALSE");
    };
    gl.onFrontFace = [&](const GLenum mode) {
      events.push_back("frontFace " + modeName(mode));
    };
    gl.onCullFace = [&](const GLenum mode) {
      events.push_back("cullFace " + modeName(mode));
    };
    gl.onMultiDrawArrays = [&](
                             const GLenum mode, const GLint*, const GLsizei*, GLsizei n) {
      events.push_back("multiDrawArrays " + modeName(mode) + " " + std::to_string(n));
    };
    gl.onDrawArrays = [&](const GLenum mode, GLint, GLsizei) {
      events.push_back("drawArrays " + modeName(mode));
    };
  }
};

const auto pushAttrib =
  "pushAttrib " + std::to_string(GL_POLYGON_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);

mdl::EntityNode* makeLight(const std::string& origin)
{
  return new mdl::EntityNode{mdl::Entity{{
    {"classname", "light"},
    {"origin", origin},
    {"light_radius", "64 64 64"},
  }}};
}

mdl::MapFixtureConfig lightEditorFixtureConfig()
{
  auto config = mdl::MapFixtureConfig{};
  config.gameInfo.gameConfig.entityConfig.rotationPolicy =
    mdl::EntityRotationPolicy::IdTech4;
  config.gameInfo.gameConfig.entityConfig.lightEditor = "idtech4";
  return config;
}

} // namespace

TEST_CASE("LightVolumeRenderer")
{
  auto preferenceManager = PreferenceManagerInstance{};

  auto tmpFile = kdl::tmp_file{};
  {
    auto ofs = std::ofstream{tmpFile.path()};
    ofs << "void main() {}\n";
  }

  auto gl = gl::MockGl{};
  auto recorder = GlRecorder{};
  recorder.install(gl);

  auto vboManager = gl::VboManager{};
  auto shaderManager =
    gl::ShaderManager{[&](const std::filesystem::path&) { return tmpFile.path(); }};
  REQUIRE(shaderManager.loadProgram(gl, gl::Shaders::VaryingPCShader).is_success());
  auto fontManager = gl::FontManager{[](const auto& path) { return path; }};

  auto fixture = mdl::MapFixture{};
  auto& map = fixture.create(lightEditorFixtureConfig());

  auto* selectedLight = makeLight("0 0 0");
  auto* otherLight = makeLight("1000 0 0");
  auto* otherEntity = new mdl::EntityNode{mdl::Entity{{
    {"classname", "info_player_start"},
    {"origin", "0 0 0"},
  }}};
  mdl::addNodes(
    map, {{&mdl::parentForNodes(map), {selectedLight, otherLight, otherEntity}}});
  mdl::selectNodes(map, {selectedLight});

  auto renderer = std::make_unique<LightVolumeRenderer>();

  // the renderer must queue its buffers for destruction before the VboManager frees
  // them, and before the VboManager itself is destroyed
  struct Cleanup
  {
    std::unique_ptr<LightVolumeRenderer>& renderer;
    gl::VboManager& vboManager;
    gl::Gl& gl;

    ~Cleanup()
    {
      renderer.reset();
      vboManager.destroyPendingVbos(gl);
    }
  } cleanup{renderer, vboManager, gl};

  const auto style = LightVolumeStyle{0.45, 4, false, false};

  auto outside = gl::PerspectiveCamera{};
  outside.moveTo(vm::vec3f{-1000, 0, 0});

  auto insideSelected = gl::PerspectiveCamera{};
  insideSelected.moveTo(vm::vec3f{0, 0, 0});

  auto insideOther = gl::PerspectiveCamera{};
  insideOther.moveTo(vm::vec3f{1000, 0, 0});

  const auto renderFrame = [&](
                             const gl::Camera& camera,
                             const LightVolumeVisibility visibility,
                             const RenderMode renderMode = RenderMode::Render3D,
                             const bool hideSelection = false) {
    auto frameStyle = style;
    frameStyle.twoD = renderMode == RenderMode::Render2D;

    renderer->collect(map, visibility, frameStyle);

    auto renderContext =
      RenderContext{gl, renderMode, camera, fontManager, shaderManager};
    if (hideSelection)
    {
      renderContext.setHideSelection();
    }

    recorder.events.clear();
    auto renderBatch = RenderBatch{vboManager};
    renderer->addFills(renderBatch);
    renderer->addLines(renderBatch);
    renderBatch.render(renderContext);
  };

  SECTION("collect")
  {
    SECTION("Selected mode collects the selected lights")
    {
      renderer->collect(map, LightVolumeVisibility::Selected, style);
      REQUIRE(renderer->selectedRanges().size() == 1);
      CHECK(renderer->selectedRanges().front().node == selectedLight);
      CHECK(renderer->selectedRanges().front().selected);
      CHECK(renderer->selectedRanges().front().fillCount == 4 * 224 * 3);
      CHECK(renderer->unselectedRanges().empty());
    }

    SECTION("All mode also collects the outer shell of every visible light")
    {
      renderer->collect(map, LightVolumeVisibility::All, style);
      CHECK(renderer->selectedRanges().size() == 1);
      REQUIRE(renderer->unselectedRanges().size() == 2);
      CHECK(renderer->unselectedRanges()[0].fillCount == 224 * 3);
      CHECK(!renderer->unselectedRanges()[0].selected);

      mdl::hideNodes(map, {otherLight});
      renderer->invalidate();
      renderer->collect(map, LightVolumeVisibility::All, style);
      REQUIRE(renderer->unselectedRanges().size() == 1);
      CHECK(renderer->unselectedRanges().front().node == selectedLight);
    }

    SECTION("Off mode collects nothing")
    {
      renderer->collect(map, LightVolumeVisibility::All, style);
      renderer->collect(map, LightVolumeVisibility::Off, style);
      CHECK(renderer->selectedRanges().empty());
      CHECK(renderer->unselectedRanges().empty());
    }

    SECTION("a game without a light editor collects nothing")
    {
      auto quakeFixture = mdl::MapFixture{};
      auto& quakeMap = quakeFixture.create();

      auto* quakeLight = makeLight("0 0 0");
      mdl::addNodes(quakeMap, {{&mdl::parentForNodes(quakeMap), {quakeLight}}});
      mdl::selectNodes(quakeMap, {quakeLight});

      renderer->collect(quakeMap, LightVolumeVisibility::All, style);
      CHECK(renderer->selectedRanges().empty());
      CHECK(renderer->unselectedRanges().empty());

      // the map dies first
      renderer->invalidate();
    }
  }

  SECTION("nodesDidChange")
  {
    renderFrame(outside, LightVolumeVisibility::All);
    recorder.generatedBuffers = 0;

    SECTION("a changed selected light rebuilds the selected pair only")
    {
      renderer->nodesDidChange({selectedLight});
      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(recorder.generatedBuffers == 2);
    }

    SECTION("a changed unselected light rebuilds the unselected pair only")
    {
      renderer->nodesDidChange({otherLight});
      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(recorder.generatedBuffers == 2);
    }

    SECTION("a changed entity that is no light rebuilds nothing")
    {
      renderer->nodesDidChange({otherEntity});
      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(recorder.generatedBuffers == 0);
    }

    SECTION("a selection change after a selected light changed rebuilds both pairs")
    {
      renderer->nodesDidChange({selectedLight});
      renderFrame(outside, LightVolumeVisibility::All);
      recorder.generatedBuffers = 0;

      mdl::deselectAll(map);
      renderer->invalidateSelection();
      renderFrame(outside, LightVolumeVisibility::All);

      // the empty selected pair allocates no buffer
      CHECK(recorder.generatedBuffers == 2);
      CHECK(renderer->selectedRanges().empty());
    }
  }

  SECTION("addFills and addLines")
  {
    SECTION("bracket their draws with pushAttrib and popAttrib")
    {
      renderFrame(outside, LightVolumeVisibility::Selected);
      CHECK(
        recorder.events
        == std::vector<std::string>{
          pushAttrib,
          "enable GL_DEPTH_TEST",
          "depthMask GL_FALSE",
          "frontFace GL_CW",
          "enable GL_CULL_FACE",
          "cullFace GL_BACK",
          "multiDrawArrays GL_TRIANGLES 1",
          "popAttrib",
          pushAttrib,
          "enable GL_DEPTH_TEST",
          "depthMask GL_FALSE",
          "drawArrays GL_LINES",
          "popAttrib",
        });
    }

    SECTION("cull the front faces of a selected light around the camera")
    {
      renderFrame(insideSelected, LightVolumeVisibility::Selected);
      CHECK(
        std::ranges::find(recorder.events, "cullFace GL_FRONT") != recorder.events.end());
      CHECK(
        std::ranges::find(recorder.events, "cullFace GL_BACK") == recorder.events.end());
    }

    SECTION("draw no fill of an unselected light around the camera")
    {
      // the selected light is drawn from its own pair, the other one is around the
      // camera: one fill draw with one range, and the lines of both
      renderFrame(insideOther, LightVolumeVisibility::All);
      CHECK(
        std::ranges::count_if(
          recorder.events,
          [](const auto& event) { return event.starts_with("multiDrawArrays GL_TRI"); })
        == 1);
      CHECK(
        std::ranges::find(recorder.events, "multiDrawArrays GL_TRIANGLES 1")
        != recorder.events.end());
      CHECK(
        std::ranges::find(recorder.events, "multiDrawArrays GL_LINES 1")
        != recorder.events.end());
    }

    SECTION("draw back to front in runs of equal array and cull face")
    {
      // from far away, the other light is farther than the selected one
      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(
        std::ranges::count_if(
          recorder.events,
          [](const auto& event) { return event.starts_with("multiDrawArrays GL_TRI"); })
        == 2);
    }

    SECTION("disable the depth test for the fills in 2D")
    {
      auto camera2D = gl::OrthographicCamera{};
      camera2D.moveTo(vm::vec3f{0, 0, 0});
      renderFrame(camera2D, LightVolumeVisibility::Selected, RenderMode::Render2D);

      REQUIRE(recorder.events.size() > 2);
      CHECK(recorder.events[0] == pushAttrib);
      CHECK(recorder.events[1] == "disable GL_DEPTH_TEST");
      // the 2D camera is always outside
      CHECK(
        std::ranges::find(recorder.events, "cullFace GL_BACK") != recorder.events.end());
    }

    SECTION("skip the selected lights while the selection is hidden")
    {
      renderFrame(outside, LightVolumeVisibility::Selected, RenderMode::Render3D, true);
      CHECK(recorder.events.empty());
    }

    SECTION("allocate no buffer for an unchanged second frame")
    {
      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(recorder.generatedBuffers == 4);
      CHECK(recorder.bufferUploads == 4);
      const auto vboCount = vboManager.currentVboCount();

      renderFrame(outside, LightVolumeVisibility::All);
      CHECK(recorder.generatedBuffers == 4);
      CHECK(recorder.bufferUploads == 4);
      CHECK(vboManager.currentVboCount() == vboCount);
      CHECK(!recorder.events.empty());
    }
  }
}

} // namespace tb::render
