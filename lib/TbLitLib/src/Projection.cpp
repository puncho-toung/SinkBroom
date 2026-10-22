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

#include <fmt/format.h>

#include <algorithm>
#include <cmath>

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

} // namespace tb::lit
