/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
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

#ifndef __DECALPROJECTION_H__
#define __DECALPROJECTION_H__

/*
===============================================================================

	Decal projection maths shared by the renderer's runtime decals
	(idRenderModelDecal) and dmap's compile-time decals (func_decal), so both
	clip and texture a surface the same way.

	A projector is a convex volume of NUM_DECAL_BOUNDING_PLANES planes built
	from a four point winding (the far cap, its normal facing the projection
	origin) and a projection origin. Pure functions on plain data: no
	allocation, no renderer state; the material is only carried along.

===============================================================================
*/

class idMaterial;

static const int NUM_DECAL_BOUNDING_PLANES	= 6;

struct decalProjectionParms_t
{
	idPlane					boundingPlanes[NUM_DECAL_BOUNDING_PLANES];
	idPlane					fadePlanes[2];
	idPlane					textureAxis[2];
	idVec3					projectionOrigin;
	idBounds				projectionBounds;
	const idMaterial* 		material;
	float					fadeDepth;
	int						startTime;
	bool					parallel;
	bool					force;
};

class idDecalProjection
{
public:
	// Creates decal projection parameters from the far cap winding (with texture coordinates)
	// and the projection origin. Returns false when the winding does not have
	// NUM_DECAL_BOUNDING_PLANES - 2 points; the caller reports it.
	static bool				CreateParms( decalProjectionParms_t& parms, const idFixedWinding& winding, const idVec3& projectionOrigin, const bool parallel, const float fadeDepth, const idMaterial* material, const int startTime );

	// Clips one triangle to the projection volume: fw gets the part inside, with the decal's
	// texture coordinates and the triangle's vertex order. Only the bounding planes whose bit
	// is set in planeMask are clipped against (the renderer passes the planes its cull bits
	// say the triangle crosses, everything else passes all of them). Returns false, and fw
	// may hold anything, when the triangle faces away from the projector or nothing of it
	// is inside.
	static bool				ClipTriangle( const decalProjectionParms_t& parms, const idVec3& v0, const idVec3& v1, const idVec3& v2, const int planeMask, idFixedWinding& fw );

	// The raw depth fade of a point against one fade plane: 0 at the plane, 1 one fadeDepth
	// behind it, negative in front of it. Callers clamp and combine the two planes.
	static float			DepthFade( const idPlane& fadePlane, const float fadeDepth, const idVec3& xyz );

	// The clamp both decal kinds apply to a depth fade: 0 in front of the fade plane, 1 from 0.99
	// on, so the far end is fully faded. The vertex alpha is 1 minus the result.
	static float			ClampDepthFade( const float depthFade );
};

/*
=================
idDecalProjection::DepthFade

Inline: the runtime decals call it per vertex.
=================
*/
ID_INLINE float idDecalProjection::DepthFade( const idPlane& fadePlane, const float fadeDepth, const idVec3& xyz )
{
	return fadePlane.Distance( xyz ) * ( -1.0f / fadeDepth );
}

/*
=================
idDecalProjection::ClampDepthFade

Inline for the same reason.
=================
*/
ID_INLINE float idDecalProjection::ClampDepthFade( const float depthFade )
{
	if( depthFade < 0.0f )
	{
		return 0.0f;
	}
	if( depthFade > 0.99f )
	{
		return 1.0f;
	}
	return depthFade;
}

#endif /* !__DECALPROJECTION_H__ */
