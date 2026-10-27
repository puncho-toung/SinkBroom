// ==== engine neo/renderer/RenderBackend.cpp lines 1-31 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Carl Kenner
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2013-2024 Robert Beckebans
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
// ==== engine neo/renderer/RenderBackend.cpp lines 2748-2766 (MatrixOrthogonalProjectionRH) ====
static void MatrixOrthogonalProjectionRH( float m[16], float left, float right, float bottom, float top, float zNear, float zFar )
{
	m[0] = 2 / ( right - left );
	m[4] = 0;
	m[8] = 0;
	m[12] = ( left + right ) / ( left - right );
	m[1] = 0;
	m[5] = 2 / ( top - bottom );
	m[9] = 0;
	m[13] = ( top + bottom ) / ( bottom - top );
	m[2] = 0;
	m[6] = 0;
	m[10] = 1 / ( zNear - zFar );
	m[14] = zNear / ( zNear - zFar );
	m[3] = 0;
	m[7] = 0;
	m[11] = 0;
	m[15] = 1;
}
// ==== engine neo/renderer/RenderBackend.cpp lines 2839-3164 (idRenderBackend::SetupShadowMapMatrices) ====
void idRenderBackend::SetupShadowMapMatrices( viewLight_t* vLight, int side, idRenderMatrix& lightProjectionRenderMatrix, idRenderMatrix& lightViewRenderMatrix )
{
	if( vLight->parallel && side >= 0 )
	{
		assert( side >= 0 && side < 6 );

		// original light direction is from surface to light origin
		idVec3 lightDir = -vLight->lightCenter;
		if( lightDir.Normalize() == 0.0f )
		{
			lightDir[2] = -1.0f;
		}

		idMat3 rotation = lightDir.ToMat3();
		//idAngles angles = lightDir.ToAngles();
		//idMat3 rotation = angles.ToMat3();

		const idVec3 viewDir = viewDef->renderView.viewaxis[0];
		const idVec3 viewPos = viewDef->renderView.vieworg;

#if 1
		idRenderMatrix::CreateViewMatrix( viewDef->renderView.vieworg, rotation, lightViewRenderMatrix );
#else
		float lightViewMatrix[16];
		MatrixLookAtRH( lightViewMatrix, viewPos, lightDir, viewDir );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightViewMatrix, lightViewRenderMatrix );
#endif

		idBounds lightBounds;
		lightBounds.Clear();

		ALIGNTYPE16 frustumCorners_t corners;
		idRenderMatrix::GetFrustumCorners( corners, vLight->inverseBaseLightProject, bounds_zeroOneCube );

		idVec4 point, transf;
		for( int j = 0; j < 8; j++ )
		{
			point[0] = corners.x[j];
			point[1] = corners.y[j];
			point[2] = corners.z[j];
			point[3] = 1;

			lightViewRenderMatrix.TransformPoint( point, transf );
			transf[0] /= transf[3];
			transf[1] /= transf[3];
			transf[2] /= transf[3];

			lightBounds.AddPoint( transf.ToVec3() );
		}

		float lightProjectionMatrix[16];
		MatrixOrthogonalProjectionRH( lightProjectionMatrix, lightBounds[0][0], lightBounds[1][0], lightBounds[0][1], lightBounds[1][1], -lightBounds[1][2], -lightBounds[0][2] );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );


		// 	'frustumMVP' goes from global space -> camera local space -> camera projective space
		// invert the MVP projection so we can deform zero-to-one cubes into the frustum pyramid shape and calculate global bounds

		idRenderMatrix splitFrustumInverse;
		if( !idRenderMatrix::Inverse( viewDef->frustumMVPs[FRUSTUM_CASCADE1 + side], splitFrustumInverse ) )
		{
			idLib::Warning( "splitFrustumMVP invert failed" );
		}

		// splitFrustumCorners in global space
		ALIGNTYPE16 frustumCorners_t splitFrustumCorners;
		idRenderMatrix::GetFrustumCorners( splitFrustumCorners, splitFrustumInverse, bounds_unitCube );

#if 0
		idBounds splitFrustumBounds;
		splitFrustumBounds.Clear();
		for( int j = 0; j < 8; j++ )
		{
			point[0] = splitFrustumCorners.x[j];
			point[1] = splitFrustumCorners.y[j];
			point[2] = splitFrustumCorners.z[j];

			splitFrustumBounds.AddPoint( point.ToVec3() );
		}

		idVec3 center = splitFrustumBounds.GetCenter();
		float radius = splitFrustumBounds.GetRadius( center );

		//ALIGNTYPE16 frustumCorners_t splitFrustumCorners;
		splitFrustumBounds[0] = idVec3( -radius, -radius, -radius );
		splitFrustumBounds[1] = idVec3( radius, radius, radius );
		splitFrustumBounds.TranslateSelf( viewPos );
		idVec3 splitFrustumCorners2[8];
		splitFrustumBounds.ToPoints( splitFrustumCorners2 );

		for( int j = 0; j < 8; j++ )
		{
			splitFrustumCorners.x[j] = splitFrustumCorners2[j].x;
			splitFrustumCorners.y[j] = splitFrustumCorners2[j].y;
			splitFrustumCorners.z[j] = splitFrustumCorners2[j].z;
		}
#endif


		idRenderMatrix lightViewProjectionRenderMatrix;
		idRenderMatrix::Multiply( lightProjectionRenderMatrix, lightViewRenderMatrix, lightViewProjectionRenderMatrix );

		// find the bounding box of the current split in the light's clip space
		idBounds cropBounds;
		cropBounds.Clear();
		for( int j = 0; j < 8; j++ )
		{
			point[0] = splitFrustumCorners.x[j];
			point[1] = splitFrustumCorners.y[j];
			point[2] = splitFrustumCorners.z[j];
			point[3] = 1;

			lightViewRenderMatrix.TransformPoint( point, transf );
			transf[0] /= transf[3];
			transf[1] /= transf[3];
			transf[2] /= transf[3];

			cropBounds.AddPoint( transf.ToVec3() );
		}

		// don't let the frustum AABB be bigger than the light AABB
		if( cropBounds[0][0] < lightBounds[0][0] )
		{
			cropBounds[0][0] = lightBounds[0][0];
		}

		if( cropBounds[0][1] < lightBounds[0][1] )
		{
			cropBounds[0][1] = lightBounds[0][1];
		}

		if( cropBounds[1][0] > lightBounds[1][0] )
		{
			cropBounds[1][0] = lightBounds[1][0];
		}

		if( cropBounds[1][1] > lightBounds[1][1] )
		{
			cropBounds[1][1] = lightBounds[1][1];
		}

		cropBounds[0][2] = lightBounds[0][2];
		cropBounds[1][2] = lightBounds[1][2];

		//float cropMatrix[16];
		//MatrixCrop(cropMatrix, cropBounds[0], cropBounds[1]);

		//idRenderMatrix cropRenderMatrix;
		//idRenderMatrix::Transpose( *( idRenderMatrix* )cropMatrix, cropRenderMatrix );

		//idRenderMatrix tmp = lightProjectionRenderMatrix;
		//idRenderMatrix::Multiply( cropRenderMatrix, tmp, lightProjectionRenderMatrix );

		MatrixOrthogonalProjectionRH( lightProjectionMatrix, cropBounds[0][0], cropBounds[1][0], cropBounds[0][1], cropBounds[1][1], -cropBounds[1][2], -cropBounds[0][2] );
		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );

		vLight->shadowV[side] = lightViewRenderMatrix;
		vLight->shadowP[side] = lightProjectionRenderMatrix;

#if defined( USE_NVRHI )
		ALIGNTYPE16 const idRenderMatrix matClipToUvzw(
			1.0f,  0.0f, 0.0f,  0.0f,
			0.0f, -1.0f, 0.0f,  0.0f,
			0.0f,  0.0f, 2.0f, -1.0f,
			0.0f,  0.0f, 0.0f,  1.0f
		);

		idRenderMatrix::Multiply( matClipToUvzw, lightProjectionRenderMatrix, vLight->shadowP[side] );
#endif
	}
	else if( vLight->pointLight && side >= 0 )
	{
		assert( side >= 0 && side < 6 );

		float	viewMatrix[16] = {0};

		idVec3	vec;
		idVec3	origin = vLight->globalLightOrigin;

		// side of a point light
		switch( side )
		{
			case 0:
				viewMatrix[0] = 1;
				viewMatrix[9] = 1;
				viewMatrix[6] = -1;
				break;
			case 1:
				viewMatrix[0] = -1;
				viewMatrix[9] = -1;
				viewMatrix[6] = -1;
				break;
			case 2:
				viewMatrix[4] = 1;
				viewMatrix[1] = -1;
				viewMatrix[10] = 1;
				break;
			case 3:
				viewMatrix[4] = -1;
				viewMatrix[1] = -1;
				viewMatrix[10] = -1;
				break;
			case 4:
				viewMatrix[8] = 1;
				viewMatrix[1] = -1;
				viewMatrix[6] = -1;
				break;
			case 5:
				viewMatrix[8] = -1;
				viewMatrix[1] = 1;
				viewMatrix[6] = -1;
				break;
		}

		viewMatrix[12] = -origin[0] * viewMatrix[0] + -origin[1] * viewMatrix[4] + -origin[2] * viewMatrix[8];
		viewMatrix[13] = -origin[0] * viewMatrix[1] + -origin[1] * viewMatrix[5] + -origin[2] * viewMatrix[9];
		viewMatrix[14] = -origin[0] * viewMatrix[2] + -origin[1] * viewMatrix[6] + -origin[2] * viewMatrix[10];

		viewMatrix[3] = 0;
		viewMatrix[7] = 0;
		viewMatrix[11] = 0;
		viewMatrix[15] = 1;

		// from world space to light origin, looking down the X axis
		float	unflippedLightViewMatrix[16];

		// from world space to OpenGL view space, looking down the negative Z axis
		float	lightViewMatrix[16];

		static float	s_flipMatrix[16] =
		{
			// convert from our coordinate system (looking down X)
			// to OpenGL's coordinate system (looking down -Z)
			0, 0, -1, 0,
			-1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 0, 1
		};

		memcpy( unflippedLightViewMatrix, viewMatrix, sizeof( unflippedLightViewMatrix ) );
		R_MatrixMultiply( viewMatrix, s_flipMatrix, lightViewMatrix );

		idRenderMatrix::Transpose( *( idRenderMatrix* )lightViewMatrix, lightViewRenderMatrix );

		// set up 90 degree projection matrix
		const float zNear = 4;
		const float	fov = r_shadowMapFrustumFOV.GetFloat();

		float ymax = zNear * tan( fov * idMath::PI / 360.0f );
		float ymin = -ymax;

		float xmax = zNear * tan( fov * idMath::PI / 360.0f );
		float xmin = -xmax;

		const float width = xmax - xmin;
		const float height = ymax - ymin;

		// from OpenGL view space to OpenGL NDC ( -1 : 1 in XYZ )
		float lightProjectionMatrix[16];

		lightProjectionMatrix[0 * 4 + 0] = -2.0f * zNear / width;
		lightProjectionMatrix[1 * 4 + 0] = 0.0f;
		lightProjectionMatrix[2 * 4 + 0] = ( xmax + xmin ) / width;	// normally 0
		lightProjectionMatrix[3 * 4 + 0] = 0.0f;

		lightProjectionMatrix[0 * 4 + 1] = 0.0f;
		lightProjectionMatrix[1 * 4 + 1] = 2.0f * zNear / height;
		lightProjectionMatrix[2 * 4 + 1] = ( ymax + ymin ) / height;	// normally 0
		lightProjectionMatrix[3 * 4 + 1] = 0.0f;

		// this is the far-plane-at-infinity formulation, and
		// crunches the Z range slightly so w=0 vertexes do not
		// rasterize right at the wraparound point
		lightProjectionMatrix[0 * 4 + 2] = 0.0f;
		lightProjectionMatrix[1 * 4 + 2] = 0.0f;
		lightProjectionMatrix[2 * 4 + 2] = -0.999f; // adjust value to prevent imprecision issues

		// the D3D clip space Z is in range [0,1] instead of [-1,1]
		// FIXME -1.0f * zNear kills shadow depth
		lightProjectionMatrix[3 * 4 + 2] = -2.0f * zNear;

		lightProjectionMatrix[0 * 4 + 3] = 0.0f;
		lightProjectionMatrix[1 * 4 + 3] = 0.0f;
		lightProjectionMatrix[2 * 4 + 3] = -1.0f;
		lightProjectionMatrix[3 * 4 + 3] = 0.0f;

		idRenderMatrix::Transpose( *( idRenderMatrix* )lightProjectionMatrix, lightProjectionRenderMatrix );

		vLight->shadowV[side] = lightViewRenderMatrix;
		vLight->shadowP[side] = lightProjectionRenderMatrix;

#if defined( USE_NVRHI )
		ALIGNTYPE16 const idRenderMatrix matClipToUvzw(
			1.0f,  0.0f, 0.0f,  0.0f,
			0.0f, -1.0f, 0.0f,  0.0f,
			0.0f,  0.0f, 2.0f, -1.0f,
			0.0f,  0.0f, 0.0f,  1.0f
		);

		idRenderMatrix::Multiply( matClipToUvzw, lightProjectionRenderMatrix, vLight->shadowP[side] );
#endif
	}
	else
	{
		// spot light
		lightViewRenderMatrix.Identity();
		lightProjectionRenderMatrix = vLight->baseLightProject;

		vLight->shadowV[0] = lightViewRenderMatrix;
		vLight->shadowP[0] = lightProjectionRenderMatrix;

#if defined( USE_NVRHI )
		// Calculate alternate matrix that maps to [0, 1] UV space instead of [-1, 1] clip space
		ALIGNTYPE16 const idRenderMatrix matClipToUvzw(
			0.5f,  0.0f, 0.0f, 0.5f,
			0.0f, -0.5f, 0.0f, 0.5f,
			0.0f,  0.0f, 1.0f, 0.0f,
			0.0f,  0.0f, 0.0f, 1.0f
		);

		idRenderMatrix shadowToClip;
		idRenderMatrix::Multiply( renderMatrix_windowSpaceToClipSpace, lightProjectionRenderMatrix, shadowToClip );
		idRenderMatrix::Multiply( matClipToUvzw, shadowToClip, vLight->shadowP[0] );
#endif
	}
}
