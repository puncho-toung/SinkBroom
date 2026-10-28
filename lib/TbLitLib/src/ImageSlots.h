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

#include "FrameResources.h"
#include "InlineVector.h"
#include "base/Macros.h"
#include "lit/MaterialService.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tb::lit
{

// a slot of the image-slot table (C28): an index, stable for the session
using ImageSlotId = std::uint32_t;
inline constexpr auto NoImageSlot = ImageSlotId(-1);

// a slot and the version of it a binding set was made with (C28)
struct SlotVersion
{
  ImageSlotId slot = NoImageSlot;
  std::uint64_t version = 0;

  bool operator==(const SlotVersion&) const = default;
};

/**
 * P2.14d, C28: the intrinsic image a slot of this usage (textureUsage_t) binds until its
 * texture has arrived, and when the service has none for it (ImageLookup's Default): a
 * bump map _flat, a light's falloff or projection image (TD_LIGHT) _white, anything else
 * _default. The engine loads its images before it draws, so it never binds a stand-in
 * (mirror.json M-IMG-DIFF-11, the preview's own rule). Pure.
 */
std::string_view pendingIntrinsicName(int usage);

/**
 * The key a material's stage, and the render thread, ask the service for an intrinsic
 * image with: the shim's record of it (ShimRenderer.cpp ImageFromFunction), which keeps
 * the record's defaults (TD_DEFAULT, TF_DEFAULT, TR_REPEAT, CF_2D) whatever the engine's
 * generator makes; _default is the default image. Pure.
 */
MaterialImageKey intrinsicImageKey(std::string_view name);

// LP2's intrinsic images (D69), which the render thread asks for at its start
std::vector<MaterialImageKey> lp2IntrinsicKeys();

// what the table answers for a key now (MaterialService::image; tests fake it)
using ImageRequest = std::function<ImageLookup(const MaterialImageKey&)>;

/**
 * C28, D37, P2.14 (LP2.1): the render thread's image-slot table. A slot per
 * MaterialImageKey holds {texture, version, state, the written mip range}. A request
 * asks the service (MaterialService::image) at once; the slot is Pending until the
 * adopted table publishes it, and refresh, at a frame's start, asks again only for the
 * pending slots (O(pending)). Until its texture has arrived a slot binds its usage's
 * intrinsic (pendingIntrinsicName), and so does a slot the service answers Default.
 *
 * The service handshake (P2.14a/b): the slot keeps the image's bytes only until the
 * uploader has written them (dropPixels), so that the service's budget (S2.4), which
 * evicts only images a consumer has taken, may drop them; when an adopted table says
 * that a slot's files changed (MaterialTable::changedImages, a reload or the poll's
 * re-probe), the slot is asked again, its texture let go and its version bumped: Pending,
 * the intrinsic, then the new texture. The budget's own drops are not changes: a slot
 * that holds its texture keeps it. LPF.1 (C40, D98, D102): a slot asked again while it
 * holds a complete texture (Resident) is Replacing instead: the complete texture stays
 * bound, with the slot's version, while the new image is answered, made and written, and
 * at the frame in which the new texture's last level is written the slot swaps (the new
 * texture bound, the version bumped, the old one retired for the frames in flight), so no
 * frame between binds the intrinsic; asked again while Replacing (a third image before
 * the second landed), the unfinished new texture is retired and the newest image starts
 * again, the complete one still bound. A slot with no complete texture is asked again as
 * before (Pending, its intrinsic). An eviction's or a test's drop (dropTexture) lets the
 * textures go as before.
 *
 * Every change of what a slot binds bumps its version (an arrival, each extension of the
 * written mip range, a re-request, a drop), and the bumped slots are reported once
 * (takeChangedSlots), so that the binding-set cache erases exactly the dependent sets.
 * Textures are made and written by the uploader (TextureUploads, step 5) through
 * attachTexture and levelsWritten.
 *
 * Threads: the render thread; releaseAll in teardown step 2.
 */
class ImageSlots
{
public:
  enum class State
  {
    // asked for; the table has not answered yet
    Pending,
    // the table has no image for it: it binds its intrinsic
    Default,
    // the image's bytes are here; its texture is made and its levels written
    Loading,
    // every level is written; the bytes are let go
    Resident,
    // LPF.1 (C40, D98): asked again while Resident: the complete texture (replaced) stays
    // bound while the new image is answered and written (pixels, texture); Resident again
    // at the swap
    Replacing,
  };

  struct Slot
  {
    MaterialImageKey key;
    State state = State::Pending;
    std::uint64_t version = 1;
    // the slot of its usage's intrinsic; NoImageSlot for an intrinsic itself
    ImageSlotId intrinsic = NoImageSlot;
    nvrhi::TextureHandle texture;
    // the texture's levels; [firstWrittenMip, mipLevels) are written (none when equal)
    int mipLevels = 0;
    int firstWrittenMip = 0;
    std::uint64_t textureBytes = 0;
    // Loading: the bytes to write, let go once they are written
    std::shared_ptr<const CachedImage> pixels;
    // U2.1: the two facts the draws read of the service's slot it was answered with
    // (the record's usage, which the PBR rename may change, and its image's format), held
    // as values: a reference to that slot would keep the image's bytes past dropPixels
    std::optional<int> adoptedUsage;
    std::optional<int> adoptedFormat;
    // the table's change count when it was answered
    std::uint64_t adoptedChange = 0;
    // LPF.1 (C40, D98): Replacing: the complete texture the slot binds while its new one
    // is written, its levels and bytes; retired at the swap
    nvrhi::TextureHandle replaced;
    int replacedMipLevels = 0;
    std::uint64_t replacedBytes = 0;
    // the new image's facts (adoptedUsage, adoptedFormat), the draws' from the swap on
    std::optional<int> replacingUsage;
    std::optional<int> replacingFormat;
    // D102: Replacing, the new texture waits for the allocation headroom (said once)
    bool waitsForRoom = false;
    // why the service has no image (Default)
    std::string reason;
  };

  // what a slot binds: its own texture's written levels, or its intrinsic's
  struct Binding
  {
    // null when neither the slot nor its intrinsic has a written level yet
    nvrhi::ITexture* texture = nullptr;
    nvrhi::TextureSubresourceSet subresources;
    // the slot asked for, and the intrinsic standing in for it when one does; LP6.1
    // (C64): held inline, at most these two
    InlineVector<SlotVersion, 2> versions;
    bool intrinsic = false;
  };

private:
  ImageRequest m_request;
  std::vector<Slot> m_slots;
  std::unordered_map<MaterialImageKey, ImageSlotId, MaterialImageKeyHash> m_ids;
  std::unordered_set<ImageSlotId> m_pending;
  std::vector<ImageSlotId> m_changed;
  // LP6.1 (C64, P8.1): the slots whose adopted usage changed without a version bump
  std::vector<ImageSlotId> m_usageChanged;
  std::uint64_t m_tableVersion = 0;
  std::uint64_t m_seenChanges = 0;
  // LP4.3 (D79, P4.6): bumped on every change of a slot's state
  std::uint64_t m_stateVersion = 0;
  // textures let go, until the frames that used them have finished (D35)
  RetiredResources m_retired;
  // the frame being recorded (refresh, dropTexture), which a swap retires the replaced
  // texture at
  std::uint64_t m_frame = 0;

public:
  ImageSlots() = default;

  // the service's answers (MaterialService::image); none: every slot stays Pending
  void setRequest(ImageRequest request);

  // the slot of a key, made and asked for on its first request
  ImageSlotId request(const MaterialImageKey& key);

  /**
   * At a frame's start, with the latest table: when it is a new version, the pending
   * slots are asked again (O(pending)), and when its change count moved (P2.14b) every
   * slot holding an image whose key changed since is asked again. frame is the frame
   * about to be recorded, for the textures let go.
   */
  void refresh(const MaterialTable& table, std::uint64_t frame);
  // D77: whether the table answers a pending slot (a texture arrival), without adopting
  bool anyArrivals(const MaterialTable& table) const;

  // the uploader's (step 5): the slot's texture of mipLevels levels, none written yet
  void attachTexture(
    ImageSlotId slot, nvrhi::TextureHandle texture, int mipLevels, std::uint64_t bytes);
  // the levels from firstMip on are written: the binding's range grows, the version bumps
  void levelsWritten(ImageSlotId slot, int firstMip);
  // every level is written: the bytes are let go (P2.14a); a Replacing slot swaps (its
  // new texture bound, its version bumped, the replaced one retired: LPF.1, D98)
  void dropPixels(ImageSlotId slot);
  // the uploader cannot make the image (a format the engine refuses): Default, with why;
  // a Replacing slot keeps its complete texture bound and lets the new one go
  void refuse(ImageSlotId slot, std::string reason);
  /**
   * LPF.1 (D102): a Replacing slot's new texture is not made for the allocation headroom
   * (U2.3): the complete texture stays bound and the slot stays Replacing, its image
   * kept, so a later frame tries again; true the first time since the slot's last texture
   * was made (the uploader counts it once).
   */
  bool waitForRoom(ImageSlotId slot);
  // tests: the slot of key holds image, as if the service had answered it (Loading)
  ImageSlotId provideForTesting(
    const MaterialImageKey& key, std::shared_ptr<const CachedImage> image);
  // the slot's texture is let go and it is asked again (eviction, a test's drop): Pending
  // and its intrinsic until the texture is back
  void dropTexture(ImageSlotId slot, std::uint64_t frame);

  // what a slot binds now
  Binding binding(ImageSlotId slot) const;

  const Slot* slot(ImageSlotId slot) const;
  std::optional<ImageSlotId> find(const MaterialImageKey& key) const;
  std::size_t size() const;
  std::size_t pending() const;
  // the slots whose images wait to be written (Loading, and Replacing with its image), in
  // id order
  std::vector<ImageSlotId> loading() const;
  bool hasLoading() const;
  // LPF.1 (C40): the slots Replacing (tests, the frame log)
  std::size_t replacing() const;
  // slots with at least one written level, and their textures' bytes (C16, C11)
  std::size_t resident() const;
  std::uint64_t textureBytes() const;
  std::uint64_t retiredBytes() const;

  // the slots whose version changed since the last call, each once
  std::vector<ImageSlotId> takeChangedSlots();
  /**
   * LP6.1 (C64, P8.1): the slots whose adopted usage (the PBR rename, which can change a
   * draw's program) changed since the last call without a version bump (an answer:
   * Pending to Loading or Default), each once; a slot's first answer at its request is
   * not one (nothing has drawn it yet). The binding-set cache does not read them (what a
   * slot binds has not changed); the draw records do.
   */
  std::vector<ImageSlotId> takeUsageChangedSlots();
  // LP6.1 (C64): the changes not taken yet, as they are (a frame's passes look at what a
  // test's hook changed after the uploads' erase; nothing in a session's own frames)
  const std::vector<ImageSlotId>& changedSlots() const;
  const std::vector<ImageSlotId>& usageChangedSlots() const;

  /**
   * LP4.3 (D79, P4.6): a counter bumped whenever a slot's state changes (an answer:
   * Pending to Loading or Default; every level written: Loading to Resident; a refusal; a
   * re-request back to Pending), so that what depends on the states (the light grid's
   * dispatch, C36) knows when to look again. A new slot is Pending and bumps nothing.
   */
  std::uint64_t stateVersion() const;

  // D35: the textures let go in frames that have finished are released
  void collectRetired(std::uint64_t completedFrames);
  // teardown step 2
  void releaseAll();

private:
  // reported: an adopted usage's change is reported (takeUsageChangedSlots); not at the
  // slot's request
  void adopt(
    ImageSlotId id,
    const ImageLookup& lookup,
    std::uint64_t change,
    bool reported = true);
  void bump(ImageSlotId id);
  // asks for a slot's image again; keepComplete (a change, LPF.1): a slot holding a
  // complete texture keeps it bound and is Replacing
  void reRequest(ImageSlotId id, std::uint64_t frame, bool keepComplete);
  // a Replacing slot's replaced texture is retired and its new one bound (the swap), or
  // (restore) the replaced one bound again and the new one retired
  void endReplacing(ImageSlotId id, bool restore);

  deleteCopyAndMove(ImageSlots);
};

} // namespace tb::lit
