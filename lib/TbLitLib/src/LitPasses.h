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
#include "ViewProbes.h"
#include "base/Macros.h"
#include "lit/EngineTables.h"
#include "lit/GridTable.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
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
  // LP4.1 (D79, D94): the scene's .proc tree and its AreaData (SceneGeometry::procTree,
  // areaData), which the frontend's portal flow runs on; null: no flow, LP2's rules
  std::shared_ptr<const ProcTree> procTree;
  std::shared_ptr<const AreaData> areas;
  // LP4.2a (C37, D87): the scene's probe records (SceneGeometry::probeTable), which the
  // view's probe choice reads every frame; null: none
  std::shared_ptr<const ProbeTable> probes;
  // LP4.3 (C36, D79): the bake's light grid (SceneGeometry::gridTable), with the tree;
  // null or empty: every surface on the probe path
  std::shared_ptr<const GridTable> grid;
  std::shared_ptr<const MaterialTable> materials;
  MaterialService* service = nullptr;
  // DX.2: the session's CPU pool, which makes the light triangles (none: at once)
  CpuPool* pool = nullptr;
  // a pass boundary: a stop or a park was asked for
  std::function<bool()> stopRequested;
  // LP4.2a test hook: run after the depth prepass, before the ambient pass's boundary
  // (DebugHook::AfterPrepass)
  std::function<void()> afterPrepass;
  // a warning for the session's log (posted once per session)
  std::function<void(std::string)> warn;
  // C16 (LP2.2b): the queries the prepass and the interactions are timed with, or null
  nvrhi::ITimerQuery* prepassTimer = nullptr;
  nvrhi::ITimerQuery* interactionTimer = nullptr;
  // LP4.2a (C34, C16): the ambient pass's, or null
  nvrhi::ITimerQuery* ambientTimer = nullptr;
  // LP5.2a (C46, C16): the stage pass's, or null
  nvrhi::ITimerQuery* stageTimer = nullptr;
  // LP5.2a (C47, D9): the game folder whose renderprogs2/spirv holds a newStage
  // program's blobs (SessionOptions::gamePath)
  std::filesystem::path gamePath;
};

// C16's columns of the lit passes, per frame (LP2.2b logs them)
struct LitFrameStats
{
  // the camera the frame was drawn with
  CameraState camera;
  // the lists were built again this frame (they are kept otherwise); LPF.1 (C43): for a
  // new registry or a re-parse (ListsTableKey moved)
  bool listsRebuilt = false;
  bool listsTableChanged = false;
  // lights with interactions in the lists, drawn, and overflowed (D76)
  std::size_t lightsVisible = 0;
  std::size_t lightsDrawn = 0;
  std::size_t lightsOverflowed = 0;
  // LP5.1 (C45): pc.c_textureLights and pc.c_mathLights, the lights with interactions
  // that entered the texture and the math phase (RenderBackend.cpp:4022-4029, overflowed
  // ones too), and the preview's own count of the ambientLight lights among them (no
  // engine counter, P6.13)
  std::size_t textureLights = 0;
  std::size_t mathLights = 0;
  std::size_t ambientLights = 0;
  // U2.11: draws dropped for the volatile constant buffer's budget (D76), the prepass's
  // (a perforated surface on a device whose TEXTURE layout is a volatile buffer) and any
  // interaction's
  std::size_t overflowedDraws = 0;
  std::size_t prepassDraws = 0;
  std::size_t interactionDraws = 0;
  // DX.2: interaction draws of light triangles in runs, their draw calls, the triangles
  // the static interactions culled, and whether some are not known yet (unconverged)
  std::size_t lightTriangleDraws = 0;
  std::size_t lightTriangleRuns = 0;
  std::size_t culledTriangles = 0;
  bool lightTrianglesPending = false;
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
  // D77: a draw was skipped for a texture with no written level or a chunk not placed
  // yet: the next frame is drawn at once (an unconverged pass). LP2.2b: a pending
  // pipeline does not re-trigger; the compile thread's onReady triggers the next frame
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
  // C16 (LP2.2b): the passes' timer queries were begun and ended
  bool prepassTimed = false;
  bool interactionsTimed = false;
  // LP4.3 (C36): the drawn surfaces with a grid area (FrontendLists::gridSurfaces)
  std::size_t gridSurfaces = 0;
  // LP4.1 (C16, C32, D90): the view's portal flow behind the lists the frame drew
  // (FrontendLists): the view's area (-1 outside the world and without a tree), the areas
  // flowed, the tree's area count (0 without a tree), the view lights that went on to the
  // pairing, the chains (AddAreaToView calls), the brush-entity chunks the flow left out,
  // and flowView's time when the lists were built this frame (0 when they were kept)
  int viewArea = -1;
  std::size_t areasFlowed = 0;
  std::size_t areas = 0;
  std::size_t lightsInView = 0;
  std::size_t portalChains = 0;
  std::size_t entitiesCulled = 0;
  double flowMs = 0.0;
  // LP4.2a (C37, D88): the view's probe choice: the connected probes, and whether an
  // image it read was not loaded yet (the default stood; the frame is unconverged)
  std::size_t probeCandidates = 0;
  bool probeImagesPending = false;
  // LP4.3 (C36, D88): the atlas of an area with grid points is Pending (not loaded yet:
  // its surfaces take the probe path meanwhile; the frame is unconverged)
  bool gridImagesPending = false;
  // LP4.2a (C34, D86): the ambient pass: its draws, those dropped for set 9's volatile
  // buffer budget (also in overflowedDraws), those skipped (also in skippedDraws) and of
  // them for a pending pipeline (also in pendingPipelines), the surfaces that took the
  // light-grid path (LP4.3, C36), whether its timer ran, and whether a failed pipeline
  // disabled it (D9, disabledReason says why)
  std::size_t ambientDraws = 0;
  std::size_t ambientOverflowed = 0;
  std::size_t ambientSkipped = 0;
  std::size_t ambientPendingPipelines = 0;
  std::size_t ambientGridSurfaces = 0;
  // LP4.3 (D86): the pass's writes of set 9's volatile constant buffer this frame (its
  // own buffer, against constantWriteBudget)
  std::uint64_t ambientConstantWrites = 0;
  bool ambientTimed = false;
  bool ambientDisabled = false;
  // C38 (LP4.2b's ambient-only rows): the interactions were not recorded
  // (skipInteractionsForTesting, r_skipInteractions 1)
  bool interactionsSkipped = false;
  // ----- LP5.2a (C46, C47, D124): the stage pass (DrawShaderPasses' first call) -----
  // its draws; the SL_AMBIENT stages of the surfaces it reached that drew nothing, every
  // reason summed (a condition of 0, ZERO / ONE, a black add, a transparent blend,
  // r_skipNewAmbient, a cinematic, a cube or sky texgen, a missing program, a stage
  // without an image, an image not written yet, a pending pipeline, the budget)
  std::size_t stageDraws = 0;
  std::size_t stageSkipped = 0;
  // the surfaces skipped because their material or registers are pending (D124: the
  // frame is unconverged); the surfaces with an SL_AMBIENT stage left at the stop at
  // SS_POST_PROCESS (K38)
  std::size_t stagePending = 0;
  std::size_t stagePostProcess = 0;
  // of stageSkipped: the stages of a cube or sky texgen (LP5.2b's), of a cinematic
  // (D120), of a newStage program absent from the table or its blobs (D9)
  std::size_t stageTexgenDeferred = 0;
  std::size_t stageCinematic = 0;
  std::size_t stageProgramMissing = 0;
  // the pass was not recorded (skipShaderPassesForTesting, r_skipAmbient 1's stand-in,
  // D118, or r_skipShaderPasses 1), its timer ran, a failed pipeline disabled it (D9)
  bool stagesSkipped = false;
  bool stagesTimed = false;
  bool stagesDisabled = false;
};

// a recorded draw (setDrawLogForTesting)
struct LitDrawRecord
{
  enum class Pass
  {
    Prepass,
    Interaction,
    // LP4.2a: the ambient pass
    Ambient,
    // LP5.2a: the stage pass
    Stage,
  };

  Pass pass = Pass::Prepass;
  PreviewId light = NoPreviewId;
  InteractionList list = InteractionList::Global;
  ChunkKey chunk;
  int program = 0;
  std::uint64_t state = 0;
  nvrhi::Rect scissor;
};

// DX.5: the state bits of an interaction draw with this depth function (EQUAL, LESS for
// the translucent list): additive RGB, alpha overwritten, no depth write, stencil
// ALWAYS (D75)
std::uint64_t interactionStateBits(std::uint64_t depthFunc);

/**
 * DX.5: the lit passes' pipelines that every lit scene draws with, as their first draws
 * ask for them against the HDR target and this depth format: the depth prepass
 * (builtin/depth, GLS_DEFAULT, and GLS_ALPHAMASK for a surface whose registers are
 * pending), a perforated surface's (texture_color, state 0) and the interaction programs'
 * four (interaction and PBR interaction, EQUAL and LESS). Pure.
 */
std::vector<PipelineKey> litStartPipelineKeys(nvrhi::Format depthFormat);

/**
 * LP4.2a (C34, SC322): the ambient pass's GL state before a draw (AmbientPass,
 * RenderBackend.cpp:2373 and 2600-2700): ONE / ZERO (RGBA overwritten) for a group whose
 * vertexColor is SVC_IGNORE, ONE / ONE (RGB added; alpha stays One / Zero through the
 * copied GetRenderState, SC210) otherwise; no depth write, depth EQUAL, stencil ALWAYS.
 * Pure.
 */
std::uint64_t ambientStateBits(int vertexColor);

/**
 * LP4.2a (C34, C35): where AMBIENT_LIGHTING_IBL's texture units go
 * (RenderBackend_NVRHI.cpp:698-788): set 1 the units 0-2 at t0-t2, set 2 the units 3, 4,
 * 7, 8, 9, 10, 12, 13, 14 at the slots of the same number, in the layout's item order
 * (nvrhi rule 4). Pure.
 */
struct AmbientUnitBinding
{
  std::uint32_t set = 0;
  std::size_t unit = 0;
  std::uint32_t slot = 0;
};
std::span<const AmbientUnitBinding> ambientUnitBindings();

// the texture units the passes select (GL_SelectTexture): 0-4 of the interactions, 0 of
// texture_color, and (LP4.2a) up to 14 of the ambient pass
inline constexpr std::size_t LitTextureUnits = 15;

/**
 * LP2.2a (§3.8 passes 1 and 7, C14, C27, D74-D77): the frame's lit passes into hdrFBO,
 * as DrawViewInternal draws them up to the interactions (SC201): ZeroUniforms, the view's
 * parms, the clears (r_clear 2: HDR and LDR to (0, 0, 0, 1); depth 1, stencil 128), the
 * depth prepass (FillDepthBufferFast with the generic path for the leading subview and
 * the perforated surfaces), the ambient pass (LP4.2a) and the interactions
 * (DrawInteractions: LP5.1, C45, the texture phase then the math phase,
 * DrawInteractionsPass, RenderInteractions, DrawSingleInteraction; ambientLight lights in
 * either phase through interactionAmbient), every draw through DrawElementsWithCounters'
 * order: the binding sets of the layout, the pipeline of the key, the constant buffer
 * committed (written only when the layout's uniforms changed) before setGraphicsState,
 * which runs only when something changed, push constants after it, then drawIndexed.
 *
 * The frontend lists (LitFrontend.h) are built again only when the camera, the extent,
 * the chunks (an entity's area references with them: SceneGeometry builds them when a
 * batch applies, which raises the chunks' version), the light table or the bake's tree
 * and AreaData changed, or when the last build was waiting for a material or an
 * evaluation and the material table has a new version (D52, D77).
 *
 * D76: DRAW_INTERACTION's renderparms are a volatile constant buffer (push constants
 * where the device's limit allows, the layout says which); its writes are counted while
 * recording and a frame writes at most maxVersions / (FramesInFlight + 1), since the
 * versions of the frames before it are recycled only once NVRHI knows they finished: a
 * light whose worst case would pass that is not drawn, nor any light after it in the
 * draw order (LP5.1, P6.13: the rest of its phase and the whole math phase after the
 * texture phase), and they count as overflowed.
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
    // LP4.1 (D79): a new bake brings a new tree and AreaData; an entity's references
    // change only when a batch applies, which raises chunksVersion
    const void* tree = nullptr;
    const void* areas = nullptr;
    // LP4.3 (D79, P4.6): the grid table, and with a grid which of its areas' grids are
    // valid now (a Ready or Missing atlas changes the grid-area dispatch)
    const void* grid = nullptr;
    std::uint64_t gridVersion = 0;
    std::uint64_t gridValidity = 0;
    ListsTableKey table;
    // DX.2: the light triangles' answers, and whether the cull applies (a test's switch)
    std::uint64_t lightTrianglesVersion = 0;
    bool lightTriangleCull = true;

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
  // DX.2: the static interactions' light triangles
  LightTriangleCache m_lightTriangles;
  FrontendLists m_lists;
  // LP4.2a (C37, D79): the view's probes, chosen every frame outside the cached lists
  ViewProbes m_viewProbes;
  std::optional<ListsKey> m_listsKey;
  // LP4.3 (D79): the grid validity the lists' key holds, worked out again only when the
  // slots' state version or the grid table moved (ImageSlots::stateVersion)
  std::uint64_t m_gridValidity = 0;
  std::uint64_t m_gridValiditySlotStates = ~std::uint64_t{0};
  const void* m_gridValidityTable = nullptr;
  std::uint64_t m_listsTableVersion = 0;
  DrawState m_state;
  // GL_SelectTexture's units: 0-4 of the interactions, 0 of texture_color, 0-14 of the
  // ambient pass (LP4.2a)
  std::array<std::optional<MaterialImageKey>, LitTextureUnits> m_units;
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
  bool m_drawOverflowWarned = false;
  bool m_logDraws = false;
  // V2.2's measurement: the static interaction cull applies (the engine's behaviour)
  bool m_lightTriangleCull = true;
  // C38 (LP4.2b): the interactions are not recorded (r_skipInteractions 1)
  bool m_skipInteractions = false;
  // LP5.0 (D118): r_skipAmbient 1's stand-in, which the stage pass reads (LP5.2a)
  bool m_skipShaderPasses = false;
  // LP5.2a (C46, D115): the drawn surfaces in the stage pass's order (stagePassOrder),
  // made when the lists are built; pointers into m_lists.drawSurfs
  std::vector<const LitSurface*> m_stageOrder;
  // LP5.2a (C47, D9): a newStage program's row by its name, or none (not in the table, or
  // a blob missing), looked up once; the names already warned about
  std::map<std::string, std::optional<EngineMaterialProgram>> m_materialPrograms;
  std::set<std::string> m_programWarnings;
  bool m_texgenDeferredWarned = false;
  bool m_stageImageWarned = false;
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
  // LP4.2a: the last frame's probe choice (tests, on the render thread)
  const ViewProbes& viewProbes() const;
  // DX.2, render thread: a finished light triangles job is taken (whether one was), and
  // whether one runs or waits
  bool collectLightTriangles();
  bool lightTrianglesBusy() const;
  const LitFrameStats& lastStats() const;
  const RenderParms& parms() const;
  // U2.6, render thread: the volatile constant buffers' bytes, every version (NVRHI's
  // aligned size times maxVersions)
  std::uint64_t volatileBufferBytes() const;

  // teardown step 2: every handle
  void releaseAll();

  // Test hooks (render thread).

  // the volatile constant buffers' maxVersions (D76); made again at the next draw
  void setConstantBufferVersionsForTesting(std::uint32_t versions);
  // every draw of the following frames is recorded (on) and the log cleared
  void setDrawLogForTesting(bool on);
  // V2.2: off, every surface is drawn whole as if no static interaction cull ran (the
  // fixture measurement's comparison); the lists are built again
  void setLightTriangleCullForTesting(bool on);
  // C38 (LP4.2b's ambient-only rows, mirror.json M-TEST-AMB-01): while on, the
  // interactions pass is not recorded, as r_skipInteractions 1 skips DrawInteractions
  // (RenderBackend.cpp:4181); every other pass as it is
  void setSkipInteractionsForTesting(bool on);
  // LP5.0 (D118, C46): r_skipAmbient 1's stand-in: while on, the stage pass is not
  // recorded (RenderBackend.cpp:4275 returns at once; LP5.2a); every other pass as it is
  void setSkipShaderPassesForTesting(bool on);
  bool skipShaderPassesForTesting() const;
  const std::vector<LitDrawRecord>& drawLogForTesting() const;
  // the pipeline keys the draws asked for while the log was on, also those of skipped
  // draws (a pending pipeline)
  const std::set<PipelineKey, PipelineKeyLess>& requestedKeysForTesting() const;

private:
  void rebuildLists(const LitFrameInputs& inputs, const LitView& view);
  // C16: a pass's timer query, begun and ended around it; endTimer says whether it ran
  void beginTimer(Frame& frame, nvrhi::ITimerQuery* timer);
  bool endTimer(Frame& frame, nvrhi::ITimerQuery* timer);
  void recordClears(Frame& frame);
  void recordPrepass(Frame& frame);
  // FillDepthBufferGeneric over surfaces
  bool genericDepth(Frame& frame, const std::vector<const LitSurface*>& surfaces);
  void recordInteractions(Frame& frame);
  // LP4.2a (C34): AmbientPass( fillGbuffer false ) on the probe path; LP4.3 (C36): and
  // on the light-grid path for a surface with a grid area (RenderBackend.cpp:1196-1305)
  void recordAmbient(Frame& frame);
  // AmbientPass' space change (RenderBackend.cpp:2480-2502); DrawShaderPasses' sets the
  // same four (:4352-4382)
  void setAmbientSpace(Frame& frame, std::size_t space);
  /**
   * LP5.2a (C46, C47, D115-D121, D124): DrawShaderPasses( drawSurfs, numDrawSurfs )'s
   * first call (RenderBackend.cpp:4271-4716 from :6107-6125, r_skipShaderPasses 0) into
   * hdrFBO with the prepass's depth, over m_stageOrder: a surface whose material or
   * registers are pending is skipped (counted, the frame unconverged, D124); one without
   * an SL_AMBIENT stage, a portal sky, an empty one and a suppressInSubview one are
   * passed over; the pass stops at the first surface of sort SS_POST_PROCESS or more
   * (K38, no _currentRender copy); per surface the space's matrices on a space change,
   * its engine scissor (LitSurface::scissor) with the Y flip, and each draw of
   * engineShaderPassDraws: an old-style stage with its colour, vertex colour, texture
   * matrix (and TG_SCREEN's planes) and its image at unit 0 by the slot's state
   * (stageImage), a newStage block on its table program (C47) with USER0 + j and its
   * fragment images at units 0-2 (a null one _default); at the end GL_Color( 1 ) and
   * GLS_DEFAULT (no draw).
   */
  void recordStages(Frame& frame);
  /**
   * LP5.2a (D124, P6.17): the image a stage binds at a unit, by its slot's state:
   * Default (the service has no image: the engine's image not loaded or defaulted) binds
   * _default, as BindVariableStageImage does (RenderBackend.cpp:333-342); Pending (not
   * answered yet) and Loading without a written level bind nothing yet (nullopt: the
   * draw is skipped, the frame unconverged); Loading with written levels, Resident and
   * Replacing (its complete texture, LPF.1) bind the slot.
   */
  std::optional<MaterialImageKey> stageImage(
    ImageSlots& images, const MaterialImageKey& key);
  // C47, D9: a newStage stage's program row, looked up once per name (null: missing)
  const EngineMaterialProgram* materialProgram(Frame& frame, const MaterialStage& stage);
  // RenderInteractions for one list; false when the pass is disabled
  bool renderInteractions(
    Frame& frame,
    const ViewLight& light,
    const std::vector<LitSurface>& surfaces,
    InteractionList list,
    std::uint64_t depthFunc);
  // D76: the most writes a light's interactions may make
  std::uint64_t worstCaseWrites(const ViewLight& light) const;
  // U2.11: a draw dropped for the budget: counted, and said once
  void drawOverflowed(Frame& frame);
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
    float depthBias,
    const LightTriangles* lightTriangles = nullptr);
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
