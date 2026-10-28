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

#include "BindingSetCache.h"
#include "ImageSlots.h"
#include "InlineVector.h"
#include "LitFrontend.h"
#include "LitStages.h"
#include "PipelineCompiler.h"
#include "base/Macros.h"
#include "lit/EngineBuiltins.h"
#include "lit/GridTable.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::lit
{

// LP6.1 (C64): the most slot versions a record is resolved with: its set 1's three units
// and an interaction's set 2's three (each with the intrinsic standing in), or an
// ambient draw's set 1 and its grid area's atlas
inline constexpr std::size_t DrawRecordSlots = 12;

/**
 * LP6.1 (C64, P8.1): one draw's resolution, what DrawElementsWithCounters resolved per
 * draw before (LitPasses::drawGeometry): the pipeline (its key, the pipeline, the layout
 * type, whether set 0 pushes its parms, set 0's volatile constant buffer), the binding
 * sets with their cache entries (held, so that trim never drops them, and stamped once
 * per frame through the entry, no hash), the placement copied once (the vertex and index
 * buffers, the ranges, shared for an instance's), the slot versions it was resolved with
 * (every set's, and an ambient draw's grid atlas: the index the table invalidates it by),
 * and the render-target and arena generations. Unresolved: made, or released (a slot it
 * names changed, a generation moved, the table cleared): resolved again at its next draw.
 * Render thread.
 */
struct DrawRecord
{
  enum class State : std::uint8_t
  {
    Unresolved,
    Resolved,
  };

  State state = State::Unresolved;
  PipelineKey key;
  nvrhi::IGraphicsPipeline* pipeline = nullptr;
  EngineBindingLayout layout = EngineBindingLayout::Num;
  bool pushConstants = false;
  nvrhi::IBuffer* constantBuffer = nullptr;
  std::size_t setCount = 0;
  std::array<nvrhi::IBindingSet*, nvrhi::c_MaxBindingLayouts> sets = {};
  std::array<BindingSetCache::Entry*, nvrhi::c_MaxBindingLayouts> entries = {};
  nvrhi::IBuffer* vertexBuffer = nullptr;
  nvrhi::IBuffer* indexBuffer = nullptr;
  std::uint32_t indexCount = 0;
  std::uint32_t firstIndex = 0;
  std::uint32_t baseVertex = 0;
  bool shared = false;
  InlineVector<SlotVersion, DrawRecordSlots> slots;
  std::uint64_t renderTargetGeneration = 0;
  std::uint64_t arenaGeneration = 0;
  // the frame whose draws last stamped its entries' lastUsed
  std::uint64_t stamped = 0;
};

/**
 * LP6.1 (C64): one DrawSingleInteraction draw of a surface as the stage loop made it
 * (engineSurfaceInteractions, engineAmbientSurfaceInteractions,
 * pendingSurfaceInteraction): its parms, its three images (bump, specular, diffuse: units
 * 0-2; their slots are asked for by its record's resolution, as the binding asked for
 * them before) and, for pass 7, the carried fields it read from the state the surfaces
 * before it left (InteractionDraw::inherited). Its program follows the specular slot's
 * usage, chosen at its record's resolution.
 */
struct RecordedInteraction
{
  std::array<MaterialImageKey, 3> images = {};
  std::array<Vec4, 2> bumpMatrix = {};
  std::array<Vec4, 2> diffuseMatrix = {};
  std::array<Vec4, 2> specularMatrix = {};
  Vec4 diffuseColor = {0.0f, 0.0f, 0.0f, 0.0f};
  Vec4 specularColor = {0.0f, 0.0f, 0.0f, 0.0f};
  int vertexColor = engine_material::VertexColorIgnore;
  std::uint8_t inherited = 0;

  static RecordedInteraction of(const InteractionDraw& draw);
};

// LP6.1 (C64): an ambient draw (pass 5): its interaction, its record, and on the grid
// path the area's parms by its atlas texture's size (resolved with the record)
struct AmbientRecord
{
  RecordedInteraction draw;
  DrawRecord record;
  GridParms grid;
};

// LP6.1 (C64): an interaction draw (pass 7): its interaction and its records, the plain
// program's (0) and the shadow-atlas one's (1, a light pass 6 placed): the shadowed flag
// of the record's key
struct InteractionRecord
{
  RecordedInteraction draw;
  std::array<DrawRecord, 2> variants;
};

/**
 * LP6.1 (C64): a surface's identity across lists builds, for the depth prepass's and the
 * ambient pass's records: its chunk (the lists' Chunk, stable while the chunks' version
 * holds), its model instance, its material entry and registers objects (the records hold
 * them, so that no other one takes their address), whether they are provisional, and its
 * grid area. Never a space (a build's) nor a camera field.
 */
struct SurfaceRecordKey
{
  const Chunk* chunk = nullptr;
  const ModelInstance* instance = nullptr;
  const MaterialEntry* material = nullptr;
  const EvaluatedRegisters* registers = nullptr;
  bool provisional = false;
  std::optional<int> gridArea;

  bool operator==(const SurfaceRecordKey&) const = default;
  static SurfaceRecordKey of(const LitSurface& surface);
};

struct SurfaceRecordKeyHash
{
  std::size_t operator()(const SurfaceRecordKey& key) const;
};

// LP6.1 (D167): what FillDepthBufferFast does with a surface after the leading subview
// ones (RenderBackend.cpp:934-1041): a translucent material's puts nothing in the depth
// buffer, a perforated one's goes to the generic path after the opaque ones, any other
// (and one whose material is pending) is drawn by builtin/depth
enum class DepthClass : std::uint8_t
{
  Opaque,
  Perforated,
  Translucent,
};

/**
 * LP6.1 (C64, D167): a surface's records, the one walk of passes 1 and 5 (and LP6.5a's
 * pass 3): decided once when the group is made, from its material and registers: its
 * depth class and whether it is a subview surface (SS_SUBVIEW: the prepass's leading run,
 * through the generic path), whether the ambient pass draws it (material and registers
 * known, not translucent, some stage's condition on: AmbientPass :2430-2454,
 * M-AMB-DIFF-02); its depth prepass record (an opaque surface's), and its ambient records
 * in engineAmbientSurfaceInteractions' order (made at the ambient pass's first draw of
 * the surface, which asks for its images then; never resized after: the records'
 * addresses hold), with the material and registers it was made of.
 */
struct SurfaceRecords
{
  std::shared_ptr<const MaterialEntry> material;
  std::shared_ptr<const EvaluatedRegisters> registers;
  DepthClass depth = DepthClass::Opaque;
  bool subview = false;
  bool ambientDrawn = false;
  DrawRecord prepass;
  std::vector<AmbientRecord> ambient;
  bool ambientFilled = false;
  // the last build that linked it
  std::uint64_t lastLinked = 0;
};

/**
 * LP6.1 (C64): an interaction draw group's identity across lists builds: the light (its
 * entry's serial: its record, shader and registers), the light stage, the list, and the
 * surface's identity as SurfaceRecordKey's less the grid area.
 */
struct InteractionRecordKey
{
  std::uint64_t light = 0;
  std::size_t stage = 0;
  InteractionList list = InteractionList::Global;
  const Chunk* chunk = nullptr;
  const ModelInstance* instance = nullptr;
  const MaterialEntry* material = nullptr;
  const EvaluatedRegisters* registers = nullptr;
  bool provisional = false;

  bool operator==(const InteractionRecordKey&) const = default;
};

struct InteractionRecordKeyHash
{
  std::size_t operator()(const InteractionRecordKey& key) const;
};

/**
 * LP6.1 (C64): RenderInteractions' drawInteraction_t fields that carry from surface to
 * surface and from light stage to light stage within one call (inter = {} at its start:
 * zero matrices, SVC_IGNORE): a surface's stage loop sets some (InteractionState::
 * written), its draws read the others from the surfaces before it.
 */
struct InteractionCarry
{
  std::array<Vec4, 2> bumpMatrix = {};
  std::array<Vec4, 2> diffuseMatrix = {};
  std::array<Vec4, 2> specularMatrix = {};
  int vertexColor = engine_material::VertexColorIgnore;
};

/**
 * LP6.1 (C64): a (light stage, list, surface)'s interaction draws, in the stage loop's
 * order (made with the group, never resized after), and what the surface's stage loop
 * leaves of the carried fields: their bits and values (none for a pending surface, whose
 * draw has a state of its own).
 */
struct InteractionRecords
{
  std::shared_ptr<const MaterialEntry> material;
  std::shared_ptr<const EvaluatedRegisters> registers;
  std::vector<InteractionRecord> draws;
  std::uint8_t written = 0;
  InteractionCarry carry;
  // made at the group's first draw (the stage loop then)
  bool filled = false;
  std::uint64_t lastLinked = 0;
};

/**
 * LP6.1 (C64, P8.1, P8.11, D172): the draw records of passes 1, 5 and 7, kept across
 * lists builds (the lists are built again on every camera move: a record made for one
 * build would save nothing). The rules:
 *
 * - Made when a build first links a surface (SurfaceRecords) or a (light stage, list,
 *   surface) of a light's lists (InteractionRecords), one hash lookup per link; later
 *   builds link to it. A record is resolved at its first draw and drawn from then on
 *   without a lookup.
 * - Cleared whole when a lists build's key moves in a scene field (the chunks and their
 *   version, the instances, the entities, the tree and its AreaData, the portal fog
 *   table, the material table's registry or revision); the light table's fields clear
 *   the interaction records only (with the lights' entries), the grid's fields (the grid
 *   table, its version, its validity) the surface records only. A build whose key moves
 * in the camera alone (the camera, the extent) clears nothing, nor do the light
 * triangles' answers, their cull switch or the shadows' switch, which no record depends
 * on.
 * - Cleared whole when the render-target generation moves (before the binding-set cache
 *   erases the old targets' sets) or the arena generation moves (a placement freed:
 *   GeometryArenas::generation), and by releaseAll (teardown step 2) and the volatile
 *   buffers' new size (a test's): the passes link the lists again after a clear outside a
 *   build.
 * - Released (unresolved, its sets let go, out of the index): every record a changed slot
 *   names (slotChanged: a slot's version bump, before the cache erases the slot's sets;
 * an adopted usage's change, which can change the program). A record whose pipeline is
 *   pending is never resolved: it asks the pipeline again at each draw until it is made.
 * - Swept: every 64th build, a group no build linked in the last 1024 builds goes (its
 *   records released); past RecordCapacity draw records, the least recently linked groups
 *   go first, never one the current build linked. So the table holds at most the groups
 *   the last 1024 builds linked, and never more than RecordCapacity records but the
 *   current build's own.
 * - The stamp (D172's hook): a record's draw sets lastUsed of each set it holds to the
 *   frame once per frame, through the entry it holds (no hash); a held set is never
 *   trimmed; every release lets go of its holds before any erase (the cache counts an
 *   erased held set: heldErased, 0).
 *
 * Render thread; the CPU test makes records with fake entries.
 */
class DrawRecordTable
{
public:
  // the lists' key fields the records depend on (LitPasses::ListsKey less the camera's)
  struct Lifetime
  {
    // the scene: a change clears every record
    const void* chunks = nullptr;
    std::uint64_t chunksVersion = 0;
    const void* instances = nullptr;
    const void* entities = nullptr;
    const void* tree = nullptr;
    const void* areas = nullptr;
    const void* portalFog = nullptr;
    ListsTableKey table;
    // the lights: a change clears the interaction records
    const void* lights = nullptr;
    std::uint64_t lightsVersion = 0;
    // the grid: a change clears the surface records
    const void* grid = nullptr;
    std::uint64_t gridVersion = 0;
    std::uint64_t gridValidity = 0;

    bool operator==(const Lifetime&) const = default;
  };

  // what a change cleared
  struct Cleared
  {
    bool surfaces = false;
    bool interactions = false;
  };

  static constexpr std::uint64_t SweepEvery = 64;
  static constexpr std::uint64_t SweepAge = 1024;
  static constexpr std::size_t RecordCapacity = 65536;

private:
  struct LightEntry
  {
    std::shared_ptr<const LightRecord> record;
    std::shared_ptr<const MaterialEntry> shader;
    std::vector<float> registers;
    std::uint64_t serial = 0;
    std::uint64_t lastLinked = 0;
  };

  std::optional<Lifetime> m_lifetime;
  std::uint64_t m_build = 0;
  std::uint64_t m_renderTargetGeneration = 0;
  std::uint64_t m_arenaGeneration = 0;
  bool m_generationsSeen = false;
  std::
    unordered_map<SurfaceRecordKey, std::unique_ptr<SurfaceRecords>, SurfaceRecordKeyHash>
      m_surfaces;
  std::unordered_map<
    InteractionRecordKey,
    std::unique_ptr<InteractionRecords>,
    InteractionRecordKeyHash>
    m_interactions;
  std::unordered_map<const LightRecord*, LightEntry> m_lights;
  std::uint64_t m_lightSerial = 0;
  // the resolved records under each slot they were resolved with
  std::unordered_map<ImageSlotId, std::unordered_set<DrawRecord*>> m_bySlot;
  std::size_t m_records = 0;
  std::size_t m_capacity = RecordCapacity;

public:
  DrawRecordTable() = default;

  // a lists build begins with this lifetime: what its change clears; the build count
  // moves
  Cleared beginBuild(const Lifetime& lifetime);
  // the build's links are made: the sweep
  void endBuild();
  // the frame's generations: a change of either clears every record; whether it did
  bool generations(std::uint64_t renderTargets, std::uint64_t arenas);

  // a link: the surface's records, made on its first link; the build links it
  SurfaceRecords& surface(const LitSurface& surface, bool& made);
  InteractionRecords& interaction(
    const InteractionRecordKey& key, const LitSurface& surface, bool& made);
  // the light entry's serial: a new one when its record, shader or registers changed
  std::uint64_t lightSerial(const ViewLight& light);
  // a group's records made (a surface's ambient records, a (light stage, list, surface)'s
  // interaction records): they count toward the held records
  void ambientFilled(SurfaceRecords& group);
  void filled(InteractionRecords& group);

  // a record resolved: indexed under its slots
  void resolved(DrawRecord& record);
  // a record released: its holds let go, out of the index, unresolved
  void release(DrawRecord& record);
  // every record resolved with slot released, before the cache erases its sets; how many
  std::size_t slotChanged(ImageSlotId slot);
  // a record's draw in frame: its sets' lastUsed, once per frame
  static void stamp(DrawRecord& record, std::uint64_t frame);

  // every record and light entry released and gone
  void clear();
  void clearSurfaces();
  void clearInteractions();

  // the draw records held: the surfaces' (a prepass record each, their ambient records)
  // and the interaction records
  std::size_t records() const;
  std::size_t groups() const;
  // the resolved records under a slot (tests)
  std::size_t recordsOf(ImageSlotId slot) const;
  std::uint64_t build() const;
  std::uint64_t renderTargetGeneration() const;
  std::uint64_t arenaGeneration() const;
  // tests: the sweep's capacity (RecordCapacity)
  void setCapacityForTesting(std::size_t capacity);

private:
  void releaseGroup(SurfaceRecords& group);
  void releaseGroup(InteractionRecords& group);
  void sweep();

  deleteCopyAndMove(DrawRecordTable);
};

} // namespace tb::lit
