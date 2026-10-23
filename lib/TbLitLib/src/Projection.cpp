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

#include "lit/Projection.h"

#include "lit/EngineGeometry.h"
#include "lit/NumberText.h"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tb::lit
{
namespace
{

// idMath::PI, M_DEG2RAD and M_RAD2DEG (Math.cpp:35-47), each a float
constexpr auto Pi = 3.14159265358979323846f;
constexpr auto Deg2Rad = Pi / 180.0f;
constexpr auto Rad2Deg = 180.0f / Pi;

EngineVec3 engineVec(const vm::vec3f& v)
{
  return {v.x(), v.y(), v.z()};
}

} // namespace

float verticalFovOf(const float trenchBroomFov)
{
  // in CalcFov's form: 2 · RAD2DEG( atan( ... ) ), each step a float
  const auto tanHalfX = std::tan(trenchBroomFov * 0.5f * Deg2Rad);
  return 2.0f * (std::atan(0.75f * tanHalfX) * Rad2Deg);
}

float gFovOf(const float verticalFov)
{
  const auto tanHalfY = std::tan(verticalFov * 0.5f * Deg2Rad);
  return 2.0f * (std::atan((16.0f / 9.0f) * tanHalfY) * Rad2Deg);
}

ViewProjection viewProjectionOf(const CameraState& camera)
{
  auto view = ViewProjection{};
  view.gFov = gFovOf(verticalFovOf(camera.fovDegrees));

  const auto width = int(camera.viewportWidth);
  const auto height = int(camera.viewportHeight);
  view.fov = engineCalcFov(view.gFov, width, height, ViewPixelAspect);
  view.projectionMatrix = engineProjectionMatrix(
    view.fov.x, view.fov.y, view.zNear, std::max(width, 1), std::max(height, 1));

  view.viewOrigin = engineVec(camera.position);
  view.viewAxis = engineViewAxis(engineVec(camera.direction), engineVec(camera.up));
  view.modelViewMatrix = engineViewMatrix(view.viewOrigin, view.viewAxis);
  return view;
}

std::string parityCameraLine(const CameraState& camera, const ViewProjection& view)
{
  // the engine's yaw and pitch of the forward axis (idVec3::ToPitch, ToYaw: pitch is
  // positive looking down)
  const auto& d = camera.direction;
  const auto yaw =
    std::atan2(double(d.y()), double(d.x())) * 180.0 / 3.14159265358979323846;
  const auto pitch =
    -std::atan2(
      double(d.z()),
      std::sqrt(double(d.x()) * double(d.x()) + double(d.y()) * double(d.y())))
    * 180.0 / 3.14159265358979323846;
  return fmt::format(
    "camera {} {} {} yaw {:.4f} pitch {:.4f} g_fov {} viewport {} {}",
    view.viewOrigin[0],
    view.viewOrigin[1],
    view.viewOrigin[2],
    yaw,
    pitch,
    view.gFov,
    camera.viewportWidth,
    camera.viewportHeight);
}

std::optional<float> trenchBroomFovOfGFov(const int gFov)
{
  if (gFov < 1 || gFov > 179)
  {
    return std::nullopt;
  }
  const auto target = float(gFov);
  // S39's chain in exact arithmetic: tan( g_fov / 2 ) = ( 16 / 9 ) · 0.75 · tan( fov / 2
  // )
  const auto estimate = float(
    2.0 * std::atan(0.75 * std::tan(double(gFov) * 0.5 * 3.14159265358979323846 / 180.0))
    * 180.0 / 3.14159265358979323846);
  auto below = estimate;
  auto above = estimate;
  for (auto step = 0; step < 4096; ++step)
  {
    if (gFovOf(verticalFovOf(below)) == target)
    {
      return below;
    }
    if (gFovOf(verticalFovOf(above)) == target)
    {
      return above;
    }
    below = std::nextafter(below, 0.0f);
    above = std::nextafter(above, 180.0f);
  }
  return std::nullopt;
}

std::optional<ParityCamera> parityCameraOfLine(const std::string_view line)
{
  auto tokens = std::vector<std::string_view>{};
  auto at = std::size_t{0};
  while (at < line.size())
  {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t'))
    {
      ++at;
    }
    const auto start = at;
    while (at < line.size() && line[at] != ' ' && line[at] != '\t')
    {
      ++at;
    }
    if (at > start)
    {
      tokens.push_back(line.substr(start, at - start));
    }
  }
  const auto number = [&](const std::size_t i) -> std::optional<float> {
    auto value = 0.0f;
    const auto& token = tokens[i];
    const auto* end = parseFloatChars(token.data(), token.data() + token.size(), value);
    return end == token.data() + token.size() ? std::optional{value} : std::nullopt;
  };
  if (
    tokens.size() != 13 || tokens[0] != "camera" || tokens[4] != "yaw"
    || tokens[6] != "pitch" || tokens[8] != "g_fov" || tokens[10] != "viewport")
  {
    return std::nullopt;
  }
  const auto x = number(1);
  const auto y = number(2);
  const auto z = number(3);
  const auto yaw = number(5);
  const auto pitch = number(7);
  const auto gFov = number(9);
  const auto width = number(11);
  const auto height = number(12);
  if (!x || !y || !z || !yaw || !pitch || !gFov || !width || !height)
  {
    return std::nullopt;
  }
  // the cvar is CVAR_INTEGER: its integer part (SysCvar.cpp:283)
  const auto integerFov = int(*gFov);
  const auto fov = trenchBroomFovOfGFov(integerFov);
  if (!fov)
  {
    return std::nullopt;
  }

  // U1.6: the copied idAngles( pitch, yaw, 0 ).ToMat3() (M-GEO-PURE-01): its forward
  // row and its up row
  const auto axis = engineAnglesToMat3(*pitch, *yaw, 0.0f);

  auto result = ParityCamera{};
  result.camera.position = vm::vec3f{*x, *y, *z};
  result.camera.direction = vm::vec3f{axis[0], axis[1], axis[2]};
  result.camera.up = vm::vec3f{axis[6], axis[7], axis[8]};
  result.camera.fovDegrees = *fov;
  result.camera.viewportWidth = std::uint32_t(*width);
  result.camera.viewportHeight = std::uint32_t(*height);
  result.gFov = integerFov;
  result.yaw = *yaw;
  result.pitch = *pitch;
  return result;
}

} // namespace tb::lit
