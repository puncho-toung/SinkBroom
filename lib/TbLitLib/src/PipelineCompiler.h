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

#include "StackThread.h"
#include "base/Macros.h"
#include "base/Result.h"
#include "lit/LitSession.h"
#include "lit/ShaderBlobs.h"
#include "lit/ShaderReflection.h"

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace tb::lit
{
class GpuDevice;
class SessionEvents;

// the formats of a pipeline's framebuffer: NVRHI's FramebufferInfo compares formats and
// sample counts only, and the preview draws with one sample (R-A9)
struct FramebufferFormats
{
  nvrhi::Format color = nvrhi::Format::UNKNOWN;
  // UNKNOWN for none
  nvrhi::Format depth = nvrhi::Format::UNKNOWN;
  // T0.10: 1 everywhere (R-A9); a colour list joins it before MSAA or a G-buffer
  std::uint32_t sampleCount = 1;

  bool operator==(const FramebufferFormats&) const = default;
};

// how the engine sets up a pipeline of a program
enum class PipelineRecipe
{
  // PipelineCache::GetOrCreatePipeline (PipelineCache.cpp:58-96): TriangleList, scissor
  // on, depth test and write on, blend on, then GetRenderState with the key's bits
  Draw,
  // TonemapPass::Init (TonemapPass.cpp:229-239): TriangleStrip, no culling, no depth test
  // and no stencil
  Tonemap,
  // CommonRenderPasses::BlitTexture (CommonPasses.cpp:259-269): rect.vs with blit.ps,
  // TriangleStrip, no culling, no depth test and no stencil, blending off
  Blit,
};

/**
 * D71: the engine's PipelineKey (PipelineCache.h:31-39) — state bits, program, int depth
 * bias, slope bias, framebuffer — with the framebuffer as its formats, and the recipe. A
 * Tonemap or Blit pipeline takes no state bits or biases.
 */
struct PipelineKey
{
  PipelineRecipe recipe = PipelineRecipe::Draw;
  // a BUILTIN_ index (lit/EngineBuiltins.h)
  int program = 0;
  std::uint64_t state = 0;
  int depthBias = 0;
  float slopeBias = 0.0f;
  FramebufferFormats framebuffer;

  bool operator==(const PipelineKey&) const = default;
};

// an order of PipelineKeys, for maps of them
struct PipelineKeyLess
{
  bool operator()(const PipelineKey& lhs, const PipelineKey& rhs) const;
};

// what a request finds: Pending until the compile thread has made it
template <typename Handle>
struct PipelineLookup
{
  enum class State
  {
    Pending,
    Ready,
    // the program is disabled (D9), or NVRHI returned no pipeline without a message; for
    // the session. T0.8 (M-PIPE-DIFF-01): an NVRHI Error raised on the compile thread
    // (the validation wrapper's, or a Release driver failure NVRHI reports) goes through
    // the device's message callback and ends the session instead; in Debug a failing
    // vkCreateGraphicsPipelines asserts inside NVRHI
    Failed,
  };
  State state = State::Pending;
  Handle pipeline;
  // the binding layouts, in set order, that binding sets for the pipeline are made for
  std::vector<nvrhi::BindingLayoutHandle> bindingLayouts;
  // Failed: why, "disabled: ..." for a disabled program
  std::string error;
};
using GraphicsPipelineLookup = PipelineLookup<nvrhi::GraphicsPipelineHandle>;
using ComputePipelineLookup = PipelineLookup<nvrhi::ComputePipelineHandle>;

// LP2.1 (C20): what a request for a binding layout type's layouts finds: Pending until
// the compile thread has made them, then the layouts in set order (the ones its
// pipelines are made with), or Failed with why
struct BindingLayoutLookup
{
  using State = GraphicsPipelineLookup::State;

  State state = State::Pending;
  std::vector<nvrhi::BindingLayoutHandle> layouts;
  std::string error;
};

/**
 * The session's compile thread (D71, §3.1 R4-2.21, C20): a StackThread with 8 MB of stack
 * (P2.27) that creates every shader, binding layout, input layout and pipeline the
 * preview draws with, against 1×1 framebuffers of its own that hold only the formats (one
 * per format set), so it never touches a framebuffer of a live render target. Requests
 * return at once: the first one of a key queues it and finds it Pending, and the thread
 * calls onReady, the render thread's frame trigger (D77), once it is made. There is no
 * pipeline cache file (D42); the driver's own cache warms later starts.
 *
 * A program's shaders are read from the game's blobs with plain file reads on this
 * thread (C1, lit/ShaderBlobs.h; no copied code runs here but the pure
 * lit/EnginePipelineState.h), their permutation is the builtins row's defines with
 * USE_PUSH_CONSTANTS by the device's rule (C2, C30), and each is reflected and compared
 * with the tables (D9): a mismatch, a missing blob or permutation disables the program
 * for the session with the field named, posts one warning, and every pipeline of it is
 * Failed. The programs' states are the session's report.
 *
 * Threads: start, join and releaseAll on the GUI thread (phase B, teardown steps 1 and
 * 2); the lookups and the report from any thread; the rest on the compile thread.
 */
class PipelineCompiler
{
private:
  struct Job
  {
    bool compute = false;
    PipelineKey key;
    // LP2.1: a binding layout type's layouts, not a pipeline
    bool layouts = false;
    EngineBindingLayout layout = EngineBindingLayout::Default;
  };

  struct ShaderEntry
  {
    nvrhi::ShaderHandle shader;
    std::optional<std::string> error;
  };

  struct FormatOnlyFramebuffer
  {
    FramebufferFormats formats;
    nvrhi::TextureHandle color;
    nvrhi::TextureHandle depth;
    nvrhi::FramebufferHandle framebuffer;
  };

  // ----- set before the thread starts -----
  GpuDevice& m_device;
  SessionEvents& m_events;
  std::filesystem::path m_gamePath;
  std::function<void()> m_onReady;

  // ----- compile thread while it runs, then the GUI thread -----
  StackThread m_thread;
  std::map<std::tuple<std::string, ShaderStage, std::string>, ShaderEntry> m_shaders;
  std::map<std::pair<EngineBindingLayout, bool>, std::vector<nvrhi::BindingLayoutHandle>>
    m_layouts;
  std::map<int, nvrhi::InputLayoutHandle> m_inputLayouts;
  std::vector<FormatOnlyFramebuffer> m_framebuffers;

  // ----- shared, under m_mutex -----
  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_stopRequested = false;
  std::deque<Job> m_queue;
  std::map<PipelineKey, GraphicsPipelineLookup, PipelineKeyLess> m_graphics;
  std::map<int, ComputePipelineLookup> m_compute;
  std::map<EngineBindingLayout, BindingLayoutLookup> m_layoutLookups;
  std::map<int, ProgramReport> m_programs;
  std::map<int, ExpectedShaderLayout> m_expectedForTesting;
  std::function<void()> m_beforeCreateForTesting;
  std::uint64_t m_created = 0;
  // the graphicsPipeline and computePipeline calls so far (T0.9's test)
  std::uint64_t m_lookups = 0;
  // LP2.4 (Q14): the thread's own timings
  CompileTimings m_timings;

public:
  PipelineCompiler(
    GpuDevice& device, SessionEvents& events, std::filesystem::path gamePath);
  // stops and joins the thread if it still runs
  ~PipelineCompiler();

  // GUI thread, phase B: starts the thread; onReady is called on it after each pipeline
  Result<void> start(std::function<void()> onReady);
  // DX.5, any thread: queues these keys and programs as their first lookups would, so
  // that the thread makes them before a frame asks; not counted as lookups
  void request(const std::vector<PipelineKey>& graphics, const std::vector<int>& compute);
  // any thread: the thread ends before its job's next creation step (T0.7: at most one
  // driver call later); queued keys and the stopped job's stay Pending
  void requestStop();
  // GUI thread, teardown step 1
  void join();
  // GUI thread, teardown step 2, once joined: every handle goes
  void releaseAll();

  // Any thread (the render thread draws with them): the pipeline of a key, made once.
  GraphicsPipelineLookup graphicsPipeline(const PipelineKey& key);
  ComputePipelineLookup computePipeline(int program);
  /**
   * LP2.1 (C20), any thread: the layouts of a binding layout type, in set order, made
   * once by the compile thread from the tables (engineBindingLayoutDescs with the
   * device's push-constant rule, C30), the very handles its pipelines of that type are
   * made with, so that the render thread makes binding sets against them without a
   * pipeline or a blob. Binding layouts are the compile thread's, binding sets the
   * render thread's.
   */
  BindingLayoutLookup layoutsOf(EngineBindingLayout layout);

  // any thread: every program asked for so far and whether it is enabled
  std::vector<ProgramReport> programs() const;

  // Test hooks.

  // the layout a program's shaders are compared with instead of the tables' (a wrong
  // table row, D9); set before the program is first asked for
  void setExpectedLayoutForTesting(int program, ExpectedShaderLayout expected);
  // runs on the compile thread just before each pipeline is created
  void setBeforeCreateForTesting(std::function<void()> hook);
  std::uint64_t pipelinesCreatedForTesting() const;
  // the graphicsPipeline and computePipeline calls so far, from any thread
  std::uint64_t lookupsForTesting() const;
  // LP2.4 (Q14), any thread: the jobs, pipeline creations and shaders timed so far
  CompileTimings timingsForTesting() const;

private:
  void main();
  void run(const Job& job);
  // T0.7, compile thread: an error once a stop has come, checked before each creation
  // step; such an error leaves the job's lookup Pending and disables nothing
  Result<void> unlessStopping() const;
  // compile thread: adds a pipeline creation or a shader made since start to the timings
  void timePipeline(std::chrono::steady_clock::time_point start);
  void timeShader(std::chrono::steady_clock::time_point start);
  // compile thread
  Result<nvrhi::ShaderHandle> shader(
    const EngineBuiltin& builtin,
    ShaderStage stage,
    const ExpectedShaderLayout& expected);
  Result<std::vector<nvrhi::BindingLayoutHandle>> bindingLayouts(
    EngineBindingLayout layout, bool pushConstants, const ExpectedShaderLayout& expected);
  Result<nvrhi::IFramebuffer*> framebuffer(const FramebufferFormats& formats);
  Result<ExpectedShaderLayout> expectedLayout(const EngineBuiltin& builtin);
  Result<GraphicsPipelineLookup> makeGraphics(const PipelineKey& key);
  // a graphics recipe's primitive type and render state
  void recipeState(const PipelineKey& key, nvrhi::GraphicsPipelineDesc& desc) const;
  Result<ComputePipelineLookup> makeCompute(int program);
  Result<BindingLayoutLookup> makeLayouts(EngineBindingLayout layout);
  // a program whose pipeline was made is enabled, unless it was disabled before
  void enable(int program);
  void disable(int program, const std::string& reason);
  bool programDisabled(int program, std::string& reason) const;

  deleteCopyAndMove(PipelineCompiler);
};

/**
 * LP2.1 (T0.9): the render thread's copy of the compile thread's answers, so that a draw
 * asks the compiler nothing once its pipeline is made: a Ready or Failed lookup is kept
 * here and costs one local find from then on; a Pending one is asked of the compiler
 * again on every request (every frame that draws it) until it is Ready or Failed. Without
 * a compiler every lookup is Failed. Render thread only; clear runs in teardown step 2.
 */
class RenderPipelines
{
private:
  PipelineCompiler* m_compiler = nullptr;
  std::map<PipelineKey, GraphicsPipelineLookup, PipelineKeyLess> m_graphics;
  std::map<int, ComputePipelineLookup> m_compute;
  std::map<EngineBindingLayout, BindingLayoutLookup> m_layouts;
  GraphicsPipelineLookup m_pendingGraphics;
  ComputePipelineLookup m_pendingCompute;
  BindingLayoutLookup m_pendingLayouts;
  GraphicsPipelineLookup m_noGraphics;
  ComputePipelineLookup m_noCompute;
  BindingLayoutLookup m_noLayouts;

public:
  RenderPipelines();

  // set before the render thread starts; the compiler outlives the thread (B0.10)
  void setCompiler(PipelineCompiler* compiler);

  // the lookup of a key, valid until the next call or clear
  const GraphicsPipelineLookup& graphics(const PipelineKey& key);
  const ComputePipelineLookup& compute(int program);
  const BindingLayoutLookup& layouts(EngineBindingLayout layout);

  // the Ready and Failed lookups kept
  std::size_t kept() const;
  // teardown step 2: every handle kept
  void clear();

  deleteCopyAndMove(RenderPipelines);
};

} // namespace tb::lit
