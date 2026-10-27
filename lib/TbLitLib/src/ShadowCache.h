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

#include <array>
#include <cstddef>
#include <cstdint>
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
 * - perforated: reserved for the perforated casters' evaluated stage registers and their
 *   material entries' revision (P3.6, LP3.4: they draw nothing before), 0;
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
  std::array<ShadowTileRecord, 6> tiles = {};
};

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
  // among them is dropped (its tiles' rects are vacated: LitPasses clears them)
  void beginFrame(std::span<const PreviewId> casting);
  // the light's record, made empty for it when it has none
  ShadowRecord& record(PreviewId light);
  void drop(PreviewId light);
  const ShadowRecord* find(PreviewId light) const;
  const Records& records() const;
  std::size_t size() const;
  void clear();
};

} // namespace tb::lit
