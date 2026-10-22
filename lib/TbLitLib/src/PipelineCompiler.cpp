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

// ---- engine notice ----
// Parts of this file transcribe, altered for SinkBroom, code of the Doom 3 BFG Edition
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/PipelineCache.cpp,
// renderer/NVRHI/RenderProgs_NVRHI.cpp, renderer/Passes/TonemapPass.cpp and
// renderer/Passes/CommonPasses.cpp):
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023-2025 Robert Beckebans
// The NVIDIA Donut code the engine adapted there (TonemapPass, CommonPasses) is under
// the MIT licence. That code is also subject to the additional terms that follow the GNU
// General Public License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#include "PipelineCompiler.h"

#include "GpuDevice.h"
#include "SessionEvents.h"
#include "lit/EnginePipelineState.h"

#include "kd/result.h"

#include <fmt/format.h>

#include <exception>
#include <utility>
#include <variant>

namespace tb::lit
{
namespace
{

// P2.27: driver and MoltenVK pipeline compiles are stack-hungry; macOS gives secondary
// threads 512 KB
constexpr auto CompileThreadStack = std::size_t{8} * 1024 * 1024;

std::string programName(const EngineBuiltin& builtin)
{
  return std::string{builtin.name} + builtin.nameOutSuffix;
}

std::string disabledReason(const std::string& reason)
{
  return "disabled: " + reason;
}

template <typename T>
std::string errorText(const Result<T>& result)
{
  return std::get<Error>(result.error()).msg;
}

nvrhi::TextureHandle formatOnlyTexture(
  nvrhi::IDevice& device, const nvrhi::Format format, const bool depth)
{
  return device.createTexture(
    nvrhi::TextureDesc{}
      .setWidth(1)
      .setHeight(1)
      .setFormat(format)
      .setDebugName("Lit preview format-only target")
      .setIsRenderTarget(true)
      .setInitialState(
        depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget)
      .setKeepInitialState(true));
}

} // namespace

bool PipelineCompiler::KeyLess::operator()(
  const PipelineKey& lhs, const PipelineKey& rhs) const
{
  const auto tie = [](const PipelineKey& key) {
    return std::tuple{
      int(key.recipe),
      key.program,
      key.state,
      key.depthBias,
      key.slopeBias,
      int(key.framebuffer.color),
      int(key.framebuffer.depth)};
  };
  return tie(lhs) < tie(rhs);
}

PipelineCompiler::PipelineCompiler(
  GpuDevice& device, SessionEvents& events, std::filesystem::path gamePath)
  : m_device{device}
  , m_events{events}
  , m_gamePath{std::move(gamePath)}
{
}

PipelineCompiler::~PipelineCompiler()
{
  requestStop();
  join();
}

Result<void> PipelineCompiler::start(std::function<void()> onReady)
{
  m_onReady = std::move(onReady);
  return m_thread.start(
    [this] { main(); },
    CompileThreadStack,
    [this](const char* reason) {
      m_events.markFailed(fmt::format("the compile thread failed: {}", reason));
    });
}

void PipelineCompiler::requestStop()
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_stopRequested = true;
  }
  m_wake.notify_all();
}

void PipelineCompiler::join()
{
  m_thread.join();
}

void PipelineCompiler::releaseAll()
{
  // teardown step 2: the pipelines first, then what they were made from
  {
    const auto lock = std::scoped_lock{m_mutex};
    m_graphics.clear();
    m_compute.clear();
    m_queue.clear();
  }
  m_inputLayouts.clear();
  m_layouts.clear();
  m_shaders.clear();
  m_framebuffers.clear();
}

GraphicsPipelineLookup PipelineCompiler::graphicsPipeline(const PipelineKey& key)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (const auto it = m_graphics.find(key); it != m_graphics.end())
    {
      return it->second;
    }
    m_graphics.emplace(key, GraphicsPipelineLookup{});
    m_queue.push_back(Job{false, key});
  }
  m_wake.notify_all();
  return GraphicsPipelineLookup{};
}

ComputePipelineLookup PipelineCompiler::computePipeline(const int program)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (const auto it = m_compute.find(program); it != m_compute.end())
    {
      return it->second;
    }
    m_compute.emplace(program, ComputePipelineLookup{});
    auto key = PipelineKey{};
    key.program = program;
    m_queue.push_back(Job{true, key});
  }
  m_wake.notify_all();
  return ComputePipelineLookup{};
}

std::vector<ProgramReport> PipelineCompiler::programs() const
{
  const auto lock = std::scoped_lock{m_mutex};
  auto reports = std::vector<ProgramReport>{};
  for (const auto& [program, report] : m_programs)
  {
    reports.push_back(report);
  }
  return reports;
}

void PipelineCompiler::setExpectedLayoutForTesting(
  const int program, ExpectedShaderLayout expected)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_expectedForTesting.insert_or_assign(program, std::move(expected));
}

void PipelineCompiler::setBeforeCreateForTesting(std::function<void()> hook)
{
  const auto lock = std::scoped_lock{m_mutex};
  m_beforeCreateForTesting = std::move(hook);
}

std::uint64_t PipelineCompiler::pipelinesCreatedForTesting() const
{
  const auto lock = std::scoped_lock{m_mutex};
  return m_created;
}

void PipelineCompiler::main()
{
  auto lock = std::unique_lock{m_mutex};
  while (true)
  {
    m_wake.wait(lock, [&] { return m_stopRequested || !m_queue.empty(); });
    if (m_stopRequested)
    {
      return;
    }
    const auto job = m_queue.front();
    m_queue.pop_front();
    lock.unlock();
    run(job);
    if (m_onReady)
    {
      // D77: "pipeline ready" is a frame trigger
      m_onReady();
    }
    lock.lock();
  }
}

void PipelineCompiler::run(const Job& job)
{
  const auto failed = [](std::string error) {
    return [error = std::move(error)]<typename L>(L lookup) {
      lookup.state = L::State::Failed;
      lookup.error = error;
      return lookup;
    };
  };

  try
  {
    if (job.compute)
    {
      auto lookup = makeCompute(job.key.program)
                    | kdl::or_else([&](const auto& e) -> Result<ComputePipelineLookup> {
                        return failed(e.msg)(ComputePipelineLookup{});
                      })
                    | kdl::value();
      const auto lock = std::scoped_lock{m_mutex};
      m_compute.insert_or_assign(job.key.program, std::move(lookup));
    }
    else
    {
      auto lookup = makeGraphics(job.key)
                    | kdl::or_else([&](const auto& e) -> Result<GraphicsPipelineLookup> {
                        return failed(e.msg)(GraphicsPipelineLookup{});
                      })
                    | kdl::value();
      const auto lock = std::scoped_lock{m_mutex};
      m_graphics.insert_or_assign(job.key, std::move(lookup));
    }
  }
  catch (const std::exception& e)
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (job.compute)
    {
      m_compute.insert_or_assign(
        job.key.program, failed(e.what())(ComputePipelineLookup{}));
    }
    else
    {
      m_graphics.insert_or_assign(job.key, failed(e.what())(GraphicsPipelineLookup{}));
    }
  }
}

Result<ExpectedShaderLayout> PipelineCompiler::expectedLayout(
  const EngineBuiltin& builtin)
{
  {
    const auto lock = std::scoped_lock{m_mutex};
    if (const auto it = m_expectedForTesting.find(builtin.index);
        it != m_expectedForTesting.end())
    {
      return it->second;
    }
  }
  // C2, C30: the permutation the engine loads on this device
  const auto pushed = m_device.pushConstantsEnabled(builtin.bindingLayout);
  if (auto expected = engineExpectedLayout(builtin, pushed))
  {
    return std::move(*expected);
  }
  return Error{fmt::format(
    "the binding layout {} of {} is not mirrored",
    int(builtin.bindingLayout),
    programName(builtin))};
}

Result<nvrhi::ShaderHandle> PipelineCompiler::shader(
  const EngineBuiltin& builtin,
  const ShaderStage stage,
  const ExpectedShaderLayout& expected)
{
  // C2: the row's defines with USE_PUSH_CONSTANTS by the layout the shaders are checked
  // against (a blit row has none)
  const auto defines = engineBuiltinDefines(builtin, expected.pushConstants);
  auto cacheKey =
    std::tuple{std::string{builtin.name}, stage, shaderPermutationKey(defines)};
  if (const auto it = m_shaders.find(cacheKey); it != m_shaders.end())
  {
    if (it->second.error)
    {
      return Error{*it->second.error};
    }
    return it->second.shader;
  }

  const auto file = shaderBlobPath(builtin.name, stage);
  auto& device = m_device.device();
  auto made =
    readShaderBlob(m_gamePath, builtin.name, stage)
    | kdl::and_then(
      [&](const std::string& blob) { return findShaderPermutation(blob, defines); })
    | kdl::and_then([&](std::vector<std::uint32_t> code) -> Result<nvrhi::ShaderHandle> {
        // D9: reflected and compared before NVRHI sees it
        return reflectSpirv(code)
               | kdl::and_then([&](const ShaderReflection& reflection) {
                   if (const auto difference = compareReflection(reflection, expected))
                   {
                     return Result<nvrhi::ShaderHandle>{Error{*difference}};
                   }
                   const auto desc =
                     engineShaderDesc(builtin.name, builtin.nameOutSuffix, stage);
                   auto handle = device.createShader(desc, code.data(), code.size() * 4);
                   if (!handle)
                   {
                     return Result<nvrhi::ShaderHandle>{
                       Error{"NVRHI cannot create the shader"}};
                   }
                   return Result<nvrhi::ShaderHandle>{std::move(handle)};
                 });
      })
    | kdl::or_else([&](const auto& e) -> Result<nvrhi::ShaderHandle> {
        return Error{fmt::format("{}: {}", file, e.msg)};
      });

  auto entry = ShaderEntry{};
  if (made.is_success())
  {
    entry.shader = made.value();
  }
  else
  {
    entry.error = errorText(made);
  }
  m_shaders.emplace(std::move(cacheKey), entry);
  return made;
}

Result<std::vector<nvrhi::BindingLayoutHandle>> PipelineCompiler::bindingLayouts(
  const EngineBindingLayout layout,
  const bool pushConstants,
  const ExpectedShaderLayout& expected)
{
  const auto cacheKey = std::pair{layout, pushConstants};
  if (const auto it = m_layouts.find(cacheKey); it != m_layouts.end())
  {
    return it->second;
  }
  auto& device = m_device.device();
  auto handles = std::vector<nvrhi::BindingLayoutHandle>{};
  for (const auto& desc : expected.sets)
  {
    auto handle = device.createBindingLayout(desc);
    if (!handle)
    {
      return Error{"NVRHI cannot create a binding layout"};
    }
    handles.push_back(std::move(handle));
  }
  m_layouts.emplace(cacheKey, handles);
  return handles;
}

Result<nvrhi::IFramebuffer*> PipelineCompiler::framebuffer(
  const FramebufferFormats& formats)
{
  for (const auto& framebuffer : m_framebuffers)
  {
    if (framebuffer.formats == formats)
    {
      return framebuffer.framebuffer.Get();
    }
  }

  // R4-2.21: 1×1 targets of the formats only, never a live render target
  auto& device = m_device.device();
  auto made = FormatOnlyFramebuffer{formats, {}, {}, {}};
  auto desc = nvrhi::FramebufferDesc{};
  if (formats.color != nvrhi::Format::UNKNOWN)
  {
    made.color = formatOnlyTexture(device, formats.color, false);
    if (!made.color)
    {
      return Error{"NVRHI cannot create a format-only colour target"};
    }
    desc.addColorAttachment(made.color);
  }
  if (formats.depth != nvrhi::Format::UNKNOWN)
  {
    made.depth = formatOnlyTexture(device, formats.depth, true);
    if (!made.depth)
    {
      return Error{"NVRHI cannot create a format-only depth target"};
    }
    desc.setDepthAttachment(made.depth);
  }
  made.framebuffer = device.createFramebuffer(desc);
  if (!made.framebuffer)
  {
    return Error{"NVRHI cannot create a format-only framebuffer"};
  }
  m_framebuffers.push_back(std::move(made));
  return m_framebuffers.back().framebuffer.Get();
}

Result<GraphicsPipelineLookup> PipelineCompiler::makeGraphics(const PipelineKey& key)
{
  const auto* builtin = engineBuiltin(key.program);
  if (!builtin)
  {
    return Error{fmt::format("{} is no builtin program", key.program)};
  }
  // CommonPasses' blit draws rect.vs with blit.ps (CommonPasses.cpp:59-65)
  const auto* vertexBuiltin =
    key.recipe == PipelineRecipe::Blit ? engineBuiltin(EngineBuiltinRect) : builtin;
  if (
    !(vertexBuiltin->stages & EngineStageVertex)
    || !(builtin->stages & EngineStageFragment))
  {
    return Error{fmt::format("{} is no graphics program", programName(*builtin))};
  }

  auto reason = std::string{};
  if (programDisabled(builtin->index, reason))
  {
    return Error{disabledReason(reason)};
  }

  auto lookup = GraphicsPipelineLookup{};
  auto desc = nvrhi::GraphicsPipelineDesc{};
  const auto made =
    expectedLayout(*builtin) | kdl::and_then([&](const ExpectedShaderLayout& expected) {
      return shader(*vertexBuiltin, ShaderStage::Vertex, expected)
             | kdl::and_then([&](nvrhi::ShaderHandle vs) {
                 desc.VS = std::move(vs);
                 return shader(*builtin, ShaderStage::Pixel, expected);
               })
             | kdl::and_then([&](nvrhi::ShaderHandle ps) {
                 desc.PS = std::move(ps);
                 return bindingLayouts(
                   builtin->bindingLayout, expected.pushConstants, expected);
               })
             | kdl::transform([&](std::vector<nvrhi::BindingLayoutHandle> layouts) {
                 lookup.bindingLayouts = std::move(layouts);
               });
    });
  if (made.is_error())
  {
    // D9: the program is disabled for the session
    reason = errorText(made);
    disable(builtin->index, reason);
    return Error{disabledReason(reason)};
  }

  for (const auto& layout : lookup.bindingLayouts)
  {
    desc.bindingLayouts.push_back(layout);
  }

  // RenderProgs_NVRHI.cpp:250-256: the input layout, made against the vertex shader
  if (
    vertexBuiltin->vertexLayout == EngineVertexLayout::DrawVert
    && key.recipe == PipelineRecipe::Draw)
  {
    auto& inputLayout = m_inputLayouts[vertexBuiltin->index];
    if (!inputLayout)
    {
      const auto attributes = engineDrawVertAttributes();
      inputLayout = m_device.device().createInputLayout(
        attributes.data(), std::uint32_t(attributes.size()), desc.VS);
      if (!inputLayout)
      {
        m_inputLayouts.erase(vertexBuiltin->index);
        return Error{"NVRHI cannot create the input layout"};
      }
    }
    desc.inputLayout = inputLayout;
  }

  switch (key.recipe)
  {
  case PipelineRecipe::Draw:
    // GetOrCreatePipeline's defaults (PipelineCache.cpp:78-88), then the state bits
    desc.primType = nvrhi::PrimitiveType::TriangleList;
    desc.renderState.rasterState.enableScissor();
    desc.renderState.depthStencilState.enableDepthTest().enableDepthWrite();
    for (auto& target : desc.renderState.blendState.targets)
    {
      target.enableBlend();
    }
    engineRenderState(key.state, key.depthBias, key.slopeBias, desc.renderState);
    break;
  case PipelineRecipe::Tonemap:
    // TonemapPass.cpp:229-237
    desc.primType = nvrhi::PrimitiveType::TriangleStrip;
    desc.renderState.rasterState.setCullNone();
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.stencilEnable = false;
    break;
  case PipelineRecipe::Blit:
    // CommonPasses.cpp:259-267, BlitParameters' default blend state (blending off)
    desc.primType = nvrhi::PrimitiveType::TriangleStrip;
    desc.renderState.rasterState.setCullNone();
    desc.renderState.depthStencilState.depthTestEnable = false;
    desc.renderState.depthStencilState.stencilEnable = false;
    desc.renderState.blendState.targets[0] = nvrhi::BlendState::RenderTarget{};
    break;
  }

  return framebuffer(key.framebuffer) | kdl::and_then([&](nvrhi::IFramebuffer* target) {
           auto hook = std::function<void()>{};
           {
             const auto lock = std::scoped_lock{m_mutex};
             hook = m_beforeCreateForTesting;
           }
           if (hook)
           {
             hook();
           }
           lookup.pipeline = m_device.device().createGraphicsPipeline(desc, target);
           if (!lookup.pipeline)
           {
             return Result<GraphicsPipelineLookup>{Error{fmt::format(
               "NVRHI cannot create a pipeline of {}", programName(*builtin))}};
           }
           lookup.state = GraphicsPipelineLookup::State::Ready;
           enable(builtin->index);
           return Result<GraphicsPipelineLookup>{std::move(lookup)};
         });
}

Result<ComputePipelineLookup> PipelineCompiler::makeCompute(const int program)
{
  const auto* builtin = engineBuiltin(program);
  if (!builtin || !(builtin->stages & EngineStageCompute))
  {
    return Error{fmt::format("{} is no compute program", program)};
  }
  auto reason = std::string{};
  if (programDisabled(builtin->index, reason))
  {
    return Error{disabledReason(reason)};
  }

  auto lookup = ComputePipelineLookup{};
  auto desc = nvrhi::ComputePipelineDesc{};
  const auto made =
    expectedLayout(*builtin) | kdl::and_then([&](const ExpectedShaderLayout& expected) {
      return shader(*builtin, ShaderStage::Compute, expected)
             | kdl::and_then([&](nvrhi::ShaderHandle cs) {
                 desc.CS = std::move(cs);
                 return bindingLayouts(
                   builtin->bindingLayout, expected.pushConstants, expected);
               })
             | kdl::transform([&](std::vector<nvrhi::BindingLayoutHandle> layouts) {
                 lookup.bindingLayouts = std::move(layouts);
               });
    });
  if (made.is_error())
  {
    reason = errorText(made);
    disable(builtin->index, reason);
    return Error{disabledReason(reason)};
  }
  for (const auto& layout : lookup.bindingLayouts)
  {
    desc.bindingLayouts.push_back(layout);
  }
  // TonemapPass.cpp:144-147, 196-199
  lookup.pipeline = m_device.device().createComputePipeline(desc);
  if (!lookup.pipeline)
  {
    return Error{
      fmt::format("NVRHI cannot create a pipeline of {}", programName(*builtin))};
  }
  lookup.state = ComputePipelineLookup::State::Ready;
  enable(builtin->index);
  return lookup;
}

void PipelineCompiler::enable(const int program)
{
  const auto* builtin = engineBuiltin(program);
  const auto lock = std::scoped_lock{m_mutex};
  ++m_created;
  m_programs.try_emplace(
    program, ProgramReport{program, builtin ? programName(*builtin) : "", true, {}});
}

void PipelineCompiler::disable(const int program, const std::string& reason)
{
  const auto* builtin = engineBuiltin(program);
  const auto name = builtin ? programName(*builtin) : std::to_string(program);
  {
    const auto lock = std::scoped_lock{m_mutex};
    auto& report = m_programs[program];
    if (!report.enabled && !report.reason.empty())
    {
      return;
    }
    report = ProgramReport{program, name, false, reason};
  }
  // D9: once per program, for the log; the panel's status line shows the report (LP2.3)
  m_events.post(
    SessionEvent::Warning,
    fmt::format("The lit preview cannot draw with {}: {}", name, reason));
}

bool PipelineCompiler::programDisabled(const int program, std::string& reason) const
{
  const auto lock = std::scoped_lock{m_mutex};
  const auto it = m_programs.find(program);
  if (it != m_programs.end() && !it->second.enabled)
  {
    reason = it->second.reason;
    return true;
  }
  return false;
}

} // namespace tb::lit
