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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/NVRHI/Image_NVRHI.cpp and
// renderer/Image_load.cpp):
//   Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
//   Copyright (C) 2013-2025 Robert Beckebans
//   Copyright (C) 2022 Stephen Pridham
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "ImageSlots.h"
#include "base/Result.h"
#include "lit/MaterialDecls.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <string>

namespace tb::lit
{

/**
 * SC265, SC266 (idImage::AllocImage, Image_NVRHI.cpp:345-630): the texture an image of
 * the cache's shape becomes: the format map (engineTextureFormat; a cache's RGB565 levels
 * arrive as RGBA8, P2.14e), the component mapping (engineComponentMapping), Texture2D, or
 * TextureCube with 6 slices, numLevels levels, the size rounded up to whole 4 × 4 blocks
 * for a block-compressed format, ShaderResource kept as the initial state; an error for a
 * format or texture type the engine refuses (BC7, ETC1, a format it Errors on). Pure.
 */
Result<nvrhi::TextureDesc> engineTextureDesc(
  const CachedImage& image, bool d24s8Supported, const std::string& name);

/**
 * GetRowPitch's bytes of one level record and its rows: the bytes writeTexture reads; an
 * error when the format has no pitch (T0.14) or the record holds fewer bytes. Pure.
 */
Result<std::uint64_t> levelUploadBytes(
  const CachedImage& image, const CachedImageLevel& level);

/**
 * SC268, P2.14c (LP2.1 step 5): the render thread's texture uploads, in its frame's
 * command list before the frame's first pass (D78). For each slot whose image is loading,
 * in slot order (an intrinsic before the slots it stands in for), the texture is made on
 * its first turn and its levels are written smallest first, a level (all six sides of a
 * cube's level) at a time, with writeTexture(texture, destZ, level, bytes, GetRowPitch)
 * as idImage::ActuallyLoadImage writes them (Image_load.cpp:878-891): nothing is
 * converted here. Each level written extends the slot's written range (its binding never
 * reaches an unwritten level, and its version bumps). The frame's bytes stay within cap:
 * a level that would pass it waits for the next frame, unless it is the frame's first
 * upload, so a level larger than the cap is not stuck. Once every level is written the
 * texture's state is made permanent (setPermanentTextureState(ShaderResource),
 * commitBarriers, as the engine) and the image's bytes are let go (P2.14a). An image the
 * engine would refuse makes its slot Default, with the reason. Two refusals are the
 * preview's own: U3.16, a level larger than UploadBytesPerFrame (the engine writes it
 * whole, mirror.json M-TEX-DIFF-02: here it would be one writeTexture through one upload
 * chunk of its size); U2.3, a new texture while allocationRoom says the device's
 * allocations are within AllocationHeadroom of its limit (the slot keeps binding its
 * intrinsic, headroomRefusals counts it). Render thread only.
 */
class TextureUploads
{
private:
  bool m_d24s8Supported = false;
  bool m_permanentState = true;
  std::uint64_t m_uploadedBytes = 0;
  std::uint64_t m_texturesMade = 0;
  std::uint64_t m_headroomRefusals = 0;

public:
  explicit TextureUploads(bool d24s8Supported = false);

  void setD24s8Supported(bool supported);

  /**
   * Writes what fits; frameBytes, the frame's uploads so far, grows by what is written.
   * allocationRoom, asked before each new texture, says whether one more device
   * allocation leaves the headroom (none: always). Whether a loading slot is left for a
   * later frame.
   */
  bool record(
    nvrhi::IDevice& device,
    nvrhi::ICommandList& commandList,
    ImageSlots& slots,
    std::uint64_t cap,
    std::uint64_t& frameBytes,
    const std::function<bool()>& allocationRoom = {});

  /**
   * Tests: whether a finished texture's state is made permanent (as the engine's, the
   * default). A test that reads a texture back through a copy turns it off first: NVRHI
   * never moves a permanent texture out of ShaderResource, so it cannot be a copy source.
   */
  void setPermanentStateForTesting(bool permanent);

  // the session's texture bytes written, textures made, and textures not made for the
  // allocation headroom (U2.3)
  std::uint64_t uploadedBytes() const;
  std::uint64_t texturesMade() const;
  std::uint64_t headroomRefusals() const;
};

} // namespace tb::lit
