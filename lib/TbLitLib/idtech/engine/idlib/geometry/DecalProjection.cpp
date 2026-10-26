/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2016 Robert Beckebans
Copyright (C) 2026 Phillip Melnichenko

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

#include "precompiled.h"
#pragma hdrstop

// shared by the renderer's runtime decals and dmap's func_decal, so both clip and texture a
// surface the same way; decalSelfTest prints this maths on fixed input, and its output must
// not change

/*
=================
idDecalProjection::CreateParms
=================
*/
bool idDecalProjection::CreateParms( decalProjectionParms_t& parms, const idFixedWinding& winding, const idVec3& projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial* material, const int startTime )
{

	if( winding.GetNumPoints() != NUM_DECAL_BOUNDING_PLANES - 2 )
	{
		return false;
	}

	assert( material != NULL );

	parms.projectionOrigin = projectionOrigin;
	parms.material = material;
	parms.parallel = parallel;
	parms.fadeDepth = fadeDepth;
	parms.startTime = startTime;
	parms.force = false;

	// get the winding plane and the depth of the projection volume
	idPlane windingPlane;
	winding.GetPlane( windingPlane );
	float depth = windingPlane.Distance( projectionOrigin );

	// find the bounds for the projection
	winding.GetBounds( parms.projectionBounds );
	if( parallel )
	{
		parms.projectionBounds.ExpandSelf( depth );
	}
	else
	{
		parms.projectionBounds.AddPoint( projectionOrigin );
	}

	// calculate the world space projection volume bounding planes, positive sides face outside the decal
	if( parallel )
	{
		for( int i = 0; i < winding.GetNumPoints(); i++ )
		{
			idVec3 edge = winding[( i + 1 ) % winding.GetNumPoints()].ToVec3() - winding[i].ToVec3();
			parms.boundingPlanes[i].Normal().Cross( windingPlane.Normal(), edge );
			parms.boundingPlanes[i].Normalize();
			parms.boundingPlanes[i].FitThroughPoint( winding[i].ToVec3() );
		}
	}
	else
	{
		for( int i = 0; i < winding.GetNumPoints(); i++ )
		{
			parms.boundingPlanes[i].FromPoints( projectionOrigin, winding[i].ToVec3(), winding[( i + 1 ) % winding.GetNumPoints()].ToVec3() );
		}
	}
	parms.boundingPlanes[NUM_DECAL_BOUNDING_PLANES - 2] = windingPlane;
	parms.boundingPlanes[NUM_DECAL_BOUNDING_PLANES - 2][3] -= depth;
	parms.boundingPlanes[NUM_DECAL_BOUNDING_PLANES - 1] = -windingPlane;

	// fades will be from these plane
	parms.fadePlanes[0] = windingPlane;
	parms.fadePlanes[0][3] -= fadeDepth;
	parms.fadePlanes[1] = -windingPlane;
	parms.fadePlanes[1][3] += depth - fadeDepth;

	// calculate the texture vectors for the winding
	float	len, texArea, inva;
	idVec3	temp;
	idVec5	d0, d1;

	const idVec5& a = winding[0];
	const idVec5& b = winding[1];
	const idVec5& c = winding[2];

	d0 = b.ToVec3() - a.ToVec3();
	d0.s = b.s - a.s;
	d0.t = b.t - a.t;
	d1 = c.ToVec3() - a.ToVec3();
	d1.s = c.s - a.s;
	d1.t = c.t - a.t;

	texArea = ( d0[3] * d1[4] ) - ( d0[4] * d1[3] );
	inva = 1.0f / texArea;

	temp[0] = ( d0[0] * d1[4] - d0[4] * d1[0] ) * inva;
	temp[1] = ( d0[1] * d1[4] - d0[4] * d1[1] ) * inva;
	temp[2] = ( d0[2] * d1[4] - d0[4] * d1[2] ) * inva;
	len = temp.Normalize();
	parms.textureAxis[0].Normal() = temp * ( 1.0f / len );
	parms.textureAxis[0][3] = winding[0].s - ( winding[0].ToVec3() * parms.textureAxis[0].Normal() );

	temp[0] = ( d0[3] * d1[0] - d0[0] * d1[3] ) * inva;
	temp[1] = ( d0[3] * d1[1] - d0[1] * d1[3] ) * inva;
	temp[2] = ( d0[3] * d1[2] - d0[2] * d1[3] ) * inva;
	len = temp.Normalize();
	parms.textureAxis[1].Normal() = temp * ( 1.0f / len );
	parms.textureAxis[1][3] = winding[0].t - ( winding[0].ToVec3() * parms.textureAxis[1].Normal() );

	return true;
}

/*
=================
idDecalProjection::ClipTriangle
=================
*/
bool idDecalProjection::ClipTriangle( const decalProjectionParms_t& parms, const idVec3& v0, const idVec3& v1, const idVec3& v2, const int planeMask, idFixedWinding& fw )
{
	const idVec3* verts[3] = { &v0, &v1, &v2 };

	// skip back facing triangles
	const idPlane plane( v0, v1, v2 );
	if( plane.Normal() * parms.boundingPlanes[NUM_DECAL_BOUNDING_PLANES - 2].Normal() < -0.1f )
	{
		return false;
	}

	// create a winding with texture coordinates for the triangle
	fw.SetNumPoints( 3 );
	if( parms.parallel )
	{
		for( int j = 0; j < 3; j++ )
		{
			fw[j] = *verts[j];
			fw[j].s = parms.textureAxis[0].Distance( *verts[j] );
			fw[j].t = parms.textureAxis[1].Distance( *verts[j] );
		}
	}
	else
	{
		for( int j = 0; j < 3; j++ )
		{
			const idVec3 dir = *verts[j] - parms.projectionOrigin;
			float scale;
			parms.boundingPlanes[NUM_DECAL_BOUNDING_PLANES - 1].RayIntersection( *verts[j], dir, scale );
			const idVec3 intersection = *verts[j] + scale * dir;

			fw[j] = *verts[j];
			fw[j].s = parms.textureAxis[0].Distance( intersection );
			fw[j].t = parms.textureAxis[1].Distance( intersection );
		}
	}

	// clip the exact surface triangle to the projection volume
	for( int j = 0; j < NUM_DECAL_BOUNDING_PLANES; j++ )
	{
		if( ( planeMask & ( 1 << j ) ) != 0 )
		{
			if( !fw.ClipInPlace( -parms.boundingPlanes[j] ) )
			{
				break;
			}
		}
	}

	return ( fw.GetNumPoints() != 0 );
}
