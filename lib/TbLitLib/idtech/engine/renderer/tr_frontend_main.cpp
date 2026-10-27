// ==== engine neo/renderer/tr_frontend_main.cpp lines 1-28 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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
// ==== engine neo/renderer/tr_frontend_main.cpp lines 397-456 (R_SetupSplitFrustums) ====
static void R_SetupSplitFrustums( viewDef_t* viewDef )
{
	idVec3			planeOrigin;

	const float zNearStart = ( viewDef->renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();
	float zFarEnd = 10000;

	float zNear = zNearStart;
	float zFar = zFarEnd;

	float lambda = r_shadowMapSplitWeight.GetFloat();
	float ratio = zFarEnd / zNearStart;

	for( int i = 0; i < 6; i++ )
	{
		tr.viewDef->frustumSplitDistances[i] = idMath::INFINITUM;
	}

	for( int i = 1; i <= ( r_shadowMapSplits.GetInteger() + 1 ) && i < MAX_FRUSTUMS; i++ )
	{
		float si = i / ( float )( r_shadowMapSplits.GetInteger() + 1 );

		if( i > FRUSTUM_CASCADE1 )
		{
			zNear = zFar - ( zFar * 0.005f );
		}

		zFar = 1.005f * lambda * ( zNearStart * powf( ratio, si ) ) + ( 1 - lambda ) * ( zNearStart + ( zFarEnd - zNearStart ) * si );

		if( i <= r_shadowMapSplits.GetInteger() )
		{
			tr.viewDef->frustumSplitDistances[i - 1] = zFar;
		}

		float projectionMatrix[16];
		R_SetupProjectionMatrix2( tr.viewDef, zNear, zFar, projectionMatrix );

		// setup render matrices for faster culling
		idRenderMatrix projectionRenderMatrix;
		idRenderMatrix::Transpose( *( idRenderMatrix* )projectionMatrix, projectionRenderMatrix );
		idRenderMatrix viewRenderMatrix;
		idRenderMatrix::Transpose( *( idRenderMatrix* )tr.viewDef->worldSpace.modelViewMatrix, viewRenderMatrix );
		idRenderMatrix::Multiply( projectionRenderMatrix, viewRenderMatrix, tr.viewDef->frustumMVPs[i] );

		// the planes of the view frustum are needed for portal visibility culling
		idRenderMatrix::GetFrustumPlanes( tr.viewDef->frustums[i], tr.viewDef->frustumMVPs[i], false, true );

		// the DOOM 3 frustum planes point outside the frustum
		for( int j = 0; j < 6; j++ )
		{
			tr.viewDef->frustums[i][j] = - tr.viewDef->frustums[i][j];
		}

		// remove the Z-near to avoid portals from being near clipped
		if( i == FRUSTUM_CASCADE1 )
		{
			tr.viewDef->frustums[i][4][3] -= r_znear.GetFloat();
		}
	}
}
