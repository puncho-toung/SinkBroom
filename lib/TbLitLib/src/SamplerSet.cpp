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

#include "SamplerSet.h"

#include "lit/EngineTables.h"
#include "lit/ImageCache.h"

#include <fmt/format.h>

namespace tb::lit
{

Result<const SamplerSet::Common*> SamplerSet::common(nvrhi::IDevice& device)
{
  if (!m_made)
  {
    // CommonPasses.cpp:71-97, in its order
    const auto descs = engineCommonSamplers();
    auto made = Common{};
    made.pointClamp = device.createSampler(descs.pointClamp);
    made.pointWrap = device.createSampler(descs.pointWrap);
    made.linearClamp = device.createSampler(descs.linearClamp);
    made.linearBorder = device.createSampler(descs.linearBorder);
    made.linearClampCompare = device.createSampler(descs.linearClampCompare);
    made.linearWrap = device.createSampler(descs.linearWrap);
    made.anisotropicWrap = device.createSampler(descs.anisotropicWrap);
    made.anisotropicClampEdge = device.createSampler(descs.anisotropicClampEdge);
    if (
      !made.pointClamp || !made.pointWrap || !made.linearClamp || !made.linearBorder
      || !made.linearClampCompare || !made.linearWrap || !made.anisotropicWrap
      || !made.anisotropicClampEdge)
    {
      return Error{"NVRHI cannot create the common samplers"};
    }
    m_common = std::move(made);
    m_made = true;
  }
  return &m_common;
}

Result<nvrhi::ISampler*> SamplerSet::imageSampler(
  nvrhi::IDevice& device, const int format, const int filter, const int repeat)
{
  // Image_NVRHI.cpp:173-243 CreateSampler, cached as GetSampler's SamplerCache (310-333)
  const auto depth =
    format == engine_image::FormatDepth || format == engine_image::FormatDepthStencil;
  const auto key = std::tuple{filter, repeat, depth};
  if (const auto it = m_imageSamplers.find(key); it != m_imageSamplers.end())
  {
    return it->second.Get();
  }
  const auto desc = engineImageSamplerDesc(format, filter, repeat);
  if (!desc)
  {
    return Error{fmt::format(
      "the engine has no sampler for the filter {} and the repeat {}", filter, repeat)};
  }
  auto sampler = device.createSampler(*desc);
  if (!sampler)
  {
    return Error{"NVRHI cannot create an image's sampler"};
  }
  return m_imageSamplers.emplace(key, std::move(sampler)).first->second.Get();
}

std::size_t SamplerSet::size() const
{
  return (m_made ? std::size_t{8} : std::size_t{0}) + m_imageSamplers.size();
}

void SamplerSet::releaseAll()
{
  m_imageSamplers.clear();
  m_common = Common{};
  m_made = false;
}

} // namespace tb::lit
