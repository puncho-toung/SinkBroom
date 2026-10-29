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

#include "base/Macros.h"
#include "base/Result.h"
#include "lit/LitConfig.h"
#include "lit/VulkanInstance.h"

#include "vm/vec.h"

#include <vulkan/vulkan_core.h>

namespace nvrhi
{
class IDevice;
enum class Format : std::uint8_t;
enum class TextureDimension : std::uint8_t;
} // namespace nvrhi

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

struct SessionOptions
{
  // the validation layer is on the instance; in Debug, NVRHI's validation layer as well
  bool validation = false;
  // FIFO whatever the surface offers (D48, the forced FIFO-only test)
  bool forceFifo = false;
  // the frame log (C16); empty means no log
  std::filesystem::path frameLogPath;
  // C1: the game folder (Map::gamePath()) whose base/renderprogs2/spirv holds the
  // engine's shader blobs; empty or relative: no shader loads, every program is disabled
  // (D9, T0.4)
  std::filesystem::path gamePath;
  // DX.5: the pipelines every lit scene draws with are asked for at Start (phase B), so
  // that the compile thread makes them while the scene loads; tests that order the
  // compile thread's jobs themselves turn it off
  bool requestStartPipelines = true;
  // LP3.1 (C57), tests: r_skipShadows 1's stand-in (EngineFrame keeps the engine's 0): no
  // light takes tiles, pass 6 is not recorded and every light draws the plain interaction
  // (RenderBackend.cpp:1417, 1541, 3605), for a test about something else than shadows
  // whose scene has casting lights
  bool skipShadowsForTesting = false;
  // LP6.4b (C68 (b), C11, A57, Q48): the eviction's budget for the session's GPU memory
  // in use (the live figure); VK_EXT_memory_budget's own budget is the second trigger.
  // C11's panel setting is this field (its control comes with LP6.6)
  std::uint64_t vramBudgetBytes = DefaultVramBudgetBytes;
  // LP6.5a (C69, C70, D176, D177, P8.14): the engine's r_useSSAO 1 (r_useNewSSAOPass 1)
  // with r_taaJitter 0's pixelOffset: pass 3 fills the G-buffer's normals and pass 4 the
  // ambient occlusion the ambient pass reads at t4. Off in TbLitLib tests (a case opts
  // in), on in sessions (LitSessionHost, Q43); the panel's switch is C74's (LP6.6)
  bool ssaoForTesting = false;
  // LP6.5a (P8.14, D181), LP6.6 (C73): the progressive TAA (r_antiAliasing 2's preview
  // form): once the camera stops and every other convergence term holds, 16 jittered
  // frames resolved through blob 89 into _taaResolved, then the frame settles. Off in
  // TbLitLib tests (a case opts in), on in sessions (LitSessionHost); parity keeps it off
  bool progressiveAaForTesting = false;
};

// D9: a program the compile thread was asked for, and whether the preview can draw with
// it ("disabled: <field>" otherwise); the panel's status line shows the disabled ones
struct ProgramReport
{
  // a BUILTIN_ index (lit/EngineBuiltins.h)
  int program = 0;
  // the builtin's name and suffix
  std::string name;
  bool enabled = true;
  std::string reason;

  bool operator==(const ProgramReport&) const = default;
};

/**
 * LP2.3 (D9, D77, C16): the last frame drawn, as the panel's status line shows it: its
 * settled state, the frame log's counts of the lit passes (LitFrameStats) and the VRAM
 * the session's own resources take (GpuMemoryAccount). Published by the render thread
 * after each frame it shows; read on any thread.
 */
/**
 * LP6.4a (C68 (a), D171): the slots' texture heaps as the last frame shown left them:
 * the heaps, their capacity and the bytes their ranges hold (freed ranges waiting for
 * their frames included), the textures in them and the slots' textures outside them
 * (live) with their bytes; so far: the textures placed, the dedicated ones by why (no
 * probe yet, a class refused, larger than a heap, no heap made, a bind refused), the
 * largest and the smallest range placed, the largest dedicated texture; the Start
 * probe's time (P8.3). For the measurements.
 */
struct TextureHeapStatus
{
  std::size_t heaps = 0;
  std::uint64_t capacityBytes = 0;
  std::uint64_t usedBytes = 0;
  std::size_t placed = 0;
  // LP6.4b's figures: the image bytes of the textures in heaps (their levels as written:
  // usedBytes minus this is the drivers' padding of the images and the freed ranges),
  // and the free ranges the heaps hold (their fragments)
  std::uint64_t placedImageBytes = 0;
  std::size_t freeRanges = 0;
  std::uint64_t largestFreeBytes = 0;
  std::size_t dedicated = 0;
  std::uint64_t dedicatedBytes = 0;
  std::uint64_t texturesPlaced = 0;
  std::uint64_t dedicatedNoProbe = 0;
  std::uint64_t dedicatedRefusedClass = 0;
  std::uint64_t dedicatedTooLarge = 0;
  std::uint64_t dedicatedNoHeap = 0;
  std::uint64_t dedicatedBindRefused = 0;
  std::uint64_t largestPlacedBytes = 0;
  std::uint64_t smallestPlacedBytes = 0;
  std::uint64_t largestDedicatedBytes = 0;
  double probeMs = -1.0;

  bool operator==(const TextureHeapStatus&) const = default;
};

struct SessionStatus
{
  // frames shown so far; 0: none yet
  std::uint64_t frames = 0;
  // D77: the last frame was settled
  bool settled = false;
  // LP6.6 (C73, P8.14): its convergence terms but the TAA history's held (the "settled
  // exact frame"; settled itself while TAA is off), the session's progressive TAA is on,
  // and the history's length after the frame (0-16; 16: settled)
  bool exactSettled = false;
  bool taaOn = false;
  std::size_t taaHistory = 0;
  std::size_t lightsVisible = 0;
  std::size_t lightsDrawn = 0;
  std::size_t lightsOverflowed = 0;
  // LP3.4 (D158, P3.25, Q40): the casting lights the shadow atlas left without a tile
  // (unshadowed: LitFrameStats::lightsUnplaced) and their "name" keys in viewLights
  // order (unplacedLights)
  std::size_t lightsUnshadowed = 0;
  std::vector<std::string> unshadowedLights;
  // the lit and tonemap passes' draws, the draws skipped, the pipelines pending
  std::size_t draws = 0;
  // LP6.5a (C69, C70): pass 3's draws among them (0 without SSAO) and whether pass 4 ran
  std::size_t gbufferDraws = 0;
  bool ssaoRan = false;
  // LP6.5b (C71, the director's reading (9)): the surfaces the first stage call left at
  // SS_POST_PROCESS, and the frame's copies of the HDR target into _currentRender (0 or
  // 1: one was left for the second stage call)
  std::size_t stagePostProcess = 0;
  std::size_t stageCopies = 0;
  // LP6.5b (C72): the frame's bumpyenvironment2 draws and the surfaces with such a stage
  // in its lists (pass 3's consumer)
  std::size_t bumpyEnvironment2Draws = 0;
  std::size_t bumpyEnvironment2Listed = 0;
  std::size_t skippedDraws = 0;
  std::size_t pendingPipelines = 0;
  // the session's arenas, textures and targets (GpuMemoryAccount::total)
  std::uint64_t vramBytes = 0;
  // LP6.4a (C68 (a), (e)): its lines: the texture heaps' capacity and the dedicated slot
  // textures (the texture line), the arenas, the render targets and buffers, and what
  // was released and waits for its frames
  std::uint64_t vramHeapBytes = 0;
  std::uint64_t vramDedicatedTextureBytes = 0;
  std::uint64_t vramArenaBytes = 0;
  std::uint64_t vramRenderTargetBytes = 0;
  std::uint64_t vramReleasedBytes = 0;
  // LP6.4b (C68 (b), the director's reading (1)): the live figure the eviction compares
  // with the session's budget (vramBudgetBytes), its lines beside the ones above (the
  // arenas' and the heaps' used bytes), the session's evictions so far and the slots at
  // the mip floor now
  std::uint64_t vramLiveBytes = 0;
  std::uint64_t vramArenaUsedBytes = 0;
  std::uint64_t vramHeapUsedBytes = 0;
  std::uint64_t vramBudgetBytes = 0;
  std::uint64_t texturesEvicted = 0;
  std::size_t texturesFloored = 0;
  // LP6.4b (C68 (d), D174): the geometry arenas per kind, the compactions run (those in
  // an uploads-only idle submission among them) and the bytes they moved
  std::size_t vertexArenas = 0;
  std::size_t indexArenas = 0;
  std::uint64_t compactions = 0;
  std::uint64_t compactionSubmissions = 0;
  std::uint64_t arenaMovedBytes = 0;
  // LP6.4a: the slots' texture heaps
  TextureHeapStatus heaps;
  // LP4.1 (D90's first half, C32): the view's portal flow: the view's area (-1 outside
  // the world), the areas flowed, the tree's area count (0 without a tree: no flow) and
  // the view lights that went on to the pairing
  int viewArea = -1;
  std::size_t areasFlowed = 0;
  std::size_t areas = 0;
  std::size_t lightsInView = 0;

  // LP4.3 (C38, D90, K8, K29): the scene's bake (SceneGeometry::adoptedBake): Unknown
  // before its map path's request is answered (or without a map path: nothing is said),
  // None without a .proc or .bproc, Proc or Bproc for the tree's file, Dropped when the
  // area counts disagree (MapBake::dropReason, the tree kept); the tree file's probe time
  // (Unix seconds; 0 in a container, -1 none); the pieces of world faces in solid
  // (SceneGeometry::solidPieces, K14)
  enum class Bake
  {
    Unknown,
    None,
    Proc,
    Bproc,
    Dropped,
  };
  Bake bake = Bake::Unknown;
  std::string bakeDropReason;
  std::int64_t bakeTime = -1;
  std::size_t solidPieces = 0;
  // the host's (LitSessionHost::status, D90): the map file's last write in Unix seconds
  // (-1 unknown) and whether the document holds unsaved changes
  std::int64_t mapFileTime = -1;
  bool documentModified = false;

  bool operator==(const SessionStatus&) const = default;
};

/**
 * LP7.2 (C52, P7.14): the scene's models in one session, for the measurements and the
 * parity rows: the model entities, their instances, those whose record is Pending and
 * those drawn as nothing for a skinned model (SceneGeometry::ModelCounts), the distinct
 * records the arenas' shared surfaces come from, those surfaces placed and their bytes
 * (vertices and indexes, each placement once however many instances draw it). Tests and
 * measurements only.
 */
struct ModelTotals
{
  std::uint64_t entities = 0;
  std::uint64_t instances = 0;
  std::uint64_t pending = 0;
  std::uint64_t skinned = 0;
  std::uint64_t records = 0;
  std::uint64_t placements = 0;
  std::uint64_t placementBytes = 0;

  bool operator==(const ModelTotals&) const = default;
};

/**
 * LP2.4 (Q14): the compile thread's own timings in one session, for the measurements:
 * its jobs (a pipeline, a compute pipeline or a binding layout type's layouts) from the
 * dequeue to the answer, and within them the driver's pipeline creations alone and the
 * shaders made (the blob read, its reflection and createShader, once per permutation).
 * Tests and measurements only.
 */
struct CompileTimings
{
  std::uint64_t jobs = 0;
  std::uint64_t pipelines = 0;
  std::uint64_t shaders = 0;
  double jobMs = 0.0;
  double pipelineMs = 0.0;
  double shaderMs = 0.0;
  // the first job's start and the last job's end; empty before the first job
  std::optional<std::chrono::steady_clock::time_point> firstJobStart;
  std::optional<std::chrono::steady_clock::time_point> lastJobEnd;
};

/**
 * LP3.4 (D158, R5.15): the shadow debug views the Lit panel selects, drawn after the lit
 * passes into the frame's LDR target (over the tonemapped picture, before its blit into
 * the frame's target) and only while one is selected: None (the default, which
 * [.][parity] and [.][measure] never change); ShadowAtlas, the whole 8192² atlas by
 * builtin/debug/debug_shadowmap's (107) contract through the engine's builtin/texture
 * (14; 107 itself draws nothing on Vulkan, E-LIT-15), fitted to the view; TileAge, that
 * picture with every placed tile's rect tinted by its state and age through the engine's
 * builtin/color (1).
 */
enum class DebugView
{
  None,
  ShadowAtlas,
  TileAge,
};

/**
 * D9 (LP2.3): the panel's status line: "starting" before the first frame, then "settled"
 * or "drawing", the lights drawn of those visible (and overflowed; LP3.4, D158: and ", N
 * unshadowed (atlas full)" for the casting lights the atlas left without a tile), the
 * draws (skipped),
 * the pipelines pending and the VRAM, and while a tree exists (D90, LP4.1) "area A, F of
 * N areas" (the view's area, -1 outside the world, the areas flowed and the tree's);
 * LP4.3 (D90, K8, K29) once the bake is known "bake: none", "proc", "bproc" or
 * "dropped (<reason>)", with a tree ", stale" when the document holds unsaved changes or
 * the map file was written after the tree's file (a positive probe time: a container's
 * time 0 says nothing), and ", <n> pieces outside the baked hull" when world faces reach
 * solid; then every disabled program with its reason, "ambient disabled: ..." for one of
 * the ambient pass's (LP4.2a: the pass draws nothing without it). Pure; any thread.
 */
std::string sessionStatusLine(
  const SessionStatus& status, const std::vector<ProgramReport>& programs);

/**
 * LP3.4 (D158, P3.25, Q40): the status line's tooltip: the line, then, when the atlas
 * left lights unshadowed, a second line "unshadowed: " and their names (the line itself
 * says ", N unshadowed (atlas full)" after the lights drawn). Pure; any thread.
 */
std::string sessionStatusTooltip(
  const SessionStatus& status, const std::vector<ProgramReport>& programs);

// A copy of the 3D view's camera (§3.2); never a pointer to it.
struct CameraState
{
  vm::vec3f position = vm::vec3f{0, 0, 0};
  vm::vec3f direction = vm::vec3f{1, 0, 0};
  vm::vec3f up = vm::vec3f{0, 0, 1};
  float fovDegrees = 90.0f;
  std::uint32_t viewportWidth = 0;
  std::uint32_t viewportHeight = 0;

  bool operator==(const CameraState&) const = default;
};

enum class SessionEvent
{
  // the first frame of the session was presented (headless: finished); the message
  // names the GPU
  Ready,
  // the render thread parked; sent also when park() timed out and it parked later
  Parked,
  // the device is lost: nothing more is drawn, press Start
  DeviceLost,
  // a session error, also an NVRHI error that is not a lost device; nothing more is
  // drawn, Stop and Start
  Error,
  // a warning to log (an NVRHI warning, an unwritable frame log); the session goes on
  Warning,
  // a line of the material parse thread's log (lit/MaterialService.h): MaterialInfo for
  // its summaries, MaterialWarning for the copied parser's warnings and errors and for
  // the game files it could not read; the session goes on (R4-1.17)
  MaterialInfo,
  MaterialWarning,
  // the material service published a new table (MaterialService::table()); the owner
  // posts it from the parse thread's listener, at most one at a time until it is handled
  MaterialsPublished,
  // acquire or present returned OUT_OF_DATE or SUBOPTIMAL: the GUI thread should park,
  // destroySwapchain, createSwapchain and unpark; posted once per swapchain generation
  SwapchainOutOfDate,
  // S31, B0.3: the inbox is empty: posted when the render thread starts and whenever it
  // has taken the snapshot offered (LitSession::offerSnapshot); it does not mean that
  // the last snapshot's chunks are applied
  ReadyForSnapshot,
};

/**
 * Called from the render thread, and from the GUI thread inside create, createSwapchain
 * and the teardown steps; the Qt side marshals to the GUI thread and drops a message
 * whose generation is not the live session's (R5.17). It must not block and must be safe
 * to call from any thread.
 */
using EventSink =
  std::function<void(SessionEvent, std::string message, std::uint64_t generation)>;

/**
 * LP2.2b (C29, P2.25): a copy of a settled frame's LDR target (_currentRenderLDR,
 * RGBA8_UNORM), the engine's screenshot source, at the target's size: rows top-down, four
 * bytes a pixel. The engine's screenshot forces alpha to 255; parity compares RGB.
 */
struct LdrImage
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

/**
 * LP6.4b (C68 (b), D172), tests: what a frame's budget pass did: the frame, the slots it
 * evicted with their last-bound frames, those it floored and the Evicted ones it asked
 * again, the slots a set of the frame bound (their last-bound frame is the frame: the
 * stamp), and the live figure, the budget and whether either trigger was passed.
 */
struct BudgetPassReport
{
  std::uint64_t frame = 0;
  std::vector<std::pair<std::uint32_t, std::uint64_t>> evicted;
  std::vector<std::uint32_t> floored;
  std::vector<std::uint32_t> reRequested;
  std::vector<std::uint32_t> drawn;
  std::uint64_t liveBytes = 0;
  std::uint64_t budgetBytes = 0;
  bool over = false;
  bool left = false;
};

// The device-local heaps' VRAM, from VK_EXT_memory_budget.
struct VramUsage
{
  std::uint64_t usedBytes = 0;
  std::uint64_t budgetBytes = 0;
};

// C30: what the session's device tells the engine's rules (P2.10, P2.13)
struct DeviceFacts
{
  std::uint32_t maxPushConstantsSize = 0;
  std::uint32_t vendorID = 0;
  std::uint32_t maxMemoryAllocationCount = 0;
  // min(maxPushConstantsSize, NVRHI's c_MaxPushConstantSize)
  std::uint32_t pushConstantLimit = 0;
  bool d24s8Supported = false;
};

/**
 * What a session's teardown found, for its owner, which logs and shows a report that is
 * not clean. Complete once releaseGpuObjects has run. Messages that the session would
 * otherwise post while it stops land here, because no owner acts on them any more.
 */
struct TeardownReport
{
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t deviceReleaseCount = 0;
  std::size_t nvrhiErrors = 0;
  // lost during the session, or found lost while stopping
  bool deviceLost = false;
  // the frame log could not be written in full
  std::optional<std::string> frameLogError;
  // the warnings and errors raised while stopping
  std::vector<std::string> messages;
  std::uint64_t framesPresented = 0;
  std::uint64_t swapchainGenerations = 0;
  // when the session had its device, and once its objects were released, before the
  // device went; empty when the device does not report its budget
  std::optional<VramUsage> vramAtStart;
  std::optional<VramUsage> vramAtStop;
};

// Test hooks: faults the render thread raises in its next frame.
enum class DebugFault
{
  // vk::DeviceLostError at the start of the frame, before any GPU work
  DeviceLostBeforeFrame,
  // vk::DeviceLostError once the frame has acquired its image, before it records
  DeviceLostAfterAcquire,
  // NVRHI's "Device Removed!" through its message callback inside the submit, the only
  // way NVRHI reports a loss in executeCommandLists (R5.19)
  DeviceRemovedInSubmit,
  // an Error-severity NVRHI message, as from NVRHI's validation layer, after the submit
  NvrhiError,
  // an exception while the frame records its commands
  ThrowWhileRecording,
};

// Test hooks: where the render thread runs a test's function.
enum class DebugHook
{
  // in every frame after its commands are submitted and before it presents: a frame held
  // there is in flight, as one waiting inside a present would be
  BeforePresent,
  // in every park, once the generation's image wrappers are released, before the thread
  // counts as parked
  WhileParking,
  // in every park, once the thread counts as parked, before it posts Parked
  AfterParking,
  // in every pass that draws no frame, while the render thread's mutex is released for
  // the chunk jobs, before the pass chooses its wait
  IdleUnlocked,
  // LP4.2a: in every frame that records its lit passes, after the depth prepass and
  // before the pass boundary of the ambient pass
  AfterPrepass,
};

// §3.1: the longest park() waits; the plan's bound is the current pass plus two frames in
// flight, about 250 ms in a settled frame
constexpr auto ParkTimeout = std::chrono::milliseconds{500};

struct CellRef;
class CpuPool;
struct FirstBatchCounts;
class GpuDevice;
struct EngineTonemapDefaults;
class GridTablePublication;
class LightTablePublication;
class ProbeTablePublication;
class MaterialService;
class PipelineCompiler;
class RenderThread;
class SessionEvents;
struct Chunk;
struct ChunkKey;
struct GridTable;
struct LightTable;
struct ProbeTable;
struct RenderFrameAccess;
struct Snapshot;

/**
 * Everything a running preview owns below Qt (C18, D34): the VkDevice, the NVRHI device,
 * the swapchain generations, the render thread, the CPU pool and the frame log. The
 * instance, the surface and the material service belong to the caller. Nothing in it is
 * static. Its parts live in TbLitLib's sources, so that vulkan.hpp and NVRHI stay out of
 * TbUiLib.
 *
 * Threads (C20): the render thread alone opens, records and executes command lists and
 * collects garbage, and never waits on the GUI thread. The compile thread (D71) only
 * creates shaders, layouts, its format-only framebuffers and pipelines. The CPU pool's
 * jobs only fill CPU memory. The GUI thread creates and destroys swapchains only while
 * the render thread is parked, and runs the teardown steps once it is joined.
 *
 * Snapshots are pulled (§3.3, S31): the render thread posts ReadyForSnapshot when it
 * starts and whenever it has taken the snapshot offered; the GUI thread then offers one
 * (offerSnapshot), and at most one waits in the session's one-slot inbox. B0.3: the
 * render thread takes it at the start of a frame once the chunk jobs of the snapshot
 * before it are applied (lit/SceneGeometry.h), so one snapshot is in flight (taken, its
 * chunks not yet applied) besides the one in the inbox, and the chunks apply in the
 * snapshots' sequence order.
 */
class LitSession
{
private:
  SessionOptions m_options;
  // declared before the parts that post through it
  std::unique_ptr<SessionEvents> m_events;
  std::unique_ptr<GpuDevice> m_device;
  std::unique_ptr<RenderThread> m_renderThread;
  // S3: made at phase B, stopped in teardown step 1
  std::unique_ptr<CpuPool> m_pool;
  // D71: the compile thread, started at phase B, joined in teardown step 1 beside the
  // pool, its handles released in step 2
  std::unique_ptr<PipelineCompiler> m_compiler;
  // the owner's, which outlives the session (LitSessionHost); null after step 1
  MaterialService* m_materials = nullptr;
  // R2.9: the render thread's light registry publishes here; it outlives the thread
  std::shared_ptr<LightTablePublication> m_lightTables;
  // LP4.2a (D87): and its probe registry here, alike
  std::shared_ptr<ProbeTablePublication> m_probeTables;
  // LP4.3 (C36): and its scene's grid tables here, alike
  std::shared_ptr<GridTablePublication> m_gridTables;
  std::uint64_t m_swapchainCount = 0;

  bool m_renderThreadStopped = false;
  bool m_gpuReleased = false;
  bool m_vkDeviceDestroyed = false;
  TeardownReport m_report;
  // the process's one live session slot, released when the destructor has torn down
  bool m_claimedLiveSession = false;

public:
  /**
   * Phase B of Start (§3.1 steps 4-7): picks the physical device that presents to
   * surface, creates the VkDevice, the NVRHI device and the first swapchain generation,
   * and starts the render thread. initInstanceDispatcher must have been called for the
   * instance. A null surface is the headless test path: no presentation, no swapchain;
   * the render thread clears an offscreen texture of the given size instead. Only one
   * session lives in a process at a time, because the Vulkan dispatcher is process-wide:
   * a second create fails while one exists. Step 8 also starts the CPU pool. materials,
   * the owner's material service or null, must outlive the session's teardown step 1,
   * which forgets it.
   */
  static Result<std::unique_ptr<LitSession>> create(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height,
    SessionOptions options,
    EventSink eventSink,
    std::uint64_t generation,
    MaterialService* materials = nullptr);

  // Runs every teardown step that has not run.
  ~LitSession();

  std::uint64_t generation() const;
  // the physical device's name
  const std::string& deviceName() const;
  // C30: the device's limits and vendor; any thread
  DeviceFacts deviceFacts() const;
  // C30, D72: the device memory allocations live now (lit's GpuDevice); any thread
  std::int64_t deviceMemoryAllocations() const;
  // the GPU and its one graphics and present queue family, for the GUI thread's present
  // check after a screen change (R5.33)
  VkPhysicalDevice physicalDevice() const;
  std::uint32_t queueFamily() const;

  // latest-wins camera slot, any thread; a changed camera triggers a frame
  void setCamera(const CameraState& camera);
  // gates every frame trigger (R5.25); the GUI thread mirrors QWindow::isExposed()
  void setExposed(bool exposed);
  // any thread
  void requestFrame();

  /**
   * S31, GUI thread: puts snapshot into the one-slot inbox and triggers a frame, whose
   * start hands it to the render thread. False, and the snapshot is not taken, when the
   * slot still holds the last one, when the snapshot is null or of another session
   * generation (D53), or when the render thread has stopped; the caller keeps its changes
   * and offers again on the next ReadyForSnapshot.
   */
  bool offerSnapshot(std::shared_ptr<const Snapshot> snapshot);
  /**
   * B0.7, GUI thread: the one-slot inbox is empty and the render thread has not been
   * stopped, so that an offer now is taken unless the thread failed meanwhile; an atomic
   * read. The owner asks whenever it has changes, so that one lost ReadyForSnapshot never
   * stops the snapshots of the session.
   */
  bool snapshotSlotFree() const;
  /**
   * R2.2, any thread: the material service published a table (the owner calls it on
   * SessionEvent::MaterialsPublished). The render thread wakes and adopts it at its next
   * pass that draws no frame, so that chunks and lights built while a material, an
   * entityDef or the .proc was pending are built again without a camera move. It sets a
   * flag under the render thread's mutex and notifies.
   */
  void tableChanged();
  /**
   * C15, R2.9, any thread: the latest light records the render thread published (never
   * null; empty before the first snapshot's lights are built), also after teardown step
   * 1, when the render thread has ended.
   */
  std::shared_ptr<const LightTable> lightTable() const;
  /**
   * LP4.2a (C37, D87, R2.9), any thread: the latest probe records the render thread
   * published (never null; empty before the first build), also after teardown step 1.
   */
  std::shared_ptr<const ProbeTable> probeTable() const;
  /**
   * LP4.3 (C36, D88, R2.9), any thread: the latest light grid of the scene's bake the
   * render thread published (never null; empty without a grid), also after teardown step
   * 1.
   */
  std::shared_ptr<const GridTable> gridTable() const;

  /**
   * LPF.2 (D107, C29, P5.24; lib/TbLitLib/FEED.md), any thread but the render thread: a
   * copy of the LDR target (_currentRenderLDR, RGBA8_UNORM after the tonemap and before
   * the blit, at the session's target extent: LdrImage, rows top first, four bytes a
   * pixel, alpha as the tonemap pass writes it) of the first settled frame (D77) recorded
   * after the call. Blocks the caller until that frame has finished on the GPU, never the
   * render thread: the request triggers a frame, and the render thread copies the target
   * inside the frame's command list into one staging texture it keeps per extent (not one
   * per call), waits for the frame and maps the copy. "Settled" includes LPF.1's feed
   * counters (FrameConvergence::feedBehind): a frame is settled only when its table holds
   * every image override and re-parse handed over before its convergence, and the scene
   * scanned it, so a readback after setImageOverride shows the override (or its refusal)
   * without the caller waiting for anything else. A host that keeps feeding never gets a
   * settled frame, by design: it gets the timeout's error. An error also when the session
   * stops first, or when a headless session was never exposed (setExposed: no frame is
   * drawn).
   */
  Result<LdrImage> readbackLdr(std::chrono::milliseconds timeout);

  // S3: the CPU pool from phase B until teardown step 1, else null; GUI thread, tests
  // only (B0.10, R1.11: the render thread gets the pool and the service at its start)
  CpuPool* cpuPoolForTesting() const;
  // the owner's material service, from create until teardown step 1, else null; GUI
  // thread, tests only
  MaterialService* materialServiceForTesting() const;

  // Surface handshake (§3.1, D49), GUI thread only.

  /**
   * Asks the render thread to park and waits up to timeout. True: it is parked, and
   * destroySwapchain and createSwapchain may run. False: it has not parked yet, because
   * it is still inside a present that may need this thread's message loop; the request
   * stays, and SessionEvent::Parked arrives when it parks (check isParked()).
   */
  bool park(std::chrono::milliseconds timeout = ParkTimeout);
  bool isParked() const;
  // Destroys the swapchain and its semaphores; the render thread has released the
  // wrappers. Does nothing unless parked or stopped.
  void destroySwapchain();
  /**
   * Creates the next generation on the window's (new) surface; destroys the current one
   * first if there is one. A 0×0 extent (a minimised window) makes no swapchain: the
   * render thread draws nothing until the next one. An error, for example a GPU that no
   * longer presents to the surface after a screen change (R5.33), means Stop.
   */
  Result<void> createSwapchain(
    VkSurfaceKHR surface, std::uint32_t width, std::uint32_t height);
  bool hasSwapchain() const;
  /**
   * Ends a park. A park that completed released the generation's image wrappers, so a
   * thread that parked is resumed only after createSwapchain; one resumed without a new
   * generation draws nothing and asks for one with SwapchainOutOfDate (review V.1).
   */
  void unpark();
  /**
   * Whether a SwapchainOutOfDate still concerns the current generation: false once the
   * generation it was posted for has been replaced, so that a replacement is not
   * replaced again (review V.2). True without a swapchain.
   */
  bool swapchainReplacementRequested() const;
  /**
   * The window's DPR or screen changed. A generation that the surface reports SUBOPTIMAL
   * is replaced once; a new one of the same size that is SUBOPTIMAL again presents on as
   * it is, until the size changes or this is called.
   */
  void surfaceChanged();

  // Teardown (§3.1 steps), GUI thread; the Qt side deletes the window container between
  // releaseGpuObjects and destroyVkDevice (step 6). Each step runs once and runs the
  // earlier ones first if they have not run.

  // Step 1 comes in parts, so that a GUI thread can keep answering the messages that a
  // present may send to its window while it waits (R5.18): requestStop sets the stop
  // flag, cancels the CPU pool (R2.1: its generation bumped without a join, so its queued
  // and running jobs return at once) and returns; the render thread stops at the next
  // pass boundary, which includes the one before the present, waits for the GPU (also
  // when lost, ignoring the result) and for its jobs, writes the frame log and exits;
  // waitForRenderThread waits up to timeout and returns whether it has exited.
  // stopRenderThread requests the stop and joins without servicing anything, for callers
  // without a window, and after waitForRenderThread returned true; then it stops the CPU
  // pool (its generation bumped again, its threads joined, R5.22) and forgets the
  // material service.
  void requestStop();
  // from here on, warnings, errors and a loss go into the teardown report instead of the
  // event sink; requestStop calls it, and a caller that parks first calls it before that
  void beginStop();
  bool waitForRenderThread(std::chrono::milliseconds timeout);
  void stopRenderThread();
  // steps 2-5: release every handle, waitForIdle, runGarbageCollection (skipped when
  // lost), the swapchain wrappers, vkDestroySwapchainKHR, the semaphores, the NVRHI
  // device (its last Release() count is kept)
  void releaseGpuObjects();
  // step 7, the device part: vkDestroyDevice; the instance belongs to the caller
  void destroyVkDevice();
  // what the teardown found; complete after releaseGpuObjects
  TeardownReport teardownReport() const;

  // Test hooks

  // the render thread raises the fault in its next frame
  void debugInjectFault(DebugFault fault);
  // runs hook on the render thread at point until an empty function replaces it
  void debugSetHook(DebugHook point, std::function<void()> hook);
  // what the NVRHI device's last Release() returned; 0 expected
  std::uint32_t lastDeviceReleaseCount() const;
  std::size_t nvrhiErrorCount() const;
  // frames presented (headless: finished recording and submitted)
  std::uint64_t framesPresented() const;
  /**
   * LP2.2b (D77), any thread: the last frame drawn was settled: no pending pipeline,
   * material, evaluation or image, no provisional chunk (LP5.F: none built at a Default
   * size whose measure is still to come), every texture's levels written, no draw
   * skipped; a settled session draws nothing until a trigger.
   */
  bool lastFrameSettledForTesting() const;
  // LP3.0 (D151), any thread: the still timer's firings so far (none before LP3.3)
  std::uint64_t stillTimerFiringsForTesting() const;
  // the swapchain generations made so far; GUI thread
  std::uint64_t swapchainGenerations() const;
  // the camera the next frame draws with
  CameraState camera() const;
  bool deviceLost() const;
  // the snapshots the render thread took so far, and the last one; any thread
  std::uint64_t snapshotsTakenForTesting() const;
  std::shared_ptr<const Snapshot> lastSnapshotForTesting() const;
  // the batches of chunk or light jobs the render thread has applied; any thread
  std::uint64_t geometryBatchesForTesting() const;
  // LP6.3 (C67 (b)), any thread: the scene's first batch as the render thread last synced
  // it (lit/SceneGeometry.h): its jobs, the view's cells, the cells applied early, whole
  FirstBatchCounts firstBatchForTesting() const;
  // LP7.2 (P7.14), any thread: the scene's models as the render thread last synced and
  // placed them (ModelTotals)
  ModelTotals modelTotalsForTesting() const;
  // the device to make resources with, from create until teardown step 5; tests only
  nvrhi::IDevice* nvrhiDeviceForTesting() const;
  // the compile thread, from create until teardown step 2; tests only (the render thread
  // has it from its start, LP2.1)
  PipelineCompiler* pipelineCompilerForTesting() const;
  // the options the session was created with (C1: the game path among them); tests only
  const SessionOptions& optionsForTesting() const;
  // LP2.1, any thread: a headless session's next frame is drawn at this size (its
  // targets made again, C28), and a frame is triggered
  void resizeHeadlessForTesting(std::uint32_t width, std::uint32_t height);
  // C28, any thread: the frame resources' render-target generation (1 after create)
  std::uint64_t renderTargetGenerationForTesting() const;
  // the device-local heaps' usage now (VK_EXT_memory_budget); any thread, tests only
  std::optional<VramUsage> vramForTesting() const;
  /**
   * LP2.1, any thread: the next frame drawn runs hook once on the render thread, inside
   * its command list after its uploads and before its first pass, with the render
   * thread's resources (RenderFrameAccess, in TbLitLib's RenderThread.h); a frame is
   * triggered. A hook that is never run (the session stops first) is dropped.
   */
  void runInFrameForTesting(std::function<void(RenderFrameAccess&)> hook);
  // LP2.1, any thread: every frame runs hook as runInFrameForTesting does, until an empty
  // function replaces it
  void setEveryFrameHookForTesting(std::function<void(RenderFrameAccess&)> hook);
  /**
   * LP2.2a, any thread: the next frame drawn runs hook once on the render thread after
   * its lit passes (the depth prepass and the interactions), inside its command list,
   * for a readback of the HDR target; a frame is triggered. A hook that is never run is
   * dropped.
   */
  void runAfterPassesForTesting(std::function<void(RenderFrameAccess&)> hook);
  /**
   * LP2.2b, any thread: the next frame drawn runs hook once on the render thread after
   * its tonemap passes and its blit, inside its command list, for a readback of the LDR
   * target or the frame's target; a frame is triggered. A hook that is never run is
   * dropped.
   */
  void runAfterTonemapForTesting(std::function<void(RenderFrameAccess&)> hook);
  /**
   * LP2.2b, any thread: while colour is set, every frame clears the HDR target to it
   * (RGBA16F, so each channel is stored as a half) instead of recording its lit passes,
   * clears the LDR and depth targets as those passes do, and runs the tonemap passes and
   * the blit on it; nullopt draws the scene again. A frame is triggered.
   */
  void setHdrClearForTesting(std::optional<std::array<float, 4>> colour);
  /**
   * LP2.3 (Q21's pinned-exposure diagnostic, C29), any thread: the tonemap passes use
   * these values instead of the engine's defaults (EngineTonemap, lit/EngineTables.h)
   * from the next frame on, as parity.py --pinned sets r_hdrMinLuminance and
   * r_hdrMaxLuminance in the engine; a frame is triggered.
   */
  void setTonemapForTesting(const EngineTonemapDefaults& tonemap);
  // LP2.1, any thread: the render thread's arenas hold these chunks instead of the
  // scene's until null replaces them (D36); a frame is triggered
  void setChunksForTesting(std::shared_ptr<const std::map<ChunkKey, Chunk>> chunks);
  /**
   * LP6.4a step 0b, any thread: the scene's chunk jobs of the batches started from the
   * next snapshot the render thread takes on run hook first, on the pool, with the job's
   * cells (lit/SceneGeometry.h, setBeforeJobForTesting): a test holds the first batch's
   * entity job there. The hook must let every job go before the session stops.
   */
  void setBeforeChunkJobForTesting(std::function<void(const std::vector<CellRef>&)> hook);
  // LP2.1, any thread: what a frame uploads at most (§3.10's 32 MB cap)
  void setUploadCapForTesting(std::uint64_t bytes);
  // LP2.1, any thread: while held, no frame counts as finished on the GPU, so the ranges
  // freed in the arenas and the retired targets stay held (D35, D36)
  void holdFrameCompletionForTesting(bool hold);
  // U2.3, any thread: the allocation limit the headroom (AllocationHeadroom) counts from
  // instead of the device's maxMemoryAllocationCount; 0 for the device's
  void setAllocationLimitForTesting(std::uint32_t limit);
  // U2.6, LP6.4b (C68 (b)), any thread: the session's eviction budget
  // (SessionOptions::vramBudgetBytes) from the next frame on; 0 for the options' again
  void setVramBudgetForTesting(std::uint64_t bytes);
  // LP6.4b, any thread: hook runs on the render thread after every frame's budget pass
  // (BudgetPassReport) until an empty function replaces it; no frame is triggered
  void setBudgetPassHookForTesting(std::function<void(const BudgetPassReport&)> hook);
  // LP6.4b (C68 (d)), any thread: while held, no settled frame marks an arena
  // compaction; tests only
  void holdCompactionForTesting(bool hold);
  /**
   * C38 (LP4.2b's ambient-only parity rows, mirror.json M-TEST-AMB-01), any thread: while
   * on, the frames do not record the interactions pass, as the engine's
   * r_skipInteractions 1 skips DrawInteractions (RenderBackend.cpp:4181); the clears, the
   * prepass, the ambient pass and the tonemap passes run as they do. A frame is
   * triggered. Tests only.
   */
  void skipInteractionsForTesting(bool skip);
  /**
   * LP5.0 (D118, C50), any thread: r_skipAmbient's stand-in for the parity rows
   * (rows.json's skip_ambient, set by the [.][parity] test beside
   * skipInteractionsForTesting). Stored and handed to the frames, which do not read it
   * yet: LP5.2a's stage pass (pass 8) will not be recorded while it is on, as
   * r_skipAmbient 1 skips both DrawShaderPasses calls (RenderBackend.cpp:4275) and
   * nothing else (the ambient pass is not pass 8). Tests only.
   */
  void skipShaderPassesForTesting(bool skip);
  // LP5.0, any thread: the switch as skipShaderPassesForTesting last set it
  bool shaderPassesSkippedForTesting() const;
  // LP3.4 (D158), any thread: the debug view the frames draw from the next one on (a
  // change triggers a frame), and the one set last
  void setDebugView(DebugView view);
  DebugView debugView() const;
  /**
   * U2.3, any thread: while set, every device memory allocation in a type that is
   * host-visible and not device-local fails as a driver's out-of-memory would (NVRHI's
   * upload chunks and staging). Process-wide, like the allocation counter; tests only.
   */
  static void failHostAllocationsForTesting(bool fail);
  /**
   * D9, any thread: every program the compile thread was asked for, enabled or disabled
   * with the field that disabled it; the session's report of its passes.
   */
  std::vector<ProgramReport> programReport() const;
  // LP3.3 (D146, P3.4a), any thread: whether the device copies D32 images (the shadow
  // atlas's relocated tiles are copied; else drawn again, and programReport says so)
  bool shadowDepthCopyForTesting() const;
  /**
   * LP2.3 (D9), any thread: the last frame shown, for the panel's status line
   * (sessionStatusLine); a default status before the first frame and after teardown
   * step 1.
   */
  SessionStatus status() const;
  // LP6.4a (P8.3), any thread: the Start probe's line (the classes, their alignments, the
  // classes kept dedicated), empty before the first frame; measurements only
  std::string formatClassLineForTesting() const;
  // LP2.4 (Q14), any thread: the compile thread's timings so far; empty once teardown
  // step 2 released the compiler
  CompileTimings compileTimingsForTesting() const;
  /**
   * LP2.4, any thread: from the next snapshot that carries the document's path on, the
   * scene asks for the bake of this game-relative map path instead
   * ("maps/game/admin.map"), as for a document saved at <game>/base/<path>; null ends it.
   * The caller keeps the string alive until it sets null. Process-wide, like the
   * dispatcher; tests and measurements only, for a map loaded from a copy outside the
   * game folder.
   */
  static void setMapPathOverrideForTesting(const char* gameRelativeMapPath);
  /**
   * LP4.2b step 0, any thread: while held, the scene's probe builds wait (their inputs
   * kept), so that the frames meanwhile are not settled (FrameConvergence::
   * probesInFlight); released, the waiting build starts at the render thread's next
   * geometry poll. Process-wide (setProbeBuildsHeldForTesting); tests only.
   */
  static void holdProbeBuildsForTesting(bool hold);
  /**
   * LP6.4a (C68 (a), P8.3), any thread: while refused, the slots' textures of this class
   * (an engine format's texture format and its dimension: Texture2D, TextureCube,
   * Texture2DArray) are made dedicated as before the heaps, from the next texture made
   * on, and the Start probe's line names the class as refused by a test. Process-wide
   * (TextureHeaps.h); tests only: a test refuses before a Start or before its allocation
   * limit and clears it when it ends.
   */
  static void setFormatClassRefusedForTesting(
    nvrhi::Format format, nvrhi::TextureDimension dimension, bool refused = true);
  /**
   * LP6.4a (C68 (a)), any thread: the texture heaps' size of the sessions created from
   * now on (0: HeapBytes, 128 MB, Q47), for a test of a texture larger than a heap.
   * Process-wide; tests only.
   */
  static void setTextureHeapBytesForTesting(std::uint64_t bytes);
  // LP6.4b, any thread (tests): the geometry arenas' size of the next sessions (0:
  // ArenaBytes); process-wide like the texture heaps' size
  static void setGeometryArenaBytesForTesting(std::uint64_t bytes);
  /**
   * LP2.4 (C30, D72), any thread: the device memory allocations the counter of the last
   * session counts now; after a Stop, what that session's teardown left (0 expected).
   * Process-wide; tests and measurements only.
   */
  static std::int64_t liveDeviceMemoryAllocationsForTesting();
  /**
   * LP5.3 step 0, any thread: the library's threads started and not yet joined (every
   * StackThread and the render thread), process-wide; 0 after a session's teardown and
   * its service's join. Tests only.
   */
  static std::int64_t liveThreadsForTesting();
  /**
   * LP2.2b (C29, P2.25), any thread but the render thread: readbackLdr (LPF.2), nullopt
   * for its error. Size the session first (create, resizeHeadlessForTesting): the target
   * has the frame's extent.
   */
  std::optional<LdrImage> readbackLdrForTesting(
    std::chrono::milliseconds timeout = std::chrono::milliseconds{60000});
  // LPF.2 (P5.24), any thread: the readbacks' staging textures made so far
  std::uint64_t ldrStagingCreationsForTesting() const;

private:
  LitSession(
    SessionOptions options,
    EventSink eventSink,
    std::uint64_t generation,
    MaterialService* materials);

  Result<void> start(
    const InstanceHandles& instance,
    VkSurfaceKHR surface,
    std::uint32_t width,
    std::uint32_t height);

  deleteCopyAndMove(LitSession);
};

} // namespace tb::lit
