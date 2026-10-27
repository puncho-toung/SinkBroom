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

#include "ViewFlow.h"
#include "lit/ChunkBuilder.h"
#include "lit/EngineLight.h"
#include "lit/EngineTables.h"
#include "lit/LightRecord.h"
#include "lit/MaterialDecls.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tb::lit
{

// LP3.1 (C59): an atlas rect, its top-left pixel and its side
struct AtlasRect
{
  int x = 0;
  int y = 0;
  int size = 0;

  bool operator==(const AtlasRect&) const = default;
};

// LP3.1 (C58, C59): the shadow map type of a light (vLight->parallel, then pointLight,
// else a spot light: RenderBackend.cpp:3676-3700)
enum class ShadowLightType
{
  Point,
  Spot,
  Parallel,
};

/**
 * LP3.3 (C61, D159): the shadow cvars a light's tiles are drawn under, by value:
 * EngineFrame's nineteen of LP3.0's block (r_skipShadows to r_useShadowSurfaceScissor;
 * r_useMaskedOcclusionCulling is the frontend's, K51). The preview has no console, so
 * they are the process's constants; the record keeps them so that an override (a later
 * package's) redraws every tile.
 */
struct ShadowCvars
{
  bool skipShadows = false;
  int useShadowAtlas = 0;
  int shadowMapAtlasSize = 0;
  float shadowMapFrustumFov = 0.0f;
  int shadowMapSingleSide = 0;
  float shadowMapJitterScale = 0.0f;
  bool shadowMapRandomizeJitter = false;
  int shadowMapSamples = 0;
  int shadowMapSplits = 0;
  float shadowMapSplitWeight = 0.0f;
  float shadowMapLodScale = 0.0f;
  int shadowMapLodBias = 0;
  float shadowMapPolygonFactor = 0.0f;
  float vkShadowMapPolygonOffset = 0.0f;
  int shadowMapOccluderFacing = 0;
  float shadowMapRegularDepthBiasScale = 0.0f;
  float shadowMapSunDepthBiasScale = 0.0f;
  bool forceShadowMapsOnAlphaTestedSurfaces = false;
  bool useShadowSurfaceScissor = false;

  bool operator==(const ShadowCvars&) const = default;
};

// the nineteen of a frame's defaults. Pure
ShadowCvars shadowCvarsOf(const EngineFrameDefaults& frame);

/**
 * LP3.3 (C61, D150, P3.6, P3.27): what a light's tiles were drawn against, one per light
 * and shared by its sides. A tile is redrawn when its light's key differs from the one
 * it was drawn against (or its side's matrices differ, LP3.3 step 2).
 *
 * - lightVersion: shadowLightVersion, the light's own (origin, axis, volume, type,
 *   noshadows, the casting shader's flags) and its area references (derived.areas, empty
 *   or not, and areaNum: a light crossing the hull boundary or an area portal changes its
 *   caster set);
 * - lod: the LOD its tiles are drawn at (the held one while the camera moves, D152);
 * - casterHash and casterCount: the sum of shadowCasterTerm over the light's casters
 *   (ViewLight::casters), accumulated in the frontend's gathering after the rule admitted
 *   each (lightReachesCaster, the light volume's cull, the spectrum, the light
 *   triangles): order-independent, so the set the tiles were drawn from;
 * - perforated: LP3.4 (P3.6, C61, D156): the sum of shadowPerforatedTerm over the light's
 *   Perforated casters (their stage draws, so the registers that reach them, and their
 *   material entries' revision: the frontend's gathering, ViewLight::perforatedHash) and
 *   of shadowPerforatedImageTerm over their alpha-tested stages' image slots (pass 6's,
 *   each frame); an opaque caster's appearance is in neither (S36);
 * - cvars: the shadow cvars by value;
 * - generation: the bake the chunk set was built under (LitPasses' count of the scene's
 *   tree and AreaData pointers, tree presence included: D94's no-tree rule makes every
 *   chunk cast). Layers and filters reach the record through the caster set: a hidden
 *   entity builds no chunk (ESH:119-129) and has no space, so it leaves the hash; a
 *   hidden light leaves the layout.
 */
struct ShadowRecordKey
{
  std::uint64_t lightVersion = 0;
  int lod = -1;
  std::uint64_t casterHash = 0;
  std::size_t casterCount = 0;
  std::uint64_t perforated = 0;
  ShadowCvars cvars;
  std::uint64_t generation = 0;

  bool operator==(const ShadowRecordKey&) const = default;
};

/**
 * LP3.3 (C61, D150): a light's version as its tiles see it: the projection
 * (baseLightProject, its inverse and globalLightOrigin: the origin, axis and volume), the
 * parms' origin, axis, lightCenter and radius, its type (pointLight, parallel), its
 * noShadows as UpdateLightDef leaves it (engineLightNoShadows: a spectrum shader's
 * forced), the casting shader's flags (engineLightCastsShadows, its spectrum), whether it
 * has a volume, and its area references (areaNum, hasAreaTree and the derived.areas
 * list); never its colour, shaderParms or texture, which no tile reads. Pure.
 */
std::uint64_t shadowLightVersion(const LightRecord& light);

/**
 * LP3.3 (C61, D150, SC739): one caster's term of a light's caster hash: its id (the
 * chunk's or the model instance's key, and which of the two), its caster version (the
 * cast class, the noshadows flags, the entity-local positions and, perforated, st and the
 * material: ChunkBuilder's and SceneModels', unchanged), its space's model matrix (the
 * entity transform: origin, axis, modelscale, which the caster versions do not cover),
 * its cast class (C57) and its AreaSurface (its kind, its area, whether its references
 * are empty: the membership C57 names), mixed to 64 bits so that the sum over a set is
 * order-independent. Pure.
 */
std::uint64_t shadowCasterTerm(
  const ChunkKey& key,
  bool instance,
  std::uint64_t casterVersion,
  const EngineGlMatrix& modelMatrix,
  CastClass castClass,
  const AreaSurface& surface);

/**
 * LP3.4 (C61, P3.6, D155): one Perforated caster's term of its light's key.perforated:
 * its id (as shadowCasterTerm's), its material entry's revision and the draws
 * enginePerforatedShadowDraws makes of it under the call's opening state, which are what
 * its evaluated registers decide (the condition and the colour alpha by which its stages
 * draw, ALPHA_TEST, the texture matrix, the texgen; each draw's program, state and
 * offsets); its material or registers pending (null): the id and a pending mark. Mixed to
 * 64 bits so that the sum over a light's perforated casters is order-independent. Pure.
 */
std::uint64_t shadowPerforatedTerm(
  const ChunkKey& key,
  bool instance,
  const MaterialEntry* material,
  const EvaluatedRegisters* registers);

/**
 * LP3.4 (C61, D156, K53): an alpha-tested stage image's term of its caster's light's
 * key.perforated: its image slot (an ImageSlotId), whether the slot is complete
 * (Resident, Replacing with its complete texture, or a final Default: D88) and its
 * version while it is (an image swapped by a reload, LPF.1's Replacing, redraws the
 * tiles; a slot still loading moves nothing here: its tiles are provisional and drawn
 * again instead). Pure.
 */
std::uint64_t shadowPerforatedImageTerm(
  std::uint64_t slot, bool complete, std::uint64_t version);

// the pixels a and b share
std::int64_t atlasOverlap(const AtlasRect& a, const AtlasRect& b);

/**
 * LP3.3 (D146, D152; C62's "first fit"): the free space of a square map of tiles, the
 * shape of the copied TileMap (TM:45-117): a quad tree of nodes down to minSize, a node's
 * children in TileMap's order (BuildTree's offsets ( -s, s ), ( -s, -s ), ( s, -s ), ( s,
 * s ): as top-left pixels ( x, y + c ), ( x, y ), ( x + c, y ), ( x + c, y + c ) for
 * children of side c), so that a node's rect is the one ShadowAtlasPass computes for it
 * (iPos = ( pos · 0.5 + 0.5 ) · size - side / 2, SC719). Unlike TileMap, which can only
 * be cleared and filled largest first, a tile can be freed and any size asked for:
 * allocate takes the first free node of the size's level (13 - ceil( log2( size ) ) on
 * 8192) depth first in child order, free meaning neither taken, nor under a taken node,
 * nor over one, so that over an empty map, asked in engineSortAtlasSizes' order (largest
 * first), it gives the engine's packing (the test says so). reserve takes a given node (a
 * placement of the engine's packing), release gives one back. Pure; its owner's thread.
 */
class ShadowTileAllocator
{
private:
  struct Node
  {
    AtlasRect rect;
    int parent = -1;
    std::array<int, 4> children = {-1, -1, -1, -1};
    bool taken = false;
    // the taken nodes below it
    std::size_t takenBelow = 0;
  };

  std::vector<Node> m_nodes;
  int m_minSize = 0;
  std::size_t m_taken = 0;

public:
  ShadowTileAllocator(int mapSize, int minSize);

  // a free rect of size (a power of two from minSize to half the map; another size takes
  // the next level's, as GetTile does), or none
  std::optional<AtlasRect> allocate(int size);
  // takes rect's node when it is one and free
  bool reserve(const AtlasRect& rect);
  // gives rect's node back when it is taken
  void release(const AtlasRect& rect);
  void clear();
  // the tiles taken
  std::size_t taken() const;

private:
  int build(const AtlasRect& rect, int parent);
  int nodeOf(const AtlasRect& rect) const;
  void take(int node);
};

/**
 * LP3.3 (D147, P3.3): the parts of rect that none of covering covers, as squares of the
 * quad tree (rect split in four until a part is uncovered or wholly covered, down to
 * minSize): the vacated clears' rects, which leave every tile of the frame untouched.
 * Rects are the atlas's nodes (aligned powers of two). Pure.
 */
std::vector<AtlasRect> uncoveredRects(
  const AtlasRect& rect, std::span<const AtlasRect> covering, int minSize);

// LP3.3 (D146, C61, P3.4): a tile moved from one rect to another of its size
struct ShadowMove
{
  AtlasRect from;
  AtlasRect to;

  bool operator==(const ShadowMove&) const = default;
};

// one copy of a relocation: save (the atlas rect into the scratch rect) or restore (the
// scratch rect into the atlas rect)
struct ShadowCopy
{
  bool save = true;
  AtlasRect atlas;
  AtlasRect scratch;
  // the move it serves
  std::size_t move = 0;

  bool operator==(const ShadowCopy&) const = default;
};

struct ShadowRelocationPlan
{
  // in execution order; the copies of one kind in a row are a phase (NVRHI's barriers
  // fall between the phases: the atlas and the scratch change state there)
  std::vector<ShadowCopy> copies;
  // the moves the scratch cannot schedule: their tiles are drawn again instead
  std::vector<std::size_t> redraw;
};

/**
 * LP3.3 (D146, C61, P3.4b): the order of the copies that relocate moves through a
 * scratch of scratchSize² (2048²: sixteen 512 tiles, four 1024): every source rect is
 * copied to the scratch before any destination is written (all of them first when they
 * fit: one save phase, one restore phase); when they do not, in batches, each saving its
 * moves' sources and then writing their destinations, a move joining a batch only when
 * every unsaved source its destination overlaps belongs to the batch too (so a batch
 * never overwrites a source that has not been saved, and chains are moved from their free
 * end), and a cycle, where no move can join, through one saved slot (one move's source
 * saved and parked until its destination is free). A move the scratch cannot hold then
 * goes to redraw. Pure.
 */
ShadowRelocationPlan planShadowRelocation(
  std::span<const ShadowMove> moves, int scratchSize, int minSize);

// LP3.3 (C61): one side's tile of a light's record
struct ShadowTileRecord
{
  // the side has a rect in the layout
  bool placed = false;
  AtlasRect rect;
  // the rect holds the side's depth drawn against the record's key, every draw made and
  // no caster's light triangles pending (else it is drawn again)
  bool valid = false;
  // SetupShadowMapMatrices' stores as the tile was drawn (C61: the consumer samples with
  // them, never the current camera's, P3.7)
  EngineMat4 shadowV = {};
  EngineMat4 shadowP = {};
  // the casters' depth draws it was drawn with, and the frame that drew it
  std::size_t draws = 0;
  std::uint64_t drawnFrame = 0;
  // LP3.4 (D158): the frame that last relocated it (0: none), the tile-age view's
  std::uint64_t relocatedFrame = 0;
  // LP3.4 (D156, K53): drawn with a perforated caster's stage image not complete, or its
  // material or registers pending: provisional (not valid either, so drawn again; this
  // flag tells it from LP3.3's other reasons: a skipped draw or clear, a budget drop, a
  // caster's light triangles pending)
  bool provisional = false;
};

/**
 * LP3.3 (C61, D150, P3.27): the record of a light whose tiles are in the layout, kept
 * across frames: its type, sides, key and fade, and each side's tile.
 */
struct ShadowRecord
{
  PreviewId light = NoPreviewId;
  ShadowLightType type = ShadowLightType::Point;
  int sides = 0;
  ShadowRecordKey key;
  float fadeOut = 0.0f;
  // D152: the frames in a row whose exact LOD differed from key.lod while the camera
  // moved (at 3 the exact one is taken)
  int lodMismatchFrames = 0;
  std::array<ShadowTileRecord, 6> tiles = {};
};

// D152 (Q38's default): the parallel cascades redrawn at most per frame while the camera
// moves, and the frames in a row an exact LOD must differ before a held one changes
inline constexpr auto MovingCascadeBudget = std::size_t{2};
inline constexpr auto LodHysteresisFrames = 3;

/**
 * LP3.3 (C61, D146, A46, P3.26): the shadow records of the lights whose tiles are in the
 * layout, keyed by the light's preview id; no persistence outside the layout. A record is
 * dropped when its light does not take tiles in a frame (it stopped casting: noshadows,
 * LOD -1, no caster; it left the view; it was removed, nodesWereRemoved, so its record is
 * not in the light table), and every record on clear (a stop: documentWasLoaded stops the
 * session, D41; teardown). Render thread (LitPasses).
 */
class ShadowCache
{
public:
  using Records = std::unordered_map<PreviewId, ShadowRecord>;

private:
  Records m_records;

public:
  // a frame's casting lights (those of pass 6's loop 1): every record of a light not
  // among them is dropped, its placed tiles' rects appended to released (vacated:
  // LitPasses clears them and gives them back to the held layout)
  void beginFrame(std::span<const PreviewId> casting, std::vector<AtlasRect>& released);
  // the light's record, made empty for it when it has none
  ShadowRecord& record(PreviewId light);
  void drop(PreviewId light);
  // every tile of every record is drawn again (tests: cached-vs-fresh)
  void invalidateTiles();
  const ShadowRecord* find(PreviewId light) const;
  ShadowRecord* find(PreviewId light);
  const Records& records() const;
  std::size_t size() const;
  void clear();
};

} // namespace tb::lit
