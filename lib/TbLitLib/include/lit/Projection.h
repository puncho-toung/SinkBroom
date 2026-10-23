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

#include "lit/EngineLight.h"
#include "lit/LitSession.h"

#include <optional>
#include <string>
#include <string_view>

namespace tb::lit
{

// r_znear's default (RenderSystem_init.cpp:118); the engine's cramZNear (a quarter of it)
// is the player's weapon view, which the preview never draws (A16)
constexpr auto ViewZNear = 3.0f;
// glConfig.pixelAspect without a stereo mode
constexpr auto ViewPixelAspect = 1.0f;

/**
 * S39: TrenchBroom's fov is the horizontal fov of a 4:3 frame, so the vertical fov is
 * fov_y = 2 · atan( 0.75 · tan( fov / 2 ) ), in degrees, in float.
 */
float verticalFovOf(float trenchBroomFov);

/**
 * The g_fov whose 16:9 ideal frame (CalcFov) has that vertical fov:
 * g_fov = 2 · atan( ( 16 / 9 ) · tan( fov_y / 2 ) ), in degrees, in float. The engine's
 * g_fov is an integer cvar; the parity camera line prints this value (§6.1).
 */
float gFovOf(float verticalFov);

/**
 * The engine's view of the preview's camera (§3.2, A16, P.16): g_fov from the camera's
 * fovDegrees (TrenchBroom's zoomedFov) through S39, CalcFov with the viewport as the
 * window (a square one takes g_fov for both axes), R_SetupProjectionMatrix (zNear 3,
 * infinite far, clip z 0..1, no jitter) and R_SetupViewMatrix of the camera's position
 * and its viewaxis (forward, left = up × forward, up; TrenchBroom and the engine are both
 * Z-up), each matrix a float[16] in the engine's layout.
 */
struct ViewProjection
{
  float gFov = 0.0f;
  EngineFov fov;
  float zNear = ViewZNear;
  EngineVec3 viewOrigin = {0, 0, 0};
  EngineMat3 viewAxis = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  EngineGlMatrix projectionMatrix = {};
  EngineGlMatrix modelViewMatrix = {};
};

/**
 * Pure: it calls only the light facade's view functions, which read their arguments and
 * constants (mirror.json M-LIGHT-PURE-02), so any thread, the render thread included.
 * A viewport of 0 counts as 1 for the projection's (zero) jitter offsets.
 */
ViewProjection viewProjectionOf(const CameraState& camera);

/**
 * The parity camera line (§6.1): the camera's position, its yaw and pitch in degrees,
 * the g_fov and the viewport, for the TOOLS script that places the engine's camera. It
 * prints g_fov with every digit; the engine's g_fov cvar keeps only its integer part.
 */
std::string parityCameraLine(const CameraState& camera, const ViewProjection& view);

/**
 * C29, P2.6 (LP2.3): S39's inverse for the engine's integer g_fov: a TrenchBroom fov
 * whose gFovOf( verticalFovOf( fov ) ) is exactly g_fov in float, so that
 * viewProjectionOf's CalcFov and R_SetupProjectionMatrix get the engine's own base_fov.
 * The float nearest to 2 · atan( 0.75 · tan( g_fov / 2 ) ) that gives it exactly,
 * searched within 4096 ulps of it; nullopt when none does. Pure; any thread.
 */
std::optional<float> trenchBroomFovOfGFov(int gFov);

// A parity camera line read back (C29): the camera with the engine's view, its integer
// g_fov, and the yaw and pitch it was given in degrees
struct ParityCamera
{
  CameraState camera;
  int gFov = 0;
  float yaw = 0.0f;
  float pitch = 0.0f;
};

/**
 * C29 (LP2.3): reads a parity camera line ("camera x y z yaw Y pitch P g_fov G viewport W
 * H", parityCameraLine's and parity.py's format). The camera's direction and up are
 * idAngles( pitch, yaw, 0 ).ToMat3()'s rows 0 and 2, from the copied code through
 * engineAnglesToMat3 (U1.6, M-PARITY-01), as the engine's player view has them after
 * setviewpos; its fov is trenchBroomFovOfGFov of the integer g_fov (the line's value
 * truncated as the cvar truncates it); its viewport the line's. nullopt for a line that
 * does not read or a g_fov without an exact inverse. Pure; any thread.
 */
std::optional<ParityCamera> parityCameraOfLine(std::string_view line);

} // namespace tb::lit
