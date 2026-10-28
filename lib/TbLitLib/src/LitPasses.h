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
#include "DrawRecords.h"
#include "FrameResources.h"
#include "GeometryArenas.h"
#include "ImageSlots.h"
#include "LitFrontend.h"
#include "LitStages.h"
#include "PipelineCompiler.h"
#include "RenderParms.h"
#include "SamplerSet.h"
#include "ShadowCache.h"
#include "ViewProbes.h"
#include "base/Macros.h"
#include "lit/EngineShadowAtlas.h"
#include "lit/EngineShadowMatrices.h"
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
  // LP7.2 (C52, D129): the scene's model instances, synced with the arenas with the
  // chunks (chunksVersion covers them); null: none
  const std::map<ChunkKey, ModelInstance>* instances = nullptr;
  const std::unordered_map<PreviewId, ChunkEntityState>* entities = nullptr;
  std::shared_ptr<const LightTable> lights;
  // LP4.1 (D79, D94): the scene's .proc tree and its AreaData (SceneGeometry::procTree,
  // areaData), which the frontend's portal flow runs on; null: no flow, LP2's rules
  std::shared_ptr<const ProcTree> procTree;
  std::shared_ptr<const AreaData> areas;
  // LP5.3 (C49, D123): the scene's portal -> fog light table (SceneGeometry::
  // portalFogTable), with the tree; null: no portal is fogged
  std::shared_ptr<const PortalFogTable> portalFog;
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
  // LP5.3 (C49, C16): pass 9's (FogAllLights), or null
  nvrhi::ITimerQuery* fogTimer = nullptr;
  // LP3.1 (C59, C16): pass 6's (ShadowAtlasPass), or null
  nvrhi::ITimerQuery* shadowTimer = nullptr;
  // LP3.3 (D146, P3.4a): the device copies D32 images (GpuChoice::depthCopySupported):
  // a tile relocated at settle is copied through the scratch; else it is drawn again
  bool depthCopy = false;
  // LP3.3 (C62, D152): the camera moves (FramePacer::cameraMoving at the frame's start):
  // pass 6 holds the layout and the LODs; else the frame settles them
  bool shadowsMoving = false;
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
  // LP3.0 (C57, P3.14, P3.24f): the caster light-triangle requests of the frame's lists
  // (FrontendLists): the lights that gathered casters, the (light, chunk) pairs asked
  // for, answered and pending, the casters kept, the gathering's time when the lists were
  // built this frame (0 when they were kept), and the light-triangle cache's entries
  std::size_t casterLights = 0;
  std::size_t casterPairs = 0;
  std::size_t casterPairsResident = 0;
  std::size_t casterPairsPending = 0;
  std::size_t casters = 0;
  double casterMs = 0.0;
  std::size_t lightTriangleEntries = 0;
  // LP3.0 (D151, C62, A49), LP3.3: the shadows' convergence term, a placed tile not
  // drawn whole against its light's current record at the exact LOD (shadowLodHeld), a
  // provisional tile, a cascade budget pending (cascadesStale), a caster's light
  // triangles unknown, or (LP3.3, D145) the held layout not the engine's packing
  // (shadowLayoutDrift); and the still timer's scope: a held LOD, a pending cascade
  // budget, or the held layout not the engine's
  bool shadowsPending = false;
  bool shadowSettleWanted = false;
  // LP7.3 (C54): the dress surfaces the frame draws (FrontendLists::dressSurfaces: the
  // frame log's dress_surfaces)
  std::size_t dressSurfaces = 0;
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
  // LP5.3 (C49, D123): the portal sides the flow found fogged out, and the fog portals
  // left open because their fog light's registers were first-seen (FrontendLists)
  std::size_t portalsFogged = 0;
  std::size_t portalFogPending = 0;
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
  // r_skipNewAmbient, a cinematic, TG_REFLECT_CUBE2, a missing program, a stage without
  // an image, a cube image on texture_color, an image not written yet, a pending
  // pipeline, the budget)
  std::size_t stageDraws = 0;
  // LP7.2 (C52, D129): the draws of model instances (a shared placement), every pass's
  // (the frame log's model_draws)
  std::size_t modelDraws = 0;
  std::size_t stageSkipped = 0;
  // the surfaces skipped because their material or registers are pending (D124: the
  // frame is unconverged) and (LP5.2b, C48) the cube-program draws made with the zero
  // cube because their cube image was not written yet (Pending, or Loading without a
  // level: unconverged); the surfaces with an SL_AMBIENT stage left at the stop at
  // SS_POST_PROCESS (K38)
  std::size_t stagePending = 0;
  std::size_t stagePostProcess = 0;
  // of stageSkipped: the TG_REFLECT_CUBE2 stages (LP5.2b, C48: programs 59-62 need LP6's
  // inputs, and without a bump stage texture_color would sample the cube as a Texture2D,
  // mirror.json M-STAGE-DIFF-05), the stages of a cinematic (D120), of a newStage program
  // absent from the table or its blobs (D9)
  std::size_t stageTexgenLp6 = 0;
  std::size_t stageCinematic = 0;
  std::size_t stageProgramMissing = 0;
  // the pass was not recorded (skipShaderPassesForTesting, r_skipAmbient 1's stand-in,
  // D118, or r_skipShaderPasses 1), its timer ran, a failed pipeline disabled it (D9)
  bool stagesSkipped = false;
  bool stagesTimed = false;
  bool stagesDisabled = false;
  // ----- LP5.3 (C49, D122, D124): pass 9, FogAllLights -----
  // the fog lights FogPass drew and the blend lights BlendLight went past its returns
  // for; the fog draws, the surfaces' (T_BasicFog on the lists) and the cubes' apart
  // (fogDraws is their sum: the frame log's fog_draws), and the blend draws
  std::size_t fogLights = 0;
  std::size_t blendLights = 0;
  std::size_t fogDraws = 0;
  std::size_t fogSurfaceDraws = 0;
  std::size_t fogCubeDraws = 0;
  std::size_t blendDraws = 0;
  // the pass's draws not made: a pending pipeline, an image not written yet (a blend
  // light's stage or falloff image Pending, or Loading without a level: the frame
  // unconverged), the budget
  std::size_t fogSkipped = 0;
  // r_skipFogLights 1 (EngineFrame.skipFogLights): FogAllLights returned at once; the
  // timer ran; a failed pipeline of 65 or 67 disabled it (D9)
  bool fogLightsSkipped = false;
  bool fogTimed = false;
  bool fogDisabled = false;
  // ----- LP3.1 (C59, D147, D157): pass 6, ShadowAtlasPass -----
  // the lights that take tiles (loop 1's: a casting light with shadowLod > -1 and a
  // caster, RenderBackend.cpp:3653-3674), those all of whose sides found a tile, and
  // those with a side that found none (unshadowed: imageSize.x -1, SC720), with the
  // latter's names (their "name" key) in viewLights order
  std::size_t lightsCasting = 0;
  std::size_t lightsPlaced = 0;
  std::size_t lightsUnplaced = 0;
  std::vector<std::string> unplacedLights;
  // the tiles cleared and drawn (each its far-plane clear first) and the rects of last
  // frame's tiles this frame's do not cover, cleared (vacated, P3.3); LP3.3 (C61, D146):
  // the tiles left as they were (cached: their light's record key, rect and matrices
  // unchanged), and those copied to their new rect (relocated); LP3.4 (D156, K53): the
  // tiles drawn provisional (a perforated caster's stage image not complete, or its
  // material or registers pending: ShadowTileRecord::provisional, drawn again)
  std::size_t tilesDrawn = 0;
  std::size_t tilesVacated = 0;
  std::size_t tilesRelocated = 0;
  std::size_t tilesCached = 0;
  std::size_t tilesProvisional = 0;
  // the atlas bytes the drawn tiles' far-plane clears and draws cover (tiles × size² × 4)
  std::uint64_t atlasBytesWritten = 0;
  // LP3.3 (C61, D146): the tiles the frame's interactions sample (drawn, cached or
  // relocated) and, over them, the casters' draws and the atlas bytes each was last
  // drawn with: what the frame's shadows are made of, whichever frame drew them (the
  // [.][parity] rows' tiles_drawn, shadow_draws and atlas_bytes_written)
  std::size_t layoutTiles = 0;
  std::size_t layoutShadowDraws = 0;
  std::uint64_t layoutAtlasBytes = 0;
  // LP3.3 (C62, D152): the frame held the layout and the LODs (the camera moves), some
  // light's held LOD differs from its exact one, the parallel cascades left stale by the
  // budget (sampled with the matrices they were drawn with: counted among the cached),
  // and the held layout differs from the engine's packing of the frame's sides
  bool shadowsMoving = false;
  bool shadowLodHeld = false;
  std::size_t cascadesStale = 0;
  bool shadowLayoutDrift = false;
  // the casters' depth draws into the tiles, the (caster, side) pairs the per-side cull
  // left out (D149), and the perforated casters of the lights that take tiles whose
  // stage pass 6 cannot draw (LP3.4, D155: an alpha-tested texture_color stage without an
  // image, a TG_REFLECT_CUBE2 one; each light's once per frame); LP3.4: the perforated
  // casters' draws among shadowDraws (ShadowMapPassPerforated's)
  std::size_t shadowDraws = 0;
  std::size_t shadowCastersCulled = 0;
  std::size_t perforatedCastersSkipped = 0;
  std::size_t perforatedShadowDraws = 0;
  // LP3.4 (D158): the debug view's draws after the tonemap pass (the atlas's quad, the
  // tile-age view's rects); 0 without a view
  std::size_t debugViewDraws = 0;
  // the pass ran (its timer begun and ended), its CPU record time, and whether a failed
  // pipeline or the atlas's creation disabled it (every light then unplaced, D9)
  bool shadowAtlasTimed = false;
  double shadowRecordMs = 0.0;
  bool shadowAtlasDisabled = false;
  // LP3.1 (D76, P3.5, D157): the interactions' writes of their volatile constant buffers
  // this frame, set 10's (DRAW_INTERACTION) and set 11's (DRAW_INTERACTION_SM), each
  // against constantWriteBudget
  std::uint64_t interactionWrites = 0;
  std::uint64_t interactionSmWrites = 0;
  // LP3.2 (D157 extended, SC709, P3.10): the interaction draws that used a shadow-atlas
  // program (interactionSM), per list (countInteractionDraw): H05's proof that a
  // noSelfShadow surface of a casting light is drawn on the local list with it; the plain
  // interaction draws are interactionDraws less their sum
  std::size_t smDrawsLocal = 0;
  std::size_t smDrawsGlobal = 0;
  std::size_t smDrawsTranslucent = 0;
  // ----- LP6.0 (C65, D165): the passes' CPU record times this frame, in ms on FrameClock
  // (the frame log's lists_ms ... fog_cpu_ms) -----
  // rebuildLists whole, kept lists too (the key's test alone): the flow's flowMs and the
  // gathering's casterMs are inside it; and passes 1, 5, 7, 8 (every stage call of the
  // frame, summed) and 9, each from its timer's begin to its end (pass 6's is
  // shadowRecordMs); 0 for a pass not recorded
  double listsMs = 0.0;
  double prepassRecordMs = 0.0;
  double ambientRecordMs = 0.0;
  double interactionRecordMs = 0.0;
  double stageRecordMs = 0.0;
  double fogRecordMs = 0.0;
  // ----- LP6.1 (C64): the draw records of passes 1, 5 and 7 (DrawRecordTable) -----
  // the records the table holds at the frame's end, and the resolutions this frame (a
  // record's first draw, its draw after a release, and each draw of a record whose
  // pipeline is pending, whose chunk is not placed or whose binding failed)
  std::size_t recordsHeld = 0;
  std::size_t recordResolves = 0;
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
    // LP5.3: FogAllLights' fog draws (a surface or the cube) and blend draws
    Fog,
    Blend,
    // LP3.1 (C59, D147): pass 6's far-plane clears (a tile's, a vacated rect's) and its
    // casters' depth draws
    ShadowClear,
    Shadow,
    // LP3.3 (D146): a tile relocated (copied to its new rect through the scratch)
    ShadowMove,
  };

  Pass pass = Pass::Prepass;
  PreviewId light = NoPreviewId;
  InteractionList list = InteractionList::Global;
  ChunkKey chunk;
  int program = 0;
  std::uint64_t state = 0;
  nvrhi::Rect scissor;
  // LP3.1: a shadow draw's side (point 0-5, parallel 0-3, spot -1); -1 otherwise
  int side = -1;
};

/**
 * LP3.1 (C59, C61, SC719-SC720): a casting light's record of the frame's pass 6, the
 * viewLight_t fields ShadowAtlasPass writes (RenderBackend.cpp:3798-3895): imageSize (the
 * light's first side's output size; x -1 once a side found no tile: ImageAtlasPlaced()
 * false, RenderCommon.h:390-393), imageAtlasOffset per side (the tile's top-left pixel;
 * the sides a light does not have stay 0, as the cleared frame allocation leaves them),
 * and each drawn side's shadowV and shadowP as SetupShadowMapMatrices stored them (C61:
 * the consumer reads them from here, never recomputes them).
 */
struct ShadowLightRecord
{
  PreviewId light = NoPreviewId;
  // LP3.3 (C61): the light's record key this frame (its tiles drawn or kept against it);
  // its exact LOD (engineShadowLod's: lod is the held one while the camera moves, D152)
  // and the fade of lod (the consumer's shadowFadeOut)
  ShadowRecordKey key;
  int exactLod = -1;
  float fadeOut = 0.0f;
  // its index in FrontendLists::lights
  std::size_t index = 0;
  ShadowLightType type = ShadowLightType::Point;
  int lod = -1;
  // the sides (point 6, parallel r_shadowMapSplits + 1, spot 1) and each one's placement
  int sides = 0;
  std::array<EngineAtlasPlacement, 6> placements = {};
  std::array<int, 2> imageSize = {-1, -1};
  std::array<std::array<int, 2>, 6> atlasOffsets = {};
  std::array<EngineMat4, 6> shadowV = {};
  std::array<EngineMat4, 6> shadowP = {};
  // LP3.4 (D156, K53): a tile of the light drawn this frame is provisional
  bool provisional = false;

  bool placed() const { return imageSize[0] != -1 && imageSize[1] != -1; }
};

// LP3.3 (C61): what a tile's draws made: the casters drawn, and whether every draw was
// made (none skipped for a pending pipeline or dropped for the budget) and every caster's
// light triangles were known (a Pending caster is drawn whole: the tile is drawn again);
// LP3.4 (D156, K53): and whether a perforated caster was drawn with its stage image not
// complete or its material or registers pending (the tile provisional)
struct ShadowTileDraws
{
  std::size_t draws = 0;
  bool complete = true;
  bool provisional = false;
};

/**
 * LP3.1 (D149, P3.19): whether a caster's local bounds lie outside a shadow map side's
 * true frustum, mvp the side's clip matrix of the caster's space (the matrix its draw
 * takes): the planes extracted from it as the device clips (x and y within ±w, z within
 * 0..w, the far plane), each normalised, with a margin of 0.01 units inside of which
 * nothing is culled. A point side is 92° wide with zNear 4, a parallel side the crop box
 * with the light bounds' z range. Conservative, so picture-neutral. Pure.
 */
bool engineShadowCasterCulled(const EngineMat4& mvp, const EngineBounds& bounds);

// DX.5: the state bits of an interaction draw with this depth function (EQUAL, LESS for
// the translucent list): additive RGB, alpha overwritten, no depth write, stencil
// ALWAYS (D75)
std::uint64_t interactionStateBits(std::uint64_t depthFunc);

/**
 * LP3.1 (C60, SC726): DrawSingleInteraction's program (RenderBackend.cpp:1404-1470 PBR,
 * :1528-1595 legacy): an ambientLight light's interactionAmbient first; then, for a
 * light the frame's pass 6 placed in the atlas (shadow, its record), the shadow-atlas
 * interaction of its type, vLight->parallel, then pointLight, else spot (ids 39-50, never
 * the skinned forms, P3.24e); else the plain interaction (K26). Pure; any thread.
 */
int engineInteractionProgram(InteractionProgram program, const ShadowLightRecord* shadow);

// LP3.2 (D157 extended, SC709): a shadow-atlas interaction program (interactionSM, the
// builtins 39-49 of engineInteractionProgram). Pure
bool engineShadowAtlasProgram(int program);

/**
 * LP3.2 (D157 extended, SC709, P3.10): a drawn interaction counted into the frame's
 * stats, as RenderInteractions draws it from a light's list: interactionDraws, and when
 * its program is a shadow-atlas one its list's smDrawsLocal, smDrawsGlobal or
 * smDrawsTranslucent. Pure; the render thread's recording.
 */
void countInteractionDraw(LitFrameStats& stats, InteractionList list, int program);

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
 * LP6.1 (C64, D167): passes 1, 5 and 7 draw through draw records (DrawRecords.h): the
 * lists build links each drawn surface, and each (light stage, list, surface) of a
 * light's lists, to its records with one hash lookup; a record resolves at its first draw
 * what DrawElementsWithCounters looked up per draw (the placement, the pipeline, the
 * binding sets held in the cache), and its later draws make the state compare, the parms'
 * write and the draw alone. The table's lifetime and invalidation rules are
 * DrawRecordTable's; passes 6, 8, 9 and the tonemap keep the per-draw path.
 *
 * D76: DRAW_INTERACTION's renderparms are a volatile constant buffer (push constants
 * where the device's limit allows, the layout says which); its writes are counted while
 * recording and a frame writes at most maxVersions / (FramesInFlight + 1), since the
 * versions of the frames before it are recycled only once NVRHI knows they finished: a
 * light whose worst case would pass that is not drawn, nor any light after it in the
 * draw order (LP5.1, P6.13: the rest of its phase and the whole math phase after the
 * texture phase), and they count as overflowed.
 *
 * LP3.1 (C59, D142, D147): pass 6, ShadowAtlasPass, between the ambient pass and the
 * interactions (recordShadowAtlas): the engine's packing of the casting lights' sides
 * into the one 8192² D32 atlas (made at the first frame that places a tile, released in
 * teardown step 2), every tile cleared and drawn every frame, the rects last frame's
 * tiles left cleared first.
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
    const void* instances = nullptr;
    const void* entities = nullptr;
    const LightTable* lights = nullptr;
    std::uint64_t lightsVersion = 0;
    // LP4.1 (D79): a new bake brings a new tree and AreaData; an entity's references
    // change only when a batch applies, which raises chunksVersion
    const void* tree = nullptr;
    const void* areas = nullptr;
    // LP5.3 (D123): a new table comes with a new light table or AreaData
    const void* portalFog = nullptr;
    // LP4.3 (D79, P4.6): the grid table, and with a grid which of its areas' grids are
    // valid now (a Ready or Missing atlas changes the grid-area dispatch)
    const void* grid = nullptr;
    std::uint64_t gridVersion = 0;
    std::uint64_t gridValidity = 0;
    ListsTableKey table;
    // DX.2: the light triangles' answers, and whether the cull applies (a test's switch)
    std::uint64_t lightTrianglesVersion = 0;
    bool lightTriangleCull = true;
    // LP3.1 (C57): r_skipShadows 1's stand-in (a test's switch)
    bool skipShadows = false;

    bool operator==(const ListsKey&) const = default;
  };

  // LP6.1 (C64): a light's links into its records for the lists built: per list (local,
  // global, translucent) the run of its stages' links (each a run of (surface, records)
  // in engineOrderInteractionSurfaces' order), and its D76 worst case (worstCaseWrites)
  struct InteractionLink
  {
    const LitSurface* surface = nullptr;
    InteractionRecords* records = nullptr;
  };
  struct StageLink
  {
    std::size_t stage = 0;
    std::size_t first = 0;
    std::size_t count = 0;
  };
  struct LightLink
  {
    std::array<std::pair<std::size_t, std::size_t>, 3> lists = {};
    std::uint64_t worstCaseWrites = 0;
  };
  // LP6.1 (C64): the ambient pass's set 2 of a frame, the view's units (3, 4, 7-14): the
  // probe path's (0) and a grid area's (1 + area), made at its first draw
  struct AmbientViewSet
  {
    nvrhi::IBindingSet* set = nullptr;
    bool asked = false;
  };
  // LP6.1 (C64): what bindingSets is asked: the sets to make (a bit per set; the ambient
  // pass's records leave set 2, the view's, to AmbientViewSet) and, for a record, whether
  // it holds them: then their entries and the slot versions they were made with
  struct SetRequest
  {
    std::uint32_t sets = ~std::uint32_t{0};
    bool hold = false;
    std::array<BindingSetCache::Entry*, nvrhi::c_MaxBindingLayouts> entries = {};
    InlineVector<SlotVersion, DrawRecordSlots> slots;
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

  // what a draw reads its vertices and indexes from: a chunk's arena ranges, or (LP5.3)
  // the fog cube's buffers
  struct DrawGeometry
  {
    nvrhi::IBuffer* vertexBuffer = nullptr;
    nvrhi::IBuffer* indexBuffer = nullptr;
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t baseVertex = 0;
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

  // LP6.1 (C64): what drawGeometry and a record's draw hand DrawElementsWithCounters'
  // last part: the pipeline, the layout, the sets, the buffers and ranges
  // (constantBuffer: the layout's volatile buffer, null to look it up)
  struct DrawSubmit
  {
    nvrhi::IGraphicsPipeline* pipeline = nullptr;
    EngineBindingLayout layout = EngineBindingLayout::Num;
    bool pushConstants = false;
    nvrhi::IBuffer* constantBuffer = nullptr;
    std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts> sets = {};
    std::size_t setCount = 0;
    DrawGeometry geometry;
  };

  // what the frame's draws use, set per frame
  struct Frame
  {
    const LitFrameInputs* inputs = nullptr;
    LitView view;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    FramebufferFormats formats;
    // LP3.1 (C59): a pass's own viewport (pass 6's tile), else the view's
    std::optional<nvrhi::Viewport> viewport;
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
  // ambient pass (LP4.2a). LP6.1 (C64): each unit's image slot, taken where the pass
  // decides the unit (setUnit: its key's one hash), so that the binding reads the slot
  // directly; a key whose slot the session has not made yet is kept beside it and asked
  // for by the binding (unitSlot), as the binding asked for every key before: a slot is
  // made by the first draw that binds its image
  std::array<std::optional<ImageSlotId>, LitTextureUnits> m_units;
  std::array<std::optional<MaterialImageKey>, LitTextureUnits> m_unitKeys;
  /**
   * LP6.1 (C64): the intrinsic images the passes bind by name: _brdfLut, _white,
   * _default, _defaultUACRadiance, _blueNoise256, _fog, _fogEnter, and
   * DrawSingleInteraction's and a pending surface's fallbacks _black, _blackDiffuse and
   * _flat. Each one's slot is asked for at its first use in a session (so that a session
   * asks for no image it did not ask for before) and kept: an ImageSlotId is an index
   * into the slot table, which only grows until its releaseAll (teardown step 2, after
   * this one's), so the kept ids hold for the session's ImageSlots (m_intrinsicsOf).
   */
  enum class Intrinsic
  {
    BrdfLut,
    White,
    Default,
    DefaultCube,
    BlueNoise,
    Fog,
    FogEnter,
    Black,
    BlackDiffuse,
    Flat,
    Count,
  };
  std::array<ImageSlotId, std::size_t(Intrinsic::Count)> m_intrinsics = {};
  const ImageSlots* m_intrinsicsOf = nullptr;
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
  // LP3.4 (D158): the TEXGEN layout's set 1 is the shadow atlas (debug_shadowmap's draw)
  bool m_debugAtlasBinding = false;
  // V2.2's measurement: the static interaction cull applies (the engine's behaviour)
  bool m_lightTriangleCull = true;
  // C38 (LP4.2b): the interactions are not recorded (r_skipInteractions 1)
  bool m_skipInteractions = false;
  // LP5.0 (D118): r_skipAmbient 1's stand-in, which the stage pass reads (LP5.2a)
  bool m_skipShaderPasses = false;
  // LP5.2a (C46, D115): the drawn surfaces in the stage pass's order (stagePassOrder),
  // made when the lists are built; pointers into m_lists.drawSurfs
  std::vector<const LitSurface*> m_stageOrder;
  // LP6.1 (C64): the draw records of passes 1, 5 and 7, and the lists' links into them:
  // the drawn surfaces' (m_lists.drawSurfs' order) and the lights' (m_lists.lights'
  // order, fog and blend lights and lights without interactions empty); stale after a
  // clear outside a lists build (a generation, the volatile buffers' size): linked again
  // before the next frame's passes
  DrawRecordTable m_records;
  std::vector<SurfaceRecords*> m_surfaceLinks;
  // LP6.1 (D167): the prepass's walk of the links: the leading subview surfaces' count
  // (the generic path, in their order) and the perforated surfaces after them (the
  // generic path after the opaque draws, in the lists' order)
  std::size_t m_prepassSubviews = 0;
  std::vector<const LitSurface*> m_prepassPerforated;
  std::vector<LightLink> m_lightLinks;
  std::vector<StageLink> m_stageLinks;
  std::vector<InteractionLink> m_interactionLinks;
  bool m_linksStale = false;
  // LP6.1 (C64): the frame's ambient view sets, and AMBIENT_LIGHTING_IBL's layouts (every
  // ambient program's, kept at the first ambient record's resolution) they are made for
  std::vector<AmbientViewSet> m_ambientViewSets;
  std::vector<nvrhi::BindingLayoutHandle> m_ambientLayouts;
  bool m_ambientPush = false;
  // LP5.2a (C47, D9): a newStage program's row by its name, or none (not in the table, or
  // a blob missing), looked up once; the names already warned about
  std::map<std::string, std::optional<EngineMaterialProgram>> m_materialPrograms;
  std::set<std::string> m_programWarnings;
  // LP5.2b: said once each: TG_REFLECT_CUBE2 not drawn, TG_DIFFUSE_CUBE or TG_GLASSWARP
  // (the engine's own warning), a stage without an image or a cube on texture_color
  bool m_texgenLp6Warned = false;
  bool m_texgenWarned = false;
  bool m_stageImageWarned = false;
  // LP5.3 (C49, D122): the fog cube (R_MakeZeroOneCubeTris) as a vertex and an index
  // buffer of this session's, made and written at the first fog light's draw
  nvrhi::BufferHandle m_fogCubeVertices;
  nvrhi::BufferHandle m_fogCubeIndexes;
  // ----- LP3.1 (C59, D142, D145, D147): pass 6 -----
  // r_skipShadows 1's stand-in (a test's switch, SessionOptions::skipShadowsForTesting)
  bool m_skipShadows = false;
  // the renderer's tileMap, Init( r_shadowMapAtlasSize, shadowMapResolutions[0], 8 ) at
  // the first frame that packs (RenderBackend_NVRHI.cpp:204-215)
  EngineTileMap m_tileMap;
  bool m_tileMapReady = false;
  // _shadowMapAtlas and its depth-only framebuffer _shadowAtlas (Image_intrinsic.cpp:
  // 672-675, Framebuffer_NVRHI.cpp:136-138), made at the first frame that places a tile
  // and cleared whole once then; whether its creation failed (said once)
  nvrhi::TextureHandle m_atlas;
  nvrhi::FramebufferHandle m_atlasFramebuffer;
  bool m_atlasFailed = false;
  // the rects the atlas holds tiles in since their last clear (every tile drawn the last
  // time the pass ran), which the next run clears where its own tiles do not cover them
  std::vector<AtlasRect> m_atlasOccupied;
  // the frame's casting lights' records, in viewLights order, and each list light's
  // record (-1: none)
  std::vector<ShadowLightRecord> m_shadowRecords;
  std::vector<int> m_shadowRecordOf;
  // LP3.3 (C61, D150): the records that persist across frames, one per light whose tiles
  // are in the layout (ShadowCache), and the bake generation they are drawn under: a
  // count moved whenever the scene's tree or AreaData pointer changes (a bake arriving,
  // changing or going)
  ShadowCache m_shadowCache;
  // LP3.3 (D146, P3.4, §3.10's render-target line): the 2048² D32 scratch tiles are
  // relocated through, made with the atlas when the device copies D32 images and released
  // with it; the copy switched off (a test's: the fallback's redraw)
  nvrhi::TextureHandle m_relocationScratch;
  bool m_depthCopyOff = false;
  // LP3.3 (C62, D152): the held layout's free space (the atlas's nodes the kept records'
  // tiles take), made at the first frame that packs; a test's moving or settling frames
  std::optional<ShadowTileAllocator> m_heldLayout;
  std::optional<bool> m_shadowsMovingForTesting;
  std::uint64_t m_bakeGeneration = 0;
  const void* m_bakeTree = nullptr;
  const void* m_bakeAreas = nullptr;
  // LP3.1 (C58): the frame's split frustums (R_SetupSplitFrustums), made by pass 6
  EngineSplitFrustums m_splitFrustums;
  // D147's far-plane quad (z = w = 1) as a vertex and an index buffer of this session's,
  // made and written at the first clear
  nvrhi::BufferHandle m_clearQuadVertices;
  nvrhi::BufferHandle m_clearQuadIndexes;
  bool m_atlasWarned = false;
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
  // LP3.1 (A49, §3.10), render thread: the shadow atlas's bytes once it is made (8192² ×
  // 4, 256 MB), 0 before: the render-target budget line's
  std::uint64_t shadowAtlasBytes() const;
  // LP3.1 (C59, C61), render thread: the last frame's casting lights' records
  const std::vector<ShadowLightRecord>& shadowLights() const;
  // LP3.1 (C59), render thread: the shadow atlas, or null before the first frame that
  // places a tile (tests read it back)
  nvrhi::ITexture* shadowAtlas() const;
  // LP3.3 (C61), render thread: the records kept across frames (tests)
  const ShadowCache& shadowCache() const;
  // LP3.3 (S30, P3.26), render thread: the tiles the held layout's free-rect structure
  // holds (tests: they return with the records)
  std::size_t shadowLayoutTilesForTesting() const;
  // LP3.3 (D146, A49), render thread: the relocation scratch's bytes once it is made
  // (2048² × 4, 16 MB), 0 before
  std::uint64_t shadowScratchBytes() const;
  /**
   * LP3.4 (D158, R5.15), render thread: a debug view into framebuffer (ldrFBO, of
   * formats) over the tonemapped picture, with inputs' device, pipelines, slots, binding
   * sets and samplers; nothing for None or before the atlas is made. ShadowAtlas: the
   * engine has no drawer for BUILTIN_DEBUG_SHADOWMAP at 285ea4d1, so the shaders' own
   * contract (its .vs: the position by rpMVPmatrixX..W, TEXCOORD0 by TEXGEN_0 when
   * rpTexGen0Enabled.x > 0; its .ps: t_t1 at set 1 t0, LinearSampler at set 2 s0,
   * SampleLevel( tc.xy, rpScreenCorrectionFactor.x ), the mip level): D147's clip-space
   * square through the identity MVP, texcoords 0..1 from its positions (TEXGEN_0_S ( 0.5,
   * 0, 0, 0.5 ), _T ( 0, -0.5, 0, 0.5 ): the atlas's first row at the top), level 0, the
   * atlas at t0 and the common
   * linear-clamp sampler, into the largest centred square of the view (the 8192²
   * fitted), DEPTHFUNC_ALWAYS, no depth write, both sides: each texel's depth in red (the
   * far clear 1.0); drawn with the engine's builtin/texture (14: the same .vs, its .ps
   * tex.Sample( samp0 ) times rpColor 1 under the TEXTURE layout's same sets), because
   * debug_shadowmap's .vs writes no SV_Position and draws nothing on Vulkan (E-LIT-15).
   * TileAge: that, then every placed tile's rect of the kept records through
   * builtin/color (1) blended at alpha 0.5: provisional magenta, relocated this frame
   * blue, drawn this frame green, cached by age a = frames since it was drawn from yellow
   * (a 1) to red (a 64 and more), ( 1, 1 - t, 0 ) with t = log2( a ) / 6.
   */
  void recordDebugView(
    const LitFrameInputs& inputs,
    nvrhi::IFramebuffer* framebuffer,
    const FramebufferFormats& formats,
    DebugView view);

  // teardown step 2: every handle
  void releaseAll();

  // LP6.1 (C64, P8.1), render thread: a slot's version or adopted usage changed: the
  // records resolved with it released, before the binding-set cache erases its sets
  // (RenderThread::eraseChangedSets); how many
  std::size_t recordsSlotChanged(ImageSlotId slot);
  // LP6.1 (C64), render thread: the render targets were made again: every record cleared
  // before the binding-set cache erases the old targets' sets
  void renderTargetsChanged(std::uint64_t generation);
  // LP6.1 (C64), render thread: the draw records (tests)
  const DrawRecordTable& recordsForTesting() const;

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
  // LP3.1 (C57): r_skipShadows 1's stand-in (SessionOptions::skipShadowsForTesting):
  // while on, no light takes tiles (the frontend makes no shadow LOD and gathers no
  // caster), pass 6 is not recorded (RenderBackend.cpp:3605) and every light draws the
  // plain interaction (:1417, :1541); the lists are built again
  void setSkipShadowsForTesting(bool on);
  // LP3.3 (C61, D160's cached-vs-fresh test): every kept tile is drawn again at the next
  // frame that runs pass 6, as if its light's record had changed
  void invalidateShadowTilesForTesting();
  // LP3.3 (D146, P3.4a): off, a tile relocated at settle is drawn again instead of copied
  // (the fallback of a device that cannot copy D32 images)
  void setShadowDepthCopyForTesting(bool on);
  // LP3.3 (C62): the frames' pass 6 moving (true: the layout and LODs held) or settling
  // (false) whatever the camera does; nullopt: the camera's (LitFrameInputs)
  void setShadowsMovingForTesting(std::optional<bool> moving);
  const std::vector<LitDrawRecord>& drawLogForTesting() const;
  // the pipeline keys the draws asked for while the log was on, also those of skipped
  // draws (a pending pipeline)
  const std::set<PipelineKey, PipelineKeyLess>& requestedKeysForTesting() const;

private:
  // record's frame (record sets the held records after it)
  void recordFrame(const LitFrameInputs& inputs);
  void rebuildLists(const LitFrameInputs& inputs, const LitView& view);
  // LP6.1 (C64): the lists linked to their records, one hash lookup per link (after a
  // build, or after a clear outside one)
  void linkRecords();
  // LP6.1 (C64): a surface's ambient records: engineAmbientSurfaceInteractions' draws,
  // made at the ambient pass's first draw of it (its usage asks for the specular images
  // then, as the stage loop did every frame before)
  void fillAmbientRecords(
    SurfaceRecords& records, const LitSurface& surface, const ImageUsage& usage);
  // LP6.1 (C64): a (light stage, list, surface)'s interaction records: the stage loop's
  // draws from a state of their own (InteractionDraw::inherited marks what they read of
  // the surfaces before) and what the loop leaves of the carried fields, made at its
  // first draw
  void fillInteractionRecords(
    InteractionRecords& records,
    const LitSurface& surface,
    const Vec4& diffuseColor,
    const Vec4& specularColor,
    bool noSpecular,
    bool ambientLight,
    const ImageUsage& usage);
  // C16: a pass's timer query, begun and ended around it; endTimer says whether it ran
  void beginTimer(Frame& frame, nvrhi::ITimerQuery* timer);
  bool endTimer(Frame& frame, nvrhi::ITimerQuery* timer);
  void recordClears(Frame& frame);
  void recordPrepass(Frame& frame);
  // FillDepthBufferGeneric over surfaces
  bool genericDepth(Frame& frame, const std::vector<const LitSurface*>& surfaces);
  // LP3.4 (D155): what bindDepthStageDraw leaves for a draw
  struct DepthStageBinding
  {
    int program = 0;
    // false: not drawn (a texture_color stage without an image; retry: a bump image not
    // written yet, the frame unconverged and the draw tried again)
    bool drawable = true;
    bool retry = false;
    // the stage's image is not complete (its slot Pending or Loading: _default or the
    // levels at hand bound; a cube not written: the zero cube)
    bool pending = false;
  };
  /**
   * LP3.4 (D155, D156): what a draw of the generic depth path (FillDepthBufferGeneric,
   * RenderBackend.cpp:832-866) or of the perforated shadow pass (ShadowMapPassPerforated
   * :3322-3352) binds for surface before it is drawn: GL_Color and ALPHA_TEST when the
   * draw sets them, and for an alpha-tested stage's draw texture_color's (or LP5.2b's
   * replacing program's) units, SVC_IGNORE's vertex colour parms, the texture matrix and
   * the texgen (TG_SCREEN's planes from the space's modelViewMatrix and the view's
   * projection, as the engine's PrepareStageTexturing reads them in both passes); the
   * program to draw with. Render thread.
   */
  DepthStageBinding bindDepthStageDraw(
    Frame& frame, const LitSurface& surface, const PrepassDraw& draw);
  void recordInteractions(Frame& frame);
  // LP4.2a (C34): AmbientPass( fillGbuffer false ) on the probe path; LP4.3 (C36): and
  // on the light-grid path for a surface with a grid area (RenderBackend.cpp:1196-1305)
  void recordAmbient(Frame& frame);
  /**
   * LP3.1 (C59, D142, D145, D147, SC714-SC720): ShadowAtlasPass (RenderBackend.cpp:
   * 3603-3922): nothing under r_skipShadows (the test's stand-in) or without view lights
   * (:3605); loop 1 over the lists' lights in viewLights order (fog and blend lights,
   * lights without interactions, LOD -1 and lights without casters skipped: :3653-3674)
   * appends each side's ( res[lod], res[lod] ), engineShadowAtlasLayout packs them
   * (:3718-3783), and loop 2 (:3798-3895) fills each light's record: imageSize from its
   * first side's output, imageAtlasOffset per side, x -1 when a side found no tile. The
   * atlas is made at the first frame that places a tile and cleared whole then; it is not
   * cleared whole every frame (:3640-3643): the rects last run's tiles occupied that this
   * run's do not cover are cleared first (P3.3), then every placed side's tile gets its
   * far-plane clear (D147) and its draws. A frame with neither a tile nor a rect to clear
   * records nothing.
   */
  void recordShadowAtlas(Frame& frame);
  // LP3.1: the atlas and its framebuffer, made at the first call and cleared whole into
  // the frame's command list (Image_intrinsic.cpp:672-675: TD_DEPTH at 8192², a render
  // target; IN:556-566: initial state DepthWrite, clear value 1); false when it cannot be
  // made (said once). LP3.3 (D146): and the relocation scratch, when the device copies
  // D32 images (no scratch: moved tiles are drawn again)
  bool ensureShadowAtlas(Frame& frame);
  /**
   * LP3.3 (D146, C61, P3.4): the copies of plan through the scratch, in its order (its
   * saves atlas → scratch, its restores scratch → atlas: copyTexture D32 → D32, NVRHI's
   * barriers between the phases), each move logged with its light and side.
   */
  void relocateTiles(
    Frame& frame,
    const ShadowRelocationPlan& plan,
    std::span<const std::pair<PreviewId, int>> moved);
  // LP3.1 (D147): a far-plane quad over rect of the atlas, under builtin/depth with the
  // identity MVP, DEPTHFUNC_ALWAYS | CULL_TWOSIDED and no offset: depth 1.0 under the
  // rect's viewport and scissor; false when the pass is disabled
  // LP3.3 (C61): cleared, whether the quad was drawn (a pending pipeline skips it: the
  // rect is cleared again, its tile drawn again, at the next frame)
  bool clearAtlasRect(
    Frame& frame, const AtlasRect& rect, PreviewId light, int side, bool& cleared);
  /**
   * LP3.1 (C59, D149, SC721-SC722, SC749): ShadowMapPassFast( globalShadows, vLight,
   * side, atlas ) (RenderBackend.cpp:3395-3584) into the side's tile: every caster of the
   * light (ViewLight::casters, on screen or off, K49) by cast class: Opaque on
   * builtin/depth with its chunk's or instance's arena range and its light-triangle runs
   * (a Pending one whole), as the prepass draws it (SC749), under GLS_POLYGON_OFFSET |
   * CULL_TWOSIDED with slope r_shadowMapPolygonFactor and int bias
   * r_vkShadowMapPolygonOffset, depth LessOrEqual, written, its MVP the side's raster
   * pair times the model matrix (spot: windowToClip first); TranslucentCaster nothing
   * (:3506-3509); a caster outside the side's true frustum (engineShadowCasterCulled) is
   * not drawn. LP3.4 (D155, D156, K52, K53): then ShadowMapPassPerforated (:3171-3388),
   * which the fast pass calls after its opaque surfaces (:3576-3581): the Perforated
   * casters in the preview's chunk and instance order, each by
   * enginePerforatedShadowDraws of the registers the gathering carried (the side's state
   * carried from caster to caster from the opening on) through bindDepthStageDraw, culled
   * as the opaque ones, each draw counted; a material or registers pending: drawn solid
   * as an opaque caster; an image not complete or registers pending or not current make
   * the tile provisional (drawn.provisional). False when the pass is disabled.
   */
  bool shadowMapPass(
    Frame& frame,
    const ShadowLightRecord& record,
    int side,
    const EngineShadowSideMatrices& matrices,
    ShadowTileDraws& drawn);
  // LP3.1 (C59, SC719): GL_ViewportAndScissor( x, y, size, size ) on the atlas: the
  // frame's viewport and m_scissor (RenderBackend_NVRHI.cpp:2176-2193)
  void setAtlasViewport(Frame& frame, const AtlasRect& rect);
  // D147's quad, its buffers made and written into the frame's command list the first
  // time; nullopt when a buffer cannot be made
  std::optional<DrawGeometry> clearQuadGeometry(Frame& frame);
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
   * fragment images at units 0-2 (a null one _default); LP5.2b (C48): a cube or sky
   * texgen's program (55, 57, 69, 70) with its cube at unit 0 (cubeImage), 57's bump
   * image at unit 1 (bumpImage) and 70's WOBBLESKY_X..Z; at the end GL_Color( 1 ) and
   * GLS_DEFAULT (no draw).
   */
  void recordStages(Frame& frame);
  /**
   * LP5.3 (C49, D122-D124): FogAllLights (RenderBackend.cpp:5038-5076, from :6136) into
   * hdrFBO with the prepass's depth: nothing under r_skipFogLights (EngineFrame, its
   * blend lights too, as the engine's return is before the walk) or without view
   * lights; then over the view lights in the sorted order, FogPass for a fog light
   * (fogPass), BlendLight for a blend light (blendLight, LP5.3 step 3).
   */
  void recordFogLights(Frame& frame);
  /**
   * LP5.3 (C49, SC533-SC535): FogPass( global, local, vLight ) (RenderBackend.cpp:
   * 4940-5031): engineFogParms' colour (GL_Color) and planes; units 0 _fog and 1
   * _fogEnter (intrinsics through the slot table); DEPTHMASK | SRC_ALPHA /
   * ONE_MINUS_SRC_ALPHA | DEPTHFUNC_EQUAL over the global then the local list
   * (basicFog), then the same with DEPTHFUNC_LESS | GLS_CULL_BACKSIDED over the fog cube
   * (the zero-to-one cube in world space through inverseBaseLightProject, with the view's
   * scissor), whose BACKSIDED request the copied GetRenderState decodes as the
   * front-sided case (mirror.json M-FOG-DEFECT-01). False when the pass is disabled.
   */
  bool fogPass(Frame& frame, const ViewLight& light);
  // T_BasicFog( drawSurfs, fogPlanes, NULL ) (RenderBackend.cpp:4868-4933) over a list
  bool basicFog(
    Frame& frame,
    const ViewLight& light,
    const std::vector<LitSurface>& surfaces,
    const std::array<EnginePlane, 4>& planes,
    std::uint64_t stateBits);
  // T_BasicFog( &zeroOneCubeSurface, fogPlanes, &inverseBaseLightProject )
  bool fogCube(
    Frame& frame,
    const ViewLight& light,
    const std::array<EnginePlane, 4>& planes,
    std::uint64_t stateBits);
  /**
   * LP5.3 (C49, SC537): BlendLight( global, local, vLight ) (RenderBackend.cpp:
   * 4796-4853): it returns when the global list is NULL (a blend light whose surfaces
   * are all local, a light without noShadows over noSelfShadow surfaces, never draws:
   * mirrored) and under r_skipBlendLights; unit 1 the light's falloff image, then per
   * stage whose condition is on GL_State( DEPTHMASK | drawStateBits | DEPTHFUNC_EQUAL )
   * (EQUAL is 3 << 13 and covers any depth function of drawStateBits), unit 0 the
   * stage's image (both by the slot's state as stageImage: Pending, or Loading without a
   * level, skips the stage's draws, the frame unconverged), the texture matrix loaded and
   * never read (set 7 has no TEXTUREMATRIX: mirrored), GL_Color( the stage's colour
   * registers ) clamped without r_lightScale, and T_BlendLight on the global then the
   * local list (tBlendLight). False when the pass is disabled.
   */
  bool blendLight(Frame& frame, const ViewLight& light);
  // T_BlendLight (RenderBackend.cpp:4731-4786) over a list: MVP and the light's
  // lightProject through R_GlobalPlaneToLocal on a space change, TEXGEN_0_S/T/Q its rows
  // 0-2, TEXGEN_1_S row 3 (the falloff)
  bool tBlendLight(
    Frame& frame,
    const ViewLight& light,
    const std::vector<LitSurface>& surfaces,
    std::uint64_t stateBits);
  // T_BasicFog's TEXGEN_0_S = p0, TEXGEN_0_T = p1, TEXGEN_1_T = p2, TEXGEN_1_S = p3
  void setFogPlanes(const std::array<EnginePlane, 4>& planes);
  // the fog cube's geometry, its buffers made and written into the frame's command list
  // the first time; nullopt when a buffer cannot be made
  std::optional<DrawGeometry> fogCubeGeometry(Frame& frame);
  // a fog or blend draw's outcome counted; false when the pass is disabled (D9)
  bool countFogDraw(
    Frame& frame,
    DrawOutcome outcome,
    const Chunk* chunk,
    int program,
    std::uint64_t state);
  /**
   * LP5.2b (C48, D124, P6.17): the image a cube program binds at unit 0, and whether it
   * stands in for a cube not written yet. Resident, Replacing (its complete texture) and
   * Loading with written levels bind the cube; Pending and Loading without a level bind
   * the zero cube (_defaultUACRadiance, LP4.2a's black 8 × 8) and are pending (the frame
   * unconverged); Default (the engine's image not loaded or defaulted), a 2D image named
   * by the stage and a stage without an image bind the zero cube too, where the engine
   * binds its 2D _default (or a stale image) into the TextureCube slot (undefined):
   * mirror.json M-STAGE-DIFF-04.
   */
  std::pair<MaterialImageKey, bool> cubeImage(
    ImageSlots& images, const std::optional<MaterialImageKey>& key);
  /**
   * LP5.2b (C48, P6.17): the bump image 57 binds raw at unit 1 (RenderBackend.cpp:
   * 366-368), by its slot's state as stageImage, but Default binds _flat where the engine
   * binds its defaulted image (mirror.json M-STAGE-DIFF-06); a bump stage without an
   * image binds _flat too.
   */
  std::optional<MaterialImageKey> bumpImage(
    ImageSlots& images, const std::optional<MaterialImageKey>& key);
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
  // RenderInteractions for one list; false when the pass is disabled. LP3.1 (C60):
  // shadow is the light's record when pass 6 placed it in the atlas (shadowRecordOf).
  // LP6.1 (C64): its draws are the list's links' records (m_lightLinks[lightIndex])
  bool renderInteractions(
    Frame& frame,
    const ViewLight& light,
    std::size_t lightIndex,
    const ShadowLightRecord* shadow,
    const std::vector<LitSurface>& surfaces,
    InteractionList list,
    std::uint64_t depthFunc);
  /**
   * LP3.1 (C60, SC727): RenderInteractions' shadow parms (RenderBackend.cpp:1784-1866)
   * under !r_skipShadows && shadowLOD > -1, placed or not: jitterTexOffset ( 1 / 512, 1 /
   * 512, 0, 0 ) (the jitter pinned), a parallel light's rpCascadeDistances the split
   * distances; placed: screenCorrection ( 1 / ( 128 · N ), 1 / 128, 1 / 8192, bias ),
   * jitterTexScale ( r_shadowMapJitterScale, imageSize.x / 8192, shadowFadeOut, N ) and
   * the six rpShadowAtlasOffsets ( offset · ( 1 / 8192 ), 0, 0 ) (sides a light has not,
   * 0); else screenCorrection's z 1 / res[lod] and jitterTexScale's y the jitter scale; N
   * r_shadowMapSamples, bias r_shadowMapSunDepthBiasScale or RegularDepthBiasScale.
   */
  void setShadowParms(const ViewLight& light, const ShadowLightRecord* shadow);
  // LP3.1 (C60): the frame's record of a list light when pass 6 placed it in the atlas,
  // else null (r_skipShadows, not casting, LOD -1, no caster, a side without a tile)
  const ShadowLightRecord* shadowRecordOf(const ViewLight& light) const;
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
    const ShadowLightRecord* shadow,
    const MaterialStage& lightStage,
    const std::optional<EngineGlMatrix>& lightTextureMatrix);

  /**
   * LP6.1 (C64): DrawElementsWithCounters' lookups into record, in draw's and
   * drawGeometry's order (the program, the placement, the pipeline, the binding sets the
   * units name, held; the request's sets only), each failure counted as they count it
   * (the record left unresolved, so the next draw asks again: a pending pipeline every
   * frame until it is made, D77); extraSlots join the record's slots (an ambient draw's
   * grid atlas). nullopt once resolved.
   */
  std::optional<DrawOutcome> resolveRecord(
    Frame& frame,
    DrawRecord& record,
    int program,
    std::uint64_t stateBits,
    const Chunk& chunk,
    std::uint32_t sets,
    std::span<const SlotVersion> extraSlots);
  // LP6.1 (C64): a resolved record's draw: its sets stamped (once per frame), then
  // DrawElementsWithCounters' state compare, parms and draw; viewSet is set 2 when set
  // (the ambient pass's view set)
  DrawOutcome drawRecord(
    Frame& frame,
    DrawRecord& record,
    nvrhi::IBindingSet* viewSet,
    const LightTriangles* lightTriangles);
  // LP6.1 (C64): a record's draw, resolved first when it is not
  DrawOutcome drawRecorded(
    Frame& frame,
    DrawRecord& record,
    int program,
    std::uint64_t stateBits,
    const Chunk& chunk,
    const LightTriangles* lightTriangles);
  // LP6.1 (C64): the ambient pass's set 2 of the frame for index (0 the probe path's, 1 +
  // area a grid area's: its atlas at unit 7), made at its first draw; null when a unit
  // has no written level
  nvrhi::IBindingSet* ambientViewSet(
    Frame& frame, std::size_t index, const GridArea* grid);
  // DrawElementsWithCounters' state compare, parms and draw (drawGeometry's last part)
  DrawOutcome submitDraw(
    Frame& frame, const DrawSubmit& submit, const LightTriangles* lightTriangles);

  // DrawElementsWithCounters
  DrawOutcome draw(
    Frame& frame,
    int program,
    const Chunk& chunk,
    std::uint64_t stateBits,
    float slopeBias,
    float depthBias,
    const LightTriangles* lightTriangles = nullptr);
  // DrawElementsWithCounters on the given buffers (draw's body; LP5.3: the fog cube's)
  DrawOutcome drawGeometry(
    Frame& frame,
    int program,
    const DrawGeometry& geometry,
    std::uint64_t stateBits,
    float slopeBias,
    float depthBias,
    const LightTriangles* lightTriangles);
  // the binding sets of a layout for the units; false when a texture has no level yet.
  // LP6.1 (C64): request's sets only, held for a record when it says so
  bool bindingSets(
    Frame& frame,
    EngineBindingLayout layout,
    const std::vector<nvrhi::BindingLayoutHandle>& layouts,
    bool pushConstants,
    std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts>& sets,
    std::size_t& count,
    SetRequest* request = nullptr);
  nvrhi::IBuffer* constantBuffer(Frame& frame, EngineBindingLayout layout);
  // the textureUsage_t a slot's image has once loaded (the PBR rename)
  int imageUsage(ImageSlots& images, const MaterialImageKey& key) const;
  // LP6.1 (C64): the kept intrinsic a key names (intrinsicImageKey's shape), if any
  static std::optional<Intrinsic> keptIntrinsic(const MaterialImageKey& key);
  // LP6.1 (C64): an intrinsic's slot once the session has made it, kept from then on
  std::optional<ImageSlotId> intrinsicSlot(ImageSlots& images, Intrinsic intrinsic);
  // LP6.1 (C64): a key's slot, made on its first ask (a kept intrinsic's without a hash)
  ImageSlotId askSlot(ImageSlots& images, const MaterialImageKey& key);
  // LP6.1 (C64): a unit decided: an intrinsic's kept slot, a key's slot found (one hash)
  // or, when its slot is not made yet, the key kept for the binding to ask for; none
  void setUnit(ImageSlots& images, std::size_t unit, Intrinsic intrinsic);
  void setUnit(ImageSlots& images, std::size_t unit, const MaterialImageKey& key);
  void setUnit(
    ImageSlots& images, std::size_t unit, const std::optional<MaterialImageKey>& key);
  bool hasUnit(std::size_t unit) const;
  // LP6.1 (C64): the binding's slot of a unit, a kept key asked for now
  std::optional<ImageSlotId> unitSlot(ImageSlots& images, std::size_t unit);
  void log(
    const Frame& frame,
    LitDrawRecord::Pass pass,
    const Chunk& chunk,
    int program,
    std::uint64_t state);

  deleteCopyAndMove(LitPasses);
};

} // namespace tb::lit
