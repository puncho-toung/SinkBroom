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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/RenderBackend.cpp,
// renderer/NVRHI/RenderBackend_NVRHI.cpp and renderer/NVRHI/RenderProgs_NVRHI.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023 Stephen Saunders
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "BindingSetCache.h"
#include "FrameResources.h"
#include "GeometryArenas.h"
#include "ImageSlots.h"
#include "LitFrontend.h"
#include "LitStages.h"
#include "PipelineCompiler.h"
#include "RenderParms.h"
#include "SamplerSet.h"
#include "base/Macros.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace tb::lit
{

// what one frame's lit passes read and record into (render thread)
struct LitFrameInputs
{
  nvrhi::IDevice* device = nullptr;
  nvrhi::ICommandList* commandList = nullptr;
  std::uint64_t frameIndex = 0;
  FrameResources* frame = nullptr;
  GeometryArenas* arenas = nullptr;
  RenderPipelines* pipelines = nullptr;
  ImageSlots* images = nullptr;
  BindingSetCache* bindingSets = nullptr;
  SamplerSet* samplers = nullptr;
  CameraState camera;
  // the arenas' chunks (the scene's or a test's) and their version (RenderThread's
  // m_chunksVersion)
  const std::map<ChunkKey, Chunk>* chunks = nullptr;
  std::uint64_t chunksVersion = 0;
  const std::unordered_map<PreviewId, ChunkEntityState>* entities = nullptr;
  std::shared_ptr<const LightTable> lights;
  std::shared_ptr<const MaterialTable> materials;
  MaterialService* service = nullptr;
  // a pass boundary: a stop or a park was asked for
  std::function<bool()> stopRequested;
  // a warning for the session's log (posted once per session)
  std::function<void(std::string)> warn;
};

// C16's columns of the lit passes, per frame (LP2.2b logs them)
struct LitFrameStats
{
  // the camera the frame was drawn with
  CameraState camera;
  // the lists were built again this frame (they are kept otherwise)
  bool listsRebuilt = false;
  // lights with interactions in the lists, drawn, and overflowed (D76)
  std::size_t lightsVisible = 0;
  std::size_t lightsDrawn = 0;
  std::size_t lightsOverflowed = 0;
  std::size_t prepassDraws = 0;
  std::size_t interactionDraws = 0;
  // draws skipped: a pending pipeline, a texture with no written level, a chunk not
  // placed yet (D77)
  std::size_t skippedDraws = 0;
  std::size_t pendingPipelines = 0;
  // D76: volatile constant buffer writes counted while recording, their bytes, the
  // writes a frame may make, and the push-constant bytes
  std::uint64_t constantWrites = 0;
  std::uint64_t constantBytes = 0;
  std::uint64_t constantWriteBudget = 0;
  std::uint64_t pushConstantBytes = 0;
  // D77: the frame is not settled (a pending pipeline, image, chunk, material or
  // evaluation)
  bool unconverged = false;
  // D77: a draw was skipped for a pending pipeline, a texture with no written level or a
  // chunk not placed yet: the next frame is drawn at once (an unconverged pass)
  bool retrigger = false;
  // D77, D52: a material, an evaluation or a light's shader is pending: the next frame
  // is drawn when the material table has a new version (a table published), not at once,
  // so that a parse thread that is busy (or held) costs no frames
  bool waitsForTable = false;
  // D9: a pass disabled by a failed pipeline, and why
  bool prepassDisabled = false;
  bool interactionsDisabled = false;
  std::string disabledReason;
  // a stop or park came between the passes or the lights
  bool stoppedEarly = false;
};

// a recorded draw (setDrawLogForTesting)
struct LitDrawRecord
{
  enum class Pass
  {
    Prepass,
    Interaction,
  };

  Pass pass = Pass::Prepass;
  PreviewId light = NoPreviewId;
  InteractionList list = InteractionList::Global;
  ChunkKey chunk;
  int program = 0;
  std::uint64_t state = 0;
  nvrhi::Rect scissor;
};

/**
 * LP2.2a (§3.8 passes 1 and 7, C14, C27, D74-D77): the frame's lit passes into hdrFBO,
 * as DrawViewInternal draws them up to the interactions (SC201): ZeroUniforms, the view's
 * parms, the clears (r_clear 2: HDR and LDR to (0, 0, 0, 1); depth 1, stencil 128), the
 * depth prepass (FillDepthBufferFast with the generic path for the leading subview and
 * the perforated surfaces) and the texture lights' interactions (DrawInteractionsPass,
 * RenderInteractions, DrawSingleInteraction), every draw through
 * DrawElementsWithCounters' order: the binding sets of the layout, the pipeline of the
 * key, the constant buffer committed (written only when the layout's uniforms changed)
 * before setGraphicsState, which runs only when something changed, push constants after
 * it, then drawIndexed.
 *
 * The frontend lists (LitFrontend.h) are built again only when the camera, the extent,
 * the chunks or the light table changed, or when the last build was waiting for a
 * material or an evaluation and the material table has a new version (D52, D77).
 *
 * D76: DRAW_INTERACTION's renderparms are a volatile constant buffer (push constants
 * where the device's limit allows, the layout says which); its writes are counted while
 * recording and a frame writes at most maxVersions / (FramesInFlight + 1), since the
 * versions of the frames before it are recycled only once NVRHI knows they finished: a
 * light whose worst case would pass that is not drawn, nor any light after it, and they
 * count as overflowed.
 *
 * Threads: the render thread; releaseAll in teardown step 2.
 */
class LitPasses
{
private:
  // what the lists were built from
  struct ListsKey
  {
    CameraState camera;
    int width = 0;
    int height = 0;
    const void* chunks = nullptr;
    std::uint64_t chunksVersion = 0;
    const void* entities = nullptr;
    const LightTable* lights = nullptr;
    std::uint64_t lightsVersion = 0;
    std::uint64_t registry = 0;

    bool operator==(const ListsKey&) const = default;
  };

  // DrawElementsWithCounters' state between draws
  struct DrawState
  {
    nvrhi::IGraphicsPipeline* pipeline = nullptr;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts> sets = {};
    std::size_t setCount = 0;
    nvrhi::IBuffer* vertexBuffer = nullptr;
    nvrhi::IBuffer* indexBuffer = nullptr;
    nvrhi::Viewport viewport;
    nvrhi::Rect scissor;
    // the layout of the last draw; Num for none this frame
    EngineBindingLayout layout = EngineBindingLayout::Num;
    bool valid = false;
  };

  enum class DrawOutcome
  {
    Drawn,
    // skipped, the frame is unconverged
    Skipped,
    // the pipeline failed: the pass is disabled (D9)
    Disabled,
    // the volatile constant buffer's frame budget is spent (D76)
    Overflowed,
  };

  // what the frame's draws use, set per frame
  struct Frame
  {
    const LitFrameInputs* inputs = nullptr;
    LitView view;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    FramebufferFormats formats;
  };

  RenderParms m_parms;
  RegisterCache m_registers;
  FrontendLists m_lists;
  std::optional<ListsKey> m_listsKey;
  std::uint64_t m_listsTableVersion = 0;
  DrawState m_state;
  // GL_SelectTexture's units: 0-4 of the interactions, 0 of texture_color
  std::array<std::optional<MaterialImageKey>, 5> m_units;
  // the backend's currentSpace, and currentScissor (RenderBackend.cpp:672-690)
  std::optional<std::size_t> m_currentSpace;
  ScreenRect m_currentScissor;
  nvrhi::Rect m_scissor;
  // RenderProgs.cpp:752-774: a volatile constant buffer per layout that pushes nothing
  std::map<EngineBindingLayout, nvrhi::BufferHandle> m_constantBuffers;
  std::map<EngineBindingLayout, std::uint64_t> m_frameWrites;
  std::uint32_t m_constantBufferVersions = EngineFrame.constantBufferVersions;
  LitFrameStats m_stats;
  bool m_overflowWarned = false;
  bool m_logDraws = false;
  std::vector<LitDrawRecord> m_drawLog;
  // the pipeline keys asked for while the log is on, drawn or not
  std::set<PipelineKey, PipelineKeyLess> m_requestedKeys;
  // the light and list a draw belongs to, for the log
  PreviewId m_logLight = NoPreviewId;
  InteractionList m_logList = InteractionList::Global;

public:
  LitPasses() = default;

  // render thread: the frame's lit passes; what the frame did
  LitFrameStats record(const LitFrameInputs& inputs);

  // the lists of the last build (tests, on the render thread)
  const FrontendLists& lists() const;
  const LitFrameStats& lastStats() const;
  const RenderParms& parms() const;

  // teardown step 2: every handle
  void releaseAll();

  // Test hooks (render thread).

  // the volatile constant buffers' maxVersions (D76); made again at the next draw
  void setConstantBufferVersionsForTesting(std::uint32_t versions);
  // every draw of the following frames is recorded (on) and the log cleared
  void setDrawLogForTesting(bool on);
  const std::vector<LitDrawRecord>& drawLogForTesting() const;
  // the pipeline keys the draws asked for while the log was on, also those of skipped
  // draws (a pending pipeline)
  const std::set<PipelineKey, PipelineKeyLess>& requestedKeysForTesting() const;

private:
  void rebuildLists(const LitFrameInputs& inputs, const LitView& view);
  void recordClears(Frame& frame);
  void recordPrepass(Frame& frame);
  // FillDepthBufferGeneric over surfaces
  bool genericDepth(Frame& frame, const std::vector<const LitSurface*>& surfaces);
  void recordInteractions(Frame& frame);
  // RenderInteractions for one list; false when the pass is disabled
  bool renderInteractions(
    Frame& frame,
    const ViewLight& light,
    const std::vector<LitSurface>& surfaces,
    InteractionList list,
    std::uint64_t depthFunc);
  // D76: the most writes a light's interactions may make
  std::uint64_t worstCaseWrites(const ViewLight& light) const;
  std::uint64_t writeBudget() const;

  void setSpace(Frame& frame, std::size_t space);
  void setInteractionSpace(
    Frame& frame,
    std::size_t space,
    const ViewLight& light,
    const MaterialStage& lightStage,
    const std::optional<EngineGlMatrix>& lightTextureMatrix);

  // DrawElementsWithCounters
  DrawOutcome draw(
    Frame& frame,
    int program,
    const Chunk& chunk,
    std::uint64_t stateBits,
    float slopeBias,
    float depthBias);
  // the binding sets of a layout for the units; false when a texture has no level yet
  bool bindingSets(
    Frame& frame,
    EngineBindingLayout layout,
    const std::vector<nvrhi::BindingLayoutHandle>& layouts,
    bool pushConstants,
    std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts>& sets,
    std::size_t& count);
  nvrhi::IBuffer* constantBuffer(Frame& frame, EngineBindingLayout layout);
  // the textureUsage_t a slot's image has once loaded (the PBR rename)
  int imageUsage(ImageSlots& images, const MaterialImageKey& key) const;
  void log(
    const Frame& frame,
    LitDrawRecord::Pass pass,
    const Chunk& chunk,
    int program,
    std::uint64_t state);

  deleteCopyAndMove(LitPasses);
};

} // namespace tb::lit
