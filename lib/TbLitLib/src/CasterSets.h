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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/tr_frontend_addlights.cpp and
// renderer/tr_frontend_addmodels.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "LitFrontend.h"
#include "ViewFlow.h"
#include "base/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tb::lit
{

/**
 * LP6.2 (C66, D168, P8.2): what the kept caster sets were gathered under: the fields of
 * LitPasses' ListsKey that the gathering reads and the camera does not move, the chunk
 * and instance maps with the chunks' version (SceneGeometry changes the maps only in a
 * batch that applies, and the render thread raises chunksVersion then: an entity's
 * transform, references, visibility and its chunks' cast classes come with it), the
 * entity states, the bake's tree and AreaData, the material table's key (a new registry
 * or a re-parse, ListsTableKey), r_skipShadows' stand-in and whether the light-triangle
 * cull applies; and whether the view flowed (D94's no-tree rule, flowView's from the tree
 * and the areas). A moved key forgets every set and the candidates. Pure.
 */
struct CasterSetsKey
{
  const void* chunks = nullptr;
  std::uint64_t chunksVersion = 0;
  const void* instances = nullptr;
  const void* entities = nullptr;
  const void* tree = nullptr;
  const void* areas = nullptr;
  ListsTableKey table;
  bool skipShadows = false;
  bool lightTriangleCull = false;
  bool flowed = false;

  bool operator==(const CasterSetsKey&) const = default;
};

/**
 * LP6.2 (C66, D168): one caster of a kept set. Its identity is its ChunkKey, by value,
 * and whether it is a model instance's (D168: never a pointer as the identity, never a
 * space index); its entity is key.entity, whose space every build resolves (the camera
 * walk's first-seen order). Beside the identity, what the gathering resolved for it: the
 * chunk (an instance's header) and the instance, valid under the set's key as LP6.1's
 * records hold a Chunk* under the table's lifetime (SceneGeometry's chunk and instance
 * maps are untouched between chunksVersion moves); its material, its light triangles
 * (null: all of them; a Pending answer is kept as one), its term of the light's caster
 * hash (from its entity's model matrix, no view); and for a Perforated caster its
 * registers, whether they or its material are pending or not current, and its term of the
 * light's perforatedHash.
 */
struct KeptCaster
{
  ChunkKey key;
  bool isInstance = false;
  const Chunk* chunk = nullptr;
  const ModelInstance* instance = nullptr;
  std::shared_ptr<const MaterialEntry> material;
  std::shared_ptr<const LightTriangles> lightTriangles;
  std::uint64_t casterTerm = 0;
  bool perforated = false;
  std::shared_ptr<const EvaluatedRegisters> registers;
  bool provisional = false;
  std::uint64_t perforatedTerm = 0;
};

/**
 * LP6.2 (C66, D168, P8.2): a casting light's caster set as the gathering made it (the
 * casters in its order, ViewLight's hashes and count, FrontendLists' pair counts), kept
 * while its key holds: the sets' common key (CasterSetsKey), its light's record (the
 * version, and the record itself, held, so that a new record of an equal version is a
 * new key too), and two pending rules: a set with a Pending light-triangle pair is made
 * again once the cache's version has moved (P8.2: its answer may be among those taken);
 * one with a material pending, or a Perforated caster's registers pending or not current,
 * once the material table's version has (the rule rebuildLists applies to the lists).
 */
struct CasterSet
{
  std::shared_ptr<const LightRecord> record;
  std::uint64_t lightVersion = 0;
  std::vector<KeptCaster> casters;
  std::uint64_t casterHash = 0;
  std::size_t casterCount = 0;
  std::uint64_t perforatedHash = 0;
  // the (light, chunk or instance) pairs asked for, those answered (All, Runs or None)
  // and those Pending
  std::size_t pairs = 0;
  std::size_t pairsResident = 0;
  std::size_t pairsPending = 0;
  // the light-triangle cache's keys of the pairs (a chunk's key, an instance's), sorted:
  // the entries the set holds (LightTriangleCache::endBuild's bound skips them)
  std::vector<ChunkKey> pairKeys;
  // the Perforated candidates whose registers the gathering asked for (their entity and
  // material): the register cache's entries a build keeps for the set (D52)
  std::vector<std::pair<PreviewId, std::shared_ptr<const MaterialEntry>>>
    registerConsumers;
  // FrontendLists::pending's part: a Perforated candidate's material or registers pending
  // or not current (D77, D52)
  bool pending = false;
  // the pending rules: a Pending pair and the cache's version, a material or registers
  // pending and the table's version, at the set's making
  bool pairPending = false;
  std::uint64_t lightTrianglesVersion = 0;
  bool tablePending = false;
  std::uint64_t tableVersion = 0;
};

/**
 * LP6.2 (C66, D168, P8.2): the caster gathering of buildFrontendLists (LP3.0, C57, P3.14,
 * P3.16, P3.17: R_AddSingleLight's area walk from the light's own area,
 * lightReachesCaster, R_AddSingleModel's shadow surface rule: the cast class, the light
 * volume's cull, the spectrum, the light triangles; on screen or off) and its sets, kept
 * across builds. The candidates (every chunk and instance of a cast class, with their
 * AreaSurface: kind, area and their entity's references, copied) are made once per common
 * key; a casting light's set is made when it has none or its key moved (CasterSet), else
 * kept: a camera move, a new extent, the light table's version for an unchanged record,
 * the grid or the portal fog make none. What a set's making resolves of a candidate (its
 * entity's model space, visibility and shaderParms; its material; a Perforated one's
 * registers) is resolved once per build, on its first touch, whichever light's set
 * touches it. Without a kept one the frontend makes a fresh CasterSets per build, so that
 * every set is made: the one gathering. The light triangles and the registers the sets
 * hold are kept for them by their caches' rules (LitPasses:
 * LightTriangleCache::endBuild's bound skips holds(); the kept sets' registerConsumers
 * are marked used each build, keepRegisters). Render thread.
 */
class CasterSets
{
private:
  struct Candidate
  {
    // into the chunk and instance maps, valid under the common key
    const ChunkKey* key = nullptr;
    const Chunk* chunk = nullptr;
    const ModelInstance* instance = nullptr;
    AreaSurface areaSurface;
    // resolved on its first touch in a build (resolvedBuild); model null: an invisible
    // entity (no space)
    std::uint64_t resolvedBuild = 0;
    const LitSpace* model = nullptr;
    std::shared_ptr<const MaterialEntry> material;
    bool materialPending = false;
    std::shared_ptr<const EvaluatedRegisters> registers;
    bool provisional = false;
    std::uint64_t perforatedTerm = 0;
  };

  std::optional<CasterSetsKey> m_key;
  bool m_candidatesMade = false;
  std::vector<Candidate> m_candidates;
  // the candidates' entities' area references (AreaSurface::refs' storage)
  std::unordered_map<PreviewId, std::vector<int>> m_references;
  std::unordered_map<PreviewId, CasterSet> m_sets;
  // this build's model spaces per entity (none: invisible)
  std::unordered_map<PreviewId, std::optional<LitSpace>> m_models;
  std::uint64_t m_build = 0;
  std::size_t m_built = 0;
  // the light table the last sweep saw
  const LightTable* m_sweptLights = nullptr;
  std::uint64_t m_sweptVersion = 0;

public:
  // a light's set, and whether this build made it
  struct Answer
  {
    const CasterSet* set = nullptr;
    bool made = false;
  };

  // a build begins with its common key: a moved key forgets the sets and the candidates
  void beginBuild(const CasterSetsKey& key);
  /**
   * The set of a casting light (a view light with shadowLod >= 0 and a shader of spectrum
   * spectrum), made when it has none or its key moved, else the kept one; inputs is the
   * build's (chunks, instances, entities, areas, material, registers, lightTriangles and
   * the versions of FrontendInputs).
   */
  Answer set(
    const FrontendInputs& inputs,
    const std::shared_ptr<const LightRecord>& record,
    int spectrum);
  // the sets made in this build
  std::size_t built() const;
  // the build ended: when the light table moved, the sets of lights no longer in it go
  void endBuild(const LightTable* lights);
  // a kept set of light holds the light-triangle cache's entry of chunk (a chunk's key or
  // an instance's)
  bool holds(PreviewId light, const ChunkKey& chunk) const;
  const CasterSet* find(PreviewId light) const;
  std::size_t size() const;
  void clear();

private:
  void makeCandidates(const FrontendInputs& inputs);
  const LitSpace* modelOf(const FrontendInputs& inputs, PreviewId entity);
  void resolve(Candidate& candidate, const FrontendInputs& inputs);
  CasterSet make(
    const FrontendInputs& inputs,
    const std::shared_ptr<const LightRecord>& record,
    int spectrum);

public:
  CasterSets() = default;

  // the candidates' references point into m_references
  deleteCopyAndMove(CasterSets);
};

// LP6.2 (C66): the common key of a build's inputs, flowed its flow's. Pure
CasterSetsKey casterSetsKeyOf(const FrontendInputs& inputs, bool flowed);

} // namespace tb::lit
