// ==== engine neo/renderer/GLMatrix.cpp lines 1-29 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2024 Robert Beckebans
Copyright (C) 2022 Stephen Pridham

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
// ==== engine neo/renderer/GLMatrix.cpp lines 548-603 (R_SetupProjectionMatrix2) ====
void R_SetupProjectionMatrix2( const viewDef_t* viewDef, const float zNear, const float zFar, float projectionMatrix[16] )
{
	float ymax = zNear * tan( viewDef->renderView.fov_y * idMath::PI / 360.0f );
	float ymin = -ymax;

	float xmax = zNear * tan( viewDef->renderView.fov_x * idMath::PI / 360.0f );
	float xmin = -xmax;

	const float width = xmax - xmin;
	const float height = ymax - ymin;

	const int viewWidth = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int viewHeight = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	float jitterx, jittery;
	jitterx = 0.0f;
	jittery = 0.0f;
	jitterx = jitterx * width / viewWidth;
	jitterx += r_centerX.GetFloat();
	jitterx += viewDef->renderView.stereoScreenSeparation;
	xmin += jitterx * width;
	xmax += jitterx * width;

	jittery = jittery * height / viewHeight;
	jittery += r_centerY.GetFloat();
	ymin += jittery * height;
	ymax += jittery * height;

	float depth = zFar - zNear;

	projectionMatrix[0 * 4 + 0] = 2.0f * zNear / width;
	projectionMatrix[1 * 4 + 0] = 0.0f;
	projectionMatrix[2 * 4 + 0] = ( xmax + xmin ) / width;	// normally 0
	projectionMatrix[3 * 4 + 0] = 0.0f;

	projectionMatrix[0 * 4 + 1] = 0.0f;
	projectionMatrix[1 * 4 + 1] = 2.0f * zNear / height;
	projectionMatrix[2 * 4 + 1] = ( ymax + ymin ) / height;	// normally 0
	projectionMatrix[3 * 4 + 1] = 0.0f;

	projectionMatrix[0 * 4 + 2] = 0.0f;
	projectionMatrix[1 * 4 + 2] = 0.0f;
	projectionMatrix[2 * 4 + 2] =  -( zFar + zNear ) / depth;		// -0.999f; // adjust value to prevent imprecision issues
	projectionMatrix[3 * 4 + 2] = -2 * zFar * zNear / depth;	// -2.0f * zNear;

	projectionMatrix[0 * 4 + 3] = 0.0f;
	projectionMatrix[1 * 4 + 3] = 0.0f;
	projectionMatrix[2 * 4 + 3] = -1.0f;
	projectionMatrix[3 * 4 + 3] = 0.0f;

	if( viewDef->renderView.flipProjection )
	{
		projectionMatrix[1 * 4 + 1] = -viewDef->projectionMatrix[1 * 4 + 1];
		projectionMatrix[1 * 4 + 3] = -viewDef->projectionMatrix[1 * 4 + 3];
	}
}
