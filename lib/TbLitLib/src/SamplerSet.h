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
// GPL Source Code and RBDOOM-3-BFG (neo/renderer/Passes/CommonPasses.cpp and
// renderer/NVRHI/Image_NVRHI.cpp):
//   Copyright (C) 2022 Stephen Pridham
//   Copyright (C) 2023-2025 Robert Beckebans
// The NVIDIA Donut code the engine adapted there (CommonPasses) is under the MIT licence.
// That code is also subject to the additional terms that follow the GNU General Public
// License in lib/TbLitLib/idtech/engine/LICENSE.md.
// ---- end of engine notice ----

#pragma once

#include "base/Macros.h"
#include "base/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <map>
#include <tuple>

namespace tb::lit
{

/**
 * D68, SC240, SC269 (LP2.1 step 7): the render thread's samplers. The common set is
 * CommonRenderPasses::Init's eight, built as the engine builds them
 * (engineCommonSamplers: one desc changed in place, so that from LinearClampCompare on
 * each keeps the Comparison reduction, AnisotropicWrap, the interaction's s0, among
 * them), made once per session on the render thread. An image's own sampler (the TEXTURE
 * layout's) is engineImageSamplerDesc of its filter and repeat, a depth format with
 * Comparison, made once per (filter, repeat, depth) as idImage::GetSampler's cache makes
 * it. Render thread only; releaseAll in teardown step 2.
 */
class SamplerSet
{
public:
  struct Common
  {
    nvrhi::SamplerHandle pointClamp;
    nvrhi::SamplerHandle pointWrap;
    nvrhi::SamplerHandle linearClamp;
    nvrhi::SamplerHandle linearBorder;
    nvrhi::SamplerHandle linearClampCompare;
    nvrhi::SamplerHandle linearWrap;
    nvrhi::SamplerHandle anisotropicWrap;
    nvrhi::SamplerHandle anisotropicClampEdge;
  };

private:
  Common m_common;
  bool m_made = false;
  std::map<std::tuple<int, int, bool>, nvrhi::SamplerHandle> m_imageSamplers;

public:
  SamplerSet() = default;

  // render thread: the common set, made on the first call of the session
  Result<const Common*> common(nvrhi::IDevice& device);

  /**
   * Render thread: the sampler of an image of this textureFormat_t, textureFilter_t and
   * textureRepeat_t, made once per (filter, repeat, depth); an error for a filter or
   * repeat the engine refuses with FatalError, or one NVRHI cannot make.
   */
  Result<nvrhi::ISampler*> imageSampler(
    nvrhi::IDevice& device, int format, int filter, int repeat);

  // the samplers made: the common set's and the images'
  std::size_t size() const;

  // teardown step 2
  void releaseAll();

  deleteCopyAndMove(SamplerSet);
};

} // namespace tb::lit
