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

#include "RangeAllocator.h"
#include "base/Macros.h"
#include "base/Result.h"
#include "lit/ChunkBuilder.h"
#include "lit/ModelInstance.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tb::lit
{

// LP6.4b, any thread (tests): the geometry arenas' size the next sessions make (0:
// ArenaBytes); process-wide like the texture heaps' hook, and the size a session takes at
// its phase B
void setGeometryArenaBytesForTesting(std::uint64_t bytes);
std::uint64_t geometryArenaBytes();

// where a chunk's geometry is: a range of a vertex arena and one of an index arena
struct ChunkPlacement
{
  std::size_t vertexArena = 0;
  ArenaRange vertices;
  std::size_t indexArena = 0;
  ArenaRange indexes;
  // C4: idDrawVert's 32 bytes; startVertexLocation of a draw is vertices.offset / 32
  std::uint32_t vertexCount = 0;
  // 16-bit, relative to the chunk's first vertex; startIndexLocation is offset / 2
  std::uint32_t indexCount = 0;
  std::uint64_t casterVersion = 0;
  std::uint64_t appearanceVersion = 0;
  // LP7.2 (D129): a shared model surface's ranges, answered for an instance's key
  bool shared = false;
};

/**
 * S8, D36, D78, §3.4 "Memory", §3.10: the chunks' geometry in a few large NVRHI buffers,
 * vertex arenas (isVertexBuffer) and index arenas (isIndexBuffer) of ArenaBytes each,
 * each with a RangeAllocator; another arena of a kind is added when none holds a chunk.
 * A chunk is a pair of ranges, never an allocation of its own (C30's counter moves per
 * arena, not per chunk).
 *
 * sync compares the scene's chunks with what is placed: a chunk whose key went, or whose
 * caster or appearance version changed, frees its ranges, stamped with the frame, and
 * they are handed out again only once that frame has finished (reclaim); a new or changed
 * chunk is queued.
 *
 * LP7.2 (C52, D129, P7.7): a second placement map holds the shared model surfaces, by
 * (record, surface, record version), each one vertex and one index range with the count
 * of the scene's instances that name it (syncInstances): a key no instance names any more
 * (the last instance gone, or its record's version moved to a new key) frees its ranges
 * stamped with the frame, as a chunk's (D35, D36); a new key is queued and written once
 * from its record's EngineDrawVerts and 16-bit indexes, after the queued chunks, within
 * the same budget. placement( an instance's chunk key ) answers its shared ranges, so a
 * draw reads an instance as a chunk; the used bytes count each placement once.
 * recordUploads writes queued chunks into their ranges through the frame's command list
 * (writeBuffer into the arena at the range's offset, NVRHI's staging, on the graphics
 * queue, D43), before the frame's first pass (D78), within the frame's byte budget: what
 * does not fit waits for the next frame, and the frame is unconverged (D77).
 *
 * Threads: the render thread; releaseAll in teardown step 2 on the GUI thread.
 */
class GeometryArenas
{
private:
  struct Arena
  {
    nvrhi::BufferHandle buffer;
    RangeAllocator ranges;
    // LP6.4b (the director's reading (7)): false once a compaction released it (its index
    // kept, taken by the next arena of the kind made)
    bool live = true;
  };

  nvrhi::IDevice* m_device = nullptr;
  std::uint64_t m_arenaBytes = 0;
  std::vector<Arena> m_vertexArenas;
  std::vector<Arena> m_indexArenas;
  std::map<ChunkKey, ChunkPlacement> m_placed;
  // the chunks to upload, in key order
  std::set<ChunkKey> m_queued;
  // LP7.2 (D129): the shared model surfaces, the instances that name each, and the
  // placement key of every instance's chunk key
  struct SharedPlacement
  {
    std::shared_ptr<const SharedSurface> geometry;
    std::size_t instances = 0;
    std::optional<ChunkPlacement> placement;
  };
  std::map<SharedGeometryKey, SharedPlacement> m_shared;
  std::set<SharedGeometryKey> m_sharedQueued;
  std::map<ChunkKey, SharedGeometryKey> m_instanceKeys;
  std::uint64_t m_uploadedShared = 0;
  // tests (placeForTesting): arenas without a device, ranges only
  bool m_cpuArenasForTesting = false;
  // the frame's uploads so far, and the whole session's
  std::uint64_t m_uploadedBytes = 0;
  std::uint64_t m_uploadedChunks = 0;
  // the scratch copy of an index range padded to whole 4-byte words
  std::vector<std::uint16_t> m_indexScratch;
  // U2.3: whether one more device allocation leaves the headroom (none: always), and the
  // arenas not made for it
  std::function<bool()> m_allocationRoom;
  std::uint64_t m_headroomRefusals = 0;
  bool m_refusedForHeadroom = false;
  // LP6.1 (C64, P8.11): bumped by every freed placement and by releaseAll (LP6.4b: and
  // once by every compaction that moved a placement)
  std::uint64_t m_generation = 0;
  // LP6.4b (C68 (d)): the compactions run, the bytes they moved, the arenas they
  // released; tests: the moves after which a pass stops as if no range held the next (0:
  // none)
  std::uint64_t m_compactions = 0;
  std::uint64_t m_movedBytes = 0;
  std::uint64_t m_arenasReleased = 0;
  std::size_t m_moveLimitForTesting = 0;

public:
  // LP6.4b (C68 (d), D174): a compaction's source, a kind's arena
  struct Compaction
  {
    bool vertices = true;
    std::size_t source = 0;

    bool operator==(const Compaction&) const = default;
  };
  // what a compaction did: the bytes and placements moved, whether a range found no room
  // (the pass stopped there, what moved stays moved, the source kept), and the emptied
  // source's buffer, which the caller retires with the frame (D35)
  struct CompactionResult
  {
    std::uint64_t movedBytes = 0;
    std::size_t moves = 0;
    bool stopped = false;
    nvrhi::BufferHandle released;
    std::uint64_t releasedBytes = 0;
  };

  GeometryArenas() = default;

  // GUI thread in phase B, or the render thread: the device the arenas are made on, and
  // their size (ArenaBytes; tests make smaller ones)
  void setDevice(nvrhi::IDevice& device, std::uint64_t arenaBytes);
  // render thread (U2.3): asked before a new arena is made; when it says no, the chunk
  // stays queued (its draws are skipped, the frame unconverged) and headroomRefusals
  // counts it
  void setAllocationRoom(std::function<bool()> room);
  std::uint64_t headroomRefusals() const;

  /**
   * Render thread: compares chunks with what is placed and queued; the ranges of a chunk
   * that went or changed are freed in frame frame, and new or changed chunks are queued.
   * Whether anything is queued.
   */
  bool sync(const std::map<ChunkKey, Chunk>& chunks, std::uint64_t frame);

  /**
   * LP7.2 (C52, D129, P7.7), render thread: counts the instances that name each shared
   * surface; a key none names any more frees its ranges in frame frame (reclaimed once it
   * has finished), a new one is queued. Whether anything is queued.
   */
  bool syncInstances(
    const std::map<ChunkKey, ModelInstance>& instances, std::uint64_t frame);

  // render thread: the ranges freed in frames that have finished (frames 0 to
  // completedFrames - 1) may be handed out again
  void reclaim(std::uint64_t completedFrames);

  /**
   * Render thread, inside the frame's command list before its first pass (D78): writes
   * queued chunks, in key order, while the bytes stay within budget (one chunk is written
   * whatever its size when nothing was written before it, so a chunk larger than the
   * budget is not stuck); the bytes written, or an error when an arena cannot be made
   * (an arena refused for the allocation headroom is not an error: the rest wait).
   */
  Result<std::uint64_t> recordUploads(
    nvrhi::ICommandList& commandList,
    const std::map<ChunkKey, Chunk>& chunks,
    std::uint64_t budget);

  /**
   * Tests (CPU): arenas of arenaBytes made without a device (ranges only), and the
   * placement recordUploads makes, its writes left out; what it places.
   */
  void setCpuArenasForTesting(std::uint64_t arenaBytes);
  Result<std::uint64_t> placeForTesting(
    const std::map<ChunkKey, Chunk>& chunks, std::uint64_t budget);

  // queued chunks wait for a later frame
  bool pending() const;
  std::size_t queuedChunks() const;
  std::size_t placedChunks() const;
  // a chunk's placement, or (LP7.2) an instance's shared one (ChunkPlacement::shared)
  std::optional<ChunkPlacement> placement(const ChunkKey& key) const;
  // LP7.2 (D129): the shared surfaces queued and placed, the instances that name a key,
  // and the shared surfaces written so far
  std::size_t queuedShared() const;
  std::size_t placedShared() const;
  std::size_t sharedInstances(const SharedGeometryKey& key) const;
  std::uint64_t uploadedShared() const;
  // LP7.2 (P7.14): the distinct records of the shared surfaces, and the bytes of those
  // placed (vertices and indexes)
  std::size_t sharedRecords() const;
  std::uint64_t placedSharedBytes() const;

  // the arena buffers, for the passes' vertex and index buffer bindings (null for a
  // released one); the arenas of each kind (LP6.4b: not released)
  nvrhi::IBuffer* vertexBuffer(std::size_t arena) const;
  nvrhi::IBuffer* indexBuffer(std::size_t arena) const;
  std::size_t vertexArenas() const;
  std::size_t indexArenas() const;

  // C11, C16: the arenas' bytes (their buffers' sizes), the bytes ranges hold, the bytes
  // freed and waiting for their frames; the bytes and chunks uploaded so far
  std::uint64_t capacityBytes() const;
  std::uint64_t usedBytes() const;
  std::uint64_t pendingFreeBytes() const;
  std::uint64_t uploadedBytes() const;
  std::uint64_t uploadedChunks() const;

  // teardown step 2: every buffer and range
  void releaseAll();

  /**
   * LP6.4b (C68 (d), D174, U2.9, the director's reading (6)), render thread, at a settled
   * frame: for each kind (vertices first) with two arenas or more, its least-used arena,
   * when every range placed in it (the chunks' and the shared placements') fits the
   * kind's other arenas: a dry run on copies of their allocators, the largest range
   * first, each first fit as allocate hands it out. nullopt when no kind has one.
   */
  std::optional<Compaction> planCompaction() const;
  /**
   * LP6.4b: the move of compaction's source in frame: each of its placements, the largest
   * first, gets a range in the kind's other arenas (never the source, never a new arena;
   * none: the pass stops there), its bytes copied (copyBuffer on the command list; none
   * without one, the CPU tests'), the placement updated in place (a chunk's, or a shared
   * one, which its instances' keys answer), its old range freed with the frame; the
   * generation bumped once when anything moved; the source, with no range handed out
   * after it, released (its buffer handed back to retire, its entry kept).
   */
  CompactionResult compact(
    nvrhi::ICommandList* commandList, const Compaction& compaction, std::uint64_t frame);
  // the compactions run, the bytes they moved and the arenas released so far
  std::uint64_t compactions() const;
  std::uint64_t movedBytes() const;
  std::uint64_t arenasReleased() const;
  // tests: a pass stops after moves moves as if no range held the next (0: never)
  void setCompactionMoveLimitForTesting(std::size_t moves);

  /**
   * LP6.1 (C64, P8.11), render thread: the arena generation, bumped whenever a placement
   * is freed (a chunk that went or changed, a shared surface no instance names; LP6.4b's
   * compaction will bump it too) and by releaseAll: the draw records that copied a
   * placement are cleared when it moves. A new placement does not bump it (no record
   * holds a placement it has not found).
   */
  std::uint64_t generation() const;

private:
  void freePlacement(const ChunkPlacement& placement, std::uint64_t frame);
  // recordUploads' work; without a command list (placeForTesting) nothing is written
  Result<std::uint64_t> upload(
    nvrhi::ICommandList* commandList,
    const std::map<ChunkKey, Chunk>& chunks,
    std::uint64_t budget);
  // one vertex and one index range for the bytes; nullopt when none was made: refused
  // for the headroom (error empty: the rest wait), or an arena cannot be made (error)
  std::optional<ChunkPlacement> place(
    std::uint64_t vertexBytes, std::uint64_t indexBytes, std::string& error);
  // a range in one of arenas, made larger by a new arena when none holds it; nullopt
  // when the device cannot make the arena
  std::optional<std::pair<std::size_t, ArenaRange>> allocate(
    std::vector<Arena>& arenas, std::uint64_t bytes, bool vertices);

  deleteCopyAndMove(GeometryArenas);
};

} // namespace tb::lit
