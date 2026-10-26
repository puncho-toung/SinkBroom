// ==== engine neo/renderer/Model.h lines 1-30 (licence and include guard) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

#ifndef __MODEL_H__
#define __MODEL_H__
// ==== engine neo/renderer/Model.h lines 52-57 (dominantTri_t) ====
// this is used for calculating unsmoothed normals and tangents for deformed models
struct dominantTri_t
{
	triIndex_t					v2, v3;
	float						normalizationScale[3];
};
// ==== engine neo/renderer/Model.h lines 61-115 (the forward declarations, srfTriangles_t (LP7.1a)) ====
class idRenderModelStatic;
class idImportOptions;
struct viewDef_t;

// our only drawing geometry type
struct srfTriangles_t
{
	srfTriangles_t() {}

	idBounds					bounds;					// for culling

	bool						generateNormals;		// create normals from geometry, instead of using explicit ones
	bool						tangentsCalculated;		// set when the vertex tangents have been calculated
	bool						perfectHull;			// true if there aren't any dangling edges
	bool						referencedVerts;		// if true the 'verts' are referenced and should not be freed
	bool						referencedIndexes;		// if true, indexes, silIndexes, mirrorVerts, and silEdges are
	// pointers into the original surface, and should not be freed

	int							numVerts;				// number of vertices
	idDrawVert* 				verts;					// vertices, allocated with special allocator

	int							numIndexes;				// for shadows, this has both front and rear end caps and silhouette planes
	triIndex_t* 				indexes;				// indexes, allocated with special allocator

	triIndex_t* 				silIndexes;				// indexes changed to be the first vertex with same XYZ, ignoring normal and texcoords

	// RB begin
	idVec4*						mocVerts;				// idDrawVert has no w position
	unsigned int* 				mocIndexes;				// uint32 instead of uint16 for the Masked Software Occlusion Culling SIMD loading code
	// RB end

	int							numMirroredVerts;		// this many verts at the end of the vert list are tangent mirrors
	int* 						mirroredVerts;			// tri->mirroredVerts[0] is the mirror of tri->numVerts - tri->numMirroredVerts + 0

	int							numDupVerts;			// number of duplicate vertexes
	int* 						dupVerts;				// pairs of the number of the first vertex and the number of the duplicate vertex

	dominantTri_t* 				dominantTris;			// [numVerts] for deformed surface fast tangent calculation

	srfTriangles_t* 			ambientSurface;			// for light interactions, point back at the original surface that generated
	// the interaction, which we will get the ambientCache from

	srfTriangles_t* 			nextDeferredFree;		// chain of tris to free next frame

	// for deferred normal / tangent transformations by joints
	// the jointsInverted list / buffer object on md5WithJoints may be
	// shared by multiple srfTriangles_t
	idRenderModelStatic* 		staticModelWithJoints;

	// data in vertex object space, not directly readable by the CPU
	vertCacheHandle_t			indexCache;				// GL_INDEX_TYPE
	vertCacheHandle_t			ambientCache;			// idDrawVert

	DISALLOW_COPY_AND_ASSIGN( srfTriangles_t );
};
// ==== engine neo/renderer/Model.h lines 325-325 (include guard end) ====
#endif /* !__MODEL_H__ */
