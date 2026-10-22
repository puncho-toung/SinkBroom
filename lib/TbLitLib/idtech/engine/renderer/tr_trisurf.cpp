// ==== engine neo/renderer/tr_trisurf.cpp lines 1-34 (licence and includes (the MikkTSpace include is left out, Q18)) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2020 Stephen Pridham (Mikkelsen tangent space support)
Copyright (C) 2021-2024 Robert Beckebans

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

#include "RenderCommon.h"
// ==== engine neo/renderer/tr_trisurf.cpp lines 38-38 (r_useSilRemap) ====
idCVar r_useSilRemap( "r_useSilRemap", "1", CVAR_RENDERER | CVAR_BOOL, "consider verts with the same XYZ, but different ST the same for shadows" );
// ==== engine neo/renderer/tr_trisurf.cpp lines 170-174 (USE_INVA, DERIVE_UNSMOOTHED_BITANGENT) ====
// this shouldn't change anything, but previously renderbumped models seem to need it
#define USE_INVA

// instead of using the texture T vector, cross the normal and S vector for an orthogonal axis
#define DERIVE_UNSMOOTHED_BITANGENT
// ==== engine neo/renderer/tr_trisurf.cpp lines 374-405 (R_AllocStaticTriSurf, R_CopyStaticTriSurf) ====
/*
==============
R_AllocStaticTriSurf
==============
*/
srfTriangles_t* R_AllocStaticTriSurf()
{
	srfTriangles_t* tris = ( srfTriangles_t* )Mem_ClearedAlloc( sizeof( srfTriangles_t ), TAG_SRFTRIS );
	return tris;
}

/*
=================
R_CopyStaticTriSurf

This only duplicates the indexes and verts, not any of the derived data.
=================
*/
srfTriangles_t* R_CopyStaticTriSurf( const srfTriangles_t* tri )
{
	srfTriangles_t*	newTri;

	newTri = R_AllocStaticTriSurf();
	R_AllocStaticTriSurfVerts( newTri, tri->numVerts );
	R_AllocStaticTriSurfIndexes( newTri, tri->numIndexes );
	newTri->numVerts = tri->numVerts;
	newTri->numIndexes = tri->numIndexes;
	memcpy( newTri->verts, tri->verts, tri->numVerts * sizeof( newTri->verts[0] ) );
	memcpy( newTri->indexes, tri->indexes, tri->numIndexes * sizeof( newTri->indexes[0] ) );

	return newTri;
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 407-460 (R_AllocStaticTriSurfVerts, Indexes, SilIndexes, DominantTris, MirroredVerts) ====
/*
=================
R_AllocStaticTriSurfVerts
=================
*/
void R_AllocStaticTriSurfVerts( srfTriangles_t* tri, int numVerts )
{
	assert( tri->verts == NULL );
	tri->verts = ( idDrawVert* )Mem_Alloc16( numVerts * sizeof( idDrawVert ), TAG_TRI_VERTS );
}

/*
=================
R_AllocStaticTriSurfIndexes
=================
*/
void R_AllocStaticTriSurfIndexes( srfTriangles_t* tri, int numIndexes )
{
	assert( tri->indexes == NULL );
	tri->indexes = ( triIndex_t* )Mem_Alloc16( numIndexes * sizeof( triIndex_t ), TAG_TRI_INDEXES );
}

/*
=================
R_AllocStaticTriSurfSilIndexes
=================
*/
void R_AllocStaticTriSurfSilIndexes( srfTriangles_t* tri, int numIndexes )
{
	assert( tri->silIndexes == NULL );
	tri->silIndexes = ( triIndex_t* )Mem_Alloc16( numIndexes * sizeof( triIndex_t ), TAG_TRI_SIL_INDEXES );
}

/*
=================
R_AllocStaticTriSurfDominantTris
=================
*/
void R_AllocStaticTriSurfDominantTris( srfTriangles_t* tri, int numVerts )
{
	assert( tri->dominantTris == NULL );
	tri->dominantTris = ( dominantTri_t* )Mem_Alloc16( numVerts * sizeof( dominantTri_t ), TAG_TRI_DOMINANT_TRIS );
}

/*
=================
R_AllocStaticTriSurfMirroredVerts
=================
*/
void R_AllocStaticTriSurfMirroredVerts( srfTriangles_t* tri, int numMirroredVerts )
{
	assert( tri->mirroredVerts == NULL );
	tri->mirroredVerts = ( int* )Mem_Alloc16( numMirroredVerts * sizeof( *tri->mirroredVerts ), TAG_TRI_MIR_VERT );
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 495-507 (R_ResizeStaticTriSurfVerts) ====
/*
=================
R_ResizeStaticTriSurfVerts
=================
*/
void R_ResizeStaticTriSurfVerts( srfTriangles_t* tri, int numVerts )
{
	idDrawVert* newVerts = ( idDrawVert* )Mem_Alloc16( numVerts * sizeof( idDrawVert ), TAG_TRI_VERTS );
	const int copy = Min( numVerts, tri->numVerts );
	memcpy( newVerts, tri->verts, copy * sizeof( idDrawVert ) );
	Mem_Free( tri->verts );
	tri->verts = newVerts;
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 611-697 (R_CreateSilRemap, R_CreateSilIndexes) ====
/*
=================
R_CreateSilRemap
=================
*/
static int* R_CreateSilRemap( const srfTriangles_t* tri )
{
	int		c_removed, c_unique;
	int*		remap;
	int		i, j, hashKey;
	const idDrawVert* v1, *v2;

	remap = ( int* )R_ClearedStaticAlloc( tri->numVerts * sizeof( remap[0] ) );

	if( !r_useSilRemap.GetBool() )
	{
		for( i = 0; i < tri->numVerts; i++ )
		{
			remap[i] = i;
		}
		return remap;
	}

	idHashIndex		hash( 1024, tri->numVerts );

	c_removed = 0;
	c_unique = 0;
	for( i = 0; i < tri->numVerts; i++ )
	{
		v1 = &tri->verts[i];

		// see if there is an earlier vert that it can map to
		hashKey = hash.GenerateKey( v1->xyz );
		for( j = hash.First( hashKey ); j >= 0; j = hash.Next( j ) )
		{
			v2 = &tri->verts[j];
			if( v2->xyz[0] == v1->xyz[0]
					&& v2->xyz[1] == v1->xyz[1]
					&& v2->xyz[2] == v1->xyz[2] )
			{
				c_removed++;
				remap[i] = j;
				break;
			}
		}
		if( j < 0 )
		{
			c_unique++;
			remap[i] = i;
			hash.Add( hashKey, i );
		}
	}

	return remap;
}

/*
=================
R_CreateSilIndexes

Uniquing vertexes only on xyz before creating sil edges reduces
the edge count by about 20% on Q3 models
=================
*/
void R_CreateSilIndexes( srfTriangles_t* tri )
{
	int		i;
	int*		remap;

	if( tri->silIndexes )
	{
		Mem_Free( tri->silIndexes );
		tri->silIndexes = NULL;
	}

	remap = R_CreateSilRemap( tri );

	// remap indexes to the first one
	R_AllocStaticTriSurfSilIndexes( tri, tri->numIndexes );
	assert( tri->silIndexes != NULL );
	for( i = 0; i < tri->numIndexes; i++ )
	{
		tri->silIndexes[i] = remap[tri->indexes[i]];
	}

	R_StaticFree( remap );
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 742-866 (R_FaceNegativePolarity, tangentVert_t, R_DuplicateMirroredVertexes) ====
/*
===============
R_FaceNegativePolarity

Returns true if the texture polarity of the face is negative, false if it is positive or zero
===============
*/
static bool R_FaceNegativePolarity( const srfTriangles_t* tri, int firstIndex )
{
	const idDrawVert* a = tri->verts + tri->indexes[firstIndex + 0];
	const idDrawVert* b = tri->verts + tri->indexes[firstIndex + 1];
	const idDrawVert* c = tri->verts + tri->indexes[firstIndex + 2];

	const idVec2 aST = a->GetTexCoord();
	const idVec2 bST = b->GetTexCoord();
	const idVec2 cST = c->GetTexCoord();

	float d0[5];
	d0[3] = bST[0] - aST[0];
	d0[4] = bST[1] - aST[1];

	float d1[5];
	d1[3] = cST[0] - aST[0];
	d1[4] = cST[1] - aST[1];

	const float area = d0[3] * d1[4] - d0[4] * d1[3];
	if( area >= 0 )
	{
		return false;
	}
	return true;
}

/*
===================
R_DuplicateMirroredVertexes

Modifies the surface to bust apart any verts that are shared by both positive and
negative texture polarities, so tangent space smoothing at the vertex doesn't
degenerate.

This will create some identical vertexes (which will eventually get different tangent
vectors), so never optimize the resulting mesh, or it will get the mirrored edges back.

Reallocates tri->verts and changes tri->indexes in place
Silindexes are unchanged by this.

sets mirroredVerts and mirroredVerts[]
===================
*/
struct tangentVert_t
{
	bool	polarityUsed[2];
	int		negativeRemap;
};

static void	R_DuplicateMirroredVertexes( srfTriangles_t* tri )
{
	tangentVert_t*	vert;
	int				i, j;
	int				totalVerts;
	int				numMirror;

	idTempArray<tangentVert_t> tverts( tri->numVerts );
	tverts.Zero();

	// determine texture polarity of each surface

	// mark each vert with the polarities it uses
	for( i = 0; i < tri->numIndexes; i += 3 )
	{
		int	polarity = R_FaceNegativePolarity( tri, i );
		for( j = 0; j < 3; j++ )
		{
			tverts[tri->indexes[i + j]].polarityUsed[ polarity ] = true;
		}
	}

	// now create new vertex indices as needed
	totalVerts = tri->numVerts;
	for( i = 0; i < tri->numVerts; i++ )
	{
		vert = &tverts[i];
		if( vert->polarityUsed[0] && vert->polarityUsed[1] )
		{
			vert->negativeRemap = totalVerts;
			totalVerts++;
		}
	}

	tri->numMirroredVerts = totalVerts - tri->numVerts;

	if( tri->numMirroredVerts == 0 )
	{
		tri->mirroredVerts = NULL;
		return;
	}

	// now create the new list
	R_AllocStaticTriSurfMirroredVerts( tri, tri->numMirroredVerts );
	R_ResizeStaticTriSurfVerts( tri, totalVerts );

	// create the duplicates
	numMirror = 0;
	for( i = 0; i < tri->numVerts; i++ )
	{
		j = tverts[i].negativeRemap;
		if( j )
		{
			tri->verts[j] = tri->verts[i];
			tri->mirroredVerts[numMirror] = i;
			numMirror++;
		}
	}
	tri->numVerts = totalVerts;

	// change the indexes
	for( i = 0; i < tri->numIndexes; i++ )
	{
		if( tverts[tri->indexes[i]].negativeRemap && R_FaceNegativePolarity( tri, 3 * ( i / 3 ) ) )
		{
			tri->indexes[i] = tverts[tri->indexes[i]].negativeRemap;
		}
	}
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 885-1093 (R_DeriveNormalsAndTangents, R_DeriveUnsmoothedNormalsAndTangents) ====
/*
============
R_DeriveNormalsAndTangents

Derives the normal and orthogonal tangent vectors for the triangle vertices.
For each vertex the normal and tangent vectors are derived from all triangles
using the vertex which results in smooth tangents across the mesh.
============
*/
void R_DeriveNormalsAndTangents( srfTriangles_t* tri )
{
	idTempArray< idVec3 > vertexNormals( tri->numVerts );
	idTempArray< idVec3 > vertexTangents( tri->numVerts );
	idTempArray< idVec3 > vertexBitangents( tri->numVerts );

	vertexNormals.Zero();
	vertexTangents.Zero();
	vertexBitangents.Zero();

	for( int i = 0; i < tri->numIndexes; i += 3 )
	{
		const int v0 = tri->indexes[i + 0];
		const int v1 = tri->indexes[i + 1];
		const int v2 = tri->indexes[i + 2];

		const idDrawVert* a = tri->verts + v0;
		const idDrawVert* b = tri->verts + v1;
		const idDrawVert* c = tri->verts + v2;

		const idVec2 aST = a->GetTexCoord();
		const idVec2 bST = b->GetTexCoord();
		const idVec2 cST = c->GetTexCoord();

		float d0[5];
		d0[0] = b->xyz[0] - a->xyz[0];
		d0[1] = b->xyz[1] - a->xyz[1];
		d0[2] = b->xyz[2] - a->xyz[2];
		d0[3] = bST[0] - aST[0];
		d0[4] = bST[1] - aST[1];

		float d1[5];
		d1[0] = c->xyz[0] - a->xyz[0];
		d1[1] = c->xyz[1] - a->xyz[1];
		d1[2] = c->xyz[2] - a->xyz[2];
		d1[3] = cST[0] - aST[0];
		d1[4] = cST[1] - aST[1];

		idVec3 normal;
		normal[0] = d1[1] * d0[2] - d1[2] * d0[1];
		normal[1] = d1[2] * d0[0] - d1[0] * d0[2];
		normal[2] = d1[0] * d0[1] - d1[1] * d0[0];

		const float f0 = idMath::InvSqrt( normal.x * normal.x + normal.y * normal.y + normal.z * normal.z );

		normal.x *= f0;
		normal.y *= f0;
		normal.z *= f0;

		// area sign bit
		const float area = d0[3] * d1[4] - d0[4] * d1[3];
		unsigned int signBit = ( *( unsigned int* )&area ) & ( 1 << 31 );

		idVec3 tangent;
		tangent[0] = d0[0] * d1[4] - d0[4] * d1[0];
		tangent[1] = d0[1] * d1[4] - d0[4] * d1[1];
		tangent[2] = d0[2] * d1[4] - d0[4] * d1[2];

		const float f1 = idMath::InvSqrt( tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z );
		*( unsigned int* )&f1 ^= signBit;

		tangent.x *= f1;
		tangent.y *= f1;
		tangent.z *= f1;

		idVec3 bitangent;
		bitangent[0] = d0[3] * d1[0] - d0[0] * d1[3];
		bitangent[1] = d0[3] * d1[1] - d0[1] * d1[3];
		bitangent[2] = d0[3] * d1[2] - d0[2] * d1[3];

		const float f2 = idMath::InvSqrt( bitangent.x * bitangent.x + bitangent.y * bitangent.y + bitangent.z * bitangent.z );
		*( unsigned int* )&f2 ^= signBit;

		bitangent.x *= f2;
		bitangent.y *= f2;
		bitangent.z *= f2;

		vertexNormals[v0] += normal;
		vertexTangents[v0] += tangent;
		vertexBitangents[v0] += bitangent;

		vertexNormals[v1] += normal;
		vertexTangents[v1] += tangent;
		vertexBitangents[v1] += bitangent;

		vertexNormals[v2] += normal;
		vertexTangents[v2] += tangent;
		vertexBitangents[v2] += bitangent;
	}

	// add the normal of a duplicated vertex to the normal of the first vertex with the same XYZ
	for( int i = 0; i < tri->numDupVerts; i++ )
	{
		vertexNormals[tri->dupVerts[i * 2 + 0]] += vertexNormals[tri->dupVerts[i * 2 + 1]];
	}

	// copy vertex normals to duplicated vertices
	for( int i = 0; i < tri->numDupVerts; i++ )
	{
		vertexNormals[tri->dupVerts[i * 2 + 1]] = vertexNormals[tri->dupVerts[i * 2 + 0]];
	}

	// Project the summed vectors onto the normal plane and normalize.
	// The tangent vectors will not necessarily be orthogonal to each
	// other, but they will be orthogonal to the surface normal.
	for( int i = 0; i < tri->numVerts; i++ )
	{
		const float normalScale = idMath::InvSqrt( vertexNormals[i].x * vertexNormals[i].x + vertexNormals[i].y * vertexNormals[i].y + vertexNormals[i].z * vertexNormals[i].z );
		vertexNormals[i].x *= normalScale;
		vertexNormals[i].y *= normalScale;
		vertexNormals[i].z *= normalScale;

		vertexTangents[i] -= ( vertexTangents[i] * vertexNormals[i] ) * vertexNormals[i];
		vertexBitangents[i] -= ( vertexBitangents[i] * vertexNormals[i] ) * vertexNormals[i];

		const float tangentScale = idMath::InvSqrt( vertexTangents[i].x * vertexTangents[i].x + vertexTangents[i].y * vertexTangents[i].y + vertexTangents[i].z * vertexTangents[i].z );
		vertexTangents[i].x *= tangentScale;
		vertexTangents[i].y *= tangentScale;
		vertexTangents[i].z *= tangentScale;

		const float bitangentScale = idMath::InvSqrt( vertexBitangents[i].x * vertexBitangents[i].x + vertexBitangents[i].y * vertexBitangents[i].y + vertexBitangents[i].z * vertexBitangents[i].z );
		vertexBitangents[i].x *= bitangentScale;
		vertexBitangents[i].y *= bitangentScale;
		vertexBitangents[i].z *= bitangentScale;
	}

	// compress the normals and tangents
	for( int i = 0; i < tri->numVerts; i++ )
	{
		tri->verts[i].SetNormal( vertexNormals[i] );
		tri->verts[i].SetTangent( vertexTangents[i] );
		tri->verts[i].SetBiTangent( vertexBitangents[i] );
	}
}

/*
============
R_DeriveUnsmoothedNormalsAndTangents
============
*/
void R_DeriveUnsmoothedNormalsAndTangents( srfTriangles_t* tri )
{
	for( int i = 0; i < tri->numVerts; i++ )
	{
		float d0, d1, d2, d3, d4;
		float d5, d6, d7, d8, d9;
		float s0, s1, s2;
		float n0, n1, n2;
		float t0, t1, t2;
		float t3, t4, t5;

		const dominantTri_t& dt = tri->dominantTris[i];

		idDrawVert* a = tri->verts + i;
		idDrawVert* b = tri->verts + dt.v2;
		idDrawVert* c = tri->verts + dt.v3;

		const idVec2 aST = a->GetTexCoord();
		const idVec2 bST = b->GetTexCoord();
		const idVec2 cST = c->GetTexCoord();

		d0 = b->xyz[0] - a->xyz[0];
		d1 = b->xyz[1] - a->xyz[1];
		d2 = b->xyz[2] - a->xyz[2];
		d3 = bST[0] - aST[0];
		d4 = bST[1] - aST[1];

		d5 = c->xyz[0] - a->xyz[0];
		d6 = c->xyz[1] - a->xyz[1];
		d7 = c->xyz[2] - a->xyz[2];
		d8 = cST[0] - aST[0];
		d9 = cST[1] - aST[1];

		s0 = dt.normalizationScale[0];
		s1 = dt.normalizationScale[1];
		s2 = dt.normalizationScale[2];

		n0 = s2 * ( d6 * d2 - d7 * d1 );
		n1 = s2 * ( d7 * d0 - d5 * d2 );
		n2 = s2 * ( d5 * d1 - d6 * d0 );

		t0 = s0 * ( d0 * d9 - d4 * d5 );
		t1 = s0 * ( d1 * d9 - d4 * d6 );
		t2 = s0 * ( d2 * d9 - d4 * d7 );

#ifndef DERIVE_UNSMOOTHED_BITANGENT
		t3 = s1 * ( d3 * d5 - d0 * d8 );
		t4 = s1 * ( d3 * d6 - d1 * d8 );
		t5 = s1 * ( d3 * d7 - d2 * d8 );
#else
		t3 = s1 * ( n2 * t1 - n1 * t2 );
		t4 = s1 * ( n0 * t2 - n2 * t0 );
		t5 = s1 * ( n1 * t0 - n0 * t1 );
#endif

		a->SetNormal( n0, n1, n2 );
		a->SetTangent( t0, t1, t2 );
		a->SetBiTangent( t3, t4, t5 );
	}
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 1150-1509 (R_DeriveTangentsWithoutNormals, indexSort_t, IndexSort, R_BuildDominantTris, R_DeriveTangents) ====
/*
=================
R_DeriveTangentsWithoutNormals

Build texture space tangents for bump mapping
If a surface is deformed, this must be recalculated

This assumes that any mirrored vertexes have already been duplicated, so
any shared vertexes will have the tangent spaces smoothed across.

Texture wrapping slightly complicates this, but as long as the normals
are shared, and the tangent vectors are projected onto the normals, the
separate vertexes should wind up with identical tangent spaces.

mirroring a normalmap WILL cause a slightly visible seam unless the normals
are completely flat around the edge's full bilerp support.

Vertexes which are smooth shaded must have their tangent vectors
in the same plane, which will allow a seamless
rendering as long as the normal map is even on both sides of the
seam.

A smooth shaded surface may have multiple tangent vectors at a vertex
due to texture seams or mirroring, but it should only have a single
normal vector.

Each triangle has a pair of tangent vectors in it's plane

Should we consider having vertexes point at shared tangent spaces
to save space or speed transforms?

this version only handles bilateral symetry
=================
*/
void R_DeriveTangentsWithoutNormals( srfTriangles_t* tri, bool useMikktspace )
{
	// SP begin
	if( useMikktspace )
	{
		if( !R_DeriveMikktspaceTangents( tri ) )
		{
			idLib::Warning( "Mikkelsen tangent space calculation failed" );
		}
		else
		{
			tri->tangentsCalculated = true;
			return;
		}
	}
	// SP End

	idTempArray< idVec3 > triangleTangents( tri->numIndexes / 3 );
	idTempArray< idVec3 > triangleBitangents( tri->numIndexes / 3 );

	//
	// calculate tangent vectors for each face in isolation
	//
	int c_positive = 0;
	int c_negative = 0;
	int c_textureDegenerateFaces = 0;
	for( int i = 0; i < tri->numIndexes; i += 3 )
	{
		idVec3	temp;

		idDrawVert* a = tri->verts + tri->indexes[i + 0];
		idDrawVert* b = tri->verts + tri->indexes[i + 1];
		idDrawVert* c = tri->verts + tri->indexes[i + 2];

		const idVec2 aST = a->GetTexCoord();
		const idVec2 bST = b->GetTexCoord();
		const idVec2 cST = c->GetTexCoord();

		float d0[5];
		d0[0] = b->xyz[0] - a->xyz[0];
		d0[1] = b->xyz[1] - a->xyz[1];
		d0[2] = b->xyz[2] - a->xyz[2];
		d0[3] = bST[0] - aST[0];
		d0[4] = bST[1] - aST[1];

		float d1[5];
		d1[0] = c->xyz[0] - a->xyz[0];
		d1[1] = c->xyz[1] - a->xyz[1];
		d1[2] = c->xyz[2] - a->xyz[2];
		d1[3] = cST[0] - aST[0];
		d1[4] = cST[1] - aST[1];

		const float area = d0[3] * d1[4] - d0[4] * d1[3];
		if( fabs( area ) < 1e-20f )
		{
			triangleTangents[i / 3].Zero();
			triangleBitangents[i / 3].Zero();
			c_textureDegenerateFaces++;
			continue;
		}
		if( area > 0.0f )
		{
			c_positive++;
		}
		else
		{
			c_negative++;
		}

#ifdef USE_INVA
		float inva = ( area < 0.0f ) ? -1.0f : 1.0f;		// was = 1.0f / area;

		temp[0] = ( d0[0] * d1[4] - d0[4] * d1[0] ) * inva;
		temp[1] = ( d0[1] * d1[4] - d0[4] * d1[1] ) * inva;
		temp[2] = ( d0[2] * d1[4] - d0[4] * d1[2] ) * inva;
		temp.Normalize();
		triangleTangents[i / 3] = temp;

		temp[0] = ( d0[3] * d1[0] - d0[0] * d1[3] ) * inva;
		temp[1] = ( d0[3] * d1[1] - d0[1] * d1[3] ) * inva;
		temp[2] = ( d0[3] * d1[2] - d0[2] * d1[3] ) * inva;
		temp.Normalize();
		triangleBitangents[i / 3] = temp;
#else
		temp[0] = ( d0[0] * d1[4] - d0[4] * d1[0] );
		temp[1] = ( d0[1] * d1[4] - d0[4] * d1[1] );
		temp[2] = ( d0[2] * d1[4] - d0[4] * d1[2] );
		temp.Normalize();
		triangleTangents[i / 3] = temp;

		temp[0] = ( d0[3] * d1[0] - d0[0] * d1[3] );
		temp[1] = ( d0[3] * d1[1] - d0[1] * d1[3] );
		temp[2] = ( d0[3] * d1[2] - d0[2] * d1[3] );
		temp.Normalize();
		triangleBitangents[i / 3] = temp;
#endif
	}

	idTempArray< idVec3 > vertexTangents( tri->numVerts );
	idTempArray< idVec3 > vertexBitangents( tri->numVerts );

	// clear the tangents
	for( int i = 0; i < tri->numVerts; ++i )
	{
		vertexTangents[i].Zero();
		vertexBitangents[i].Zero();
	}

	// sum up the neighbors
	for( int i = 0; i < tri->numIndexes; i += 3 )
	{
		// for each vertex on this face
		for( int j = 0; j < 3; j++ )
		{
			vertexTangents[tri->indexes[i + j]] += triangleTangents[i / 3];
			vertexBitangents[tri->indexes[i + j]] += triangleBitangents[i / 3];
		}
	}

	// Project the summed vectors onto the normal plane and normalize.
	// The tangent vectors will not necessarily be orthogonal to each
	// other, but they will be orthogonal to the surface normal.
	for( int i = 0; i < tri->numVerts; i++ )
	{
		idVec3 normal = tri->verts[i].GetNormal();
		normal.Normalize();

		vertexTangents[i] -= ( vertexTangents[i] * normal ) * normal;
		vertexTangents[i].Normalize();

		vertexBitangents[i] -= ( vertexBitangents[i] * normal ) * normal;
		vertexBitangents[i].Normalize();
	}

	for( int i = 0; i < tri->numVerts; i++ )
	{
		tri->verts[i].SetTangent( vertexTangents[i] );
		tri->verts[i].SetBiTangent( vertexBitangents[i] );
	}

	tri->tangentsCalculated = true;
}

/*
===================
R_BuildDominantTris

Find the largest triangle that uses each vertex
===================
*/
typedef struct
{
	int		vertexNum;
	int		faceNum;
} indexSort_t;

static int IndexSort( const void* a, const void* b )
{
	if( ( ( indexSort_t* )a )->vertexNum < ( ( indexSort_t* )b )->vertexNum )
	{
		return -1;
	}
	if( ( ( indexSort_t* )a )->vertexNum > ( ( indexSort_t* )b )->vertexNum )
	{
		return 1;
	}
	return 0;
}

void R_BuildDominantTris( srfTriangles_t* tri )
{
	int i, j;
	dominantTri_t* dt;
	const int numIndexes = tri->numIndexes;
	indexSort_t* ind = ( indexSort_t* )R_StaticAlloc( numIndexes * sizeof( indexSort_t ) );
	if( ind == NULL )
	{
		idLib::Error( "Couldn't allocate index sort array" );
		return;
	}

	for( i = 0; i < tri->numIndexes; i++ )
	{
		ind[i].vertexNum = tri->indexes[i];
		ind[i].faceNum = i / 3;
	}
	qsort( ind, tri->numIndexes, sizeof( *ind ), IndexSort );

	R_AllocStaticTriSurfDominantTris( tri, tri->numVerts );
	dt = tri->dominantTris;
	memset( dt, 0, tri->numVerts * sizeof( dt[0] ) );

	for( i = 0; i < numIndexes; i += j )
	{
		float	maxArea = 0;
#pragma warning( disable: 6385 ) // This is simply to get pass a false defect for /analyze -- if you can figure out a better way, please let Shawn know...
		int		vertNum = ind[i].vertexNum;
#pragma warning( default: 6385 )
		for( j = 0; i + j < tri->numIndexes && ind[i + j].vertexNum == vertNum; j++ )
		{
			float		d0[5], d1[5];
			idDrawVert*	a, *b, *c;
			idVec3		normal, tangent, bitangent;

			int	i1 = tri->indexes[ind[i + j].faceNum * 3 + 0];
			int	i2 = tri->indexes[ind[i + j].faceNum * 3 + 1];
			int	i3 = tri->indexes[ind[i + j].faceNum * 3 + 2];

			a = tri->verts + i1;
			b = tri->verts + i2;
			c = tri->verts + i3;

			const idVec2 aST = a->GetTexCoord();
			const idVec2 bST = b->GetTexCoord();
			const idVec2 cST = c->GetTexCoord();

			d0[0] = b->xyz[0] - a->xyz[0];
			d0[1] = b->xyz[1] - a->xyz[1];
			d0[2] = b->xyz[2] - a->xyz[2];
			d0[3] = bST[0] - aST[0];
			d0[4] = bST[1] - aST[1];

			d1[0] = c->xyz[0] - a->xyz[0];
			d1[1] = c->xyz[1] - a->xyz[1];
			d1[2] = c->xyz[2] - a->xyz[2];
			d1[3] = cST[0] - aST[0];
			d1[4] = cST[1] - aST[1];

			normal[0] = ( d1[1] * d0[2] - d1[2] * d0[1] );
			normal[1] = ( d1[2] * d0[0] - d1[0] * d0[2] );
			normal[2] = ( d1[0] * d0[1] - d1[1] * d0[0] );

			float area = normal.Length();

			// if this is smaller than what we already have, skip it
			if( area < maxArea )
			{
				continue;
			}
			maxArea = area;

			if( i1 == vertNum )
			{
				dt[vertNum].v2 = i2;
				dt[vertNum].v3 = i3;
			}
			else if( i2 == vertNum )
			{
				dt[vertNum].v2 = i3;
				dt[vertNum].v3 = i1;
			}
			else
			{
				dt[vertNum].v2 = i1;
				dt[vertNum].v3 = i2;
			}

			float	len = area;
			if( len < 0.001f )
			{
				len = 0.001f;
			}
			dt[vertNum].normalizationScale[2] = 1.0f / len;		// normal

			// texture area
			area = d0[3] * d1[4] - d0[4] * d1[3];

			tangent[0] = ( d0[0] * d1[4] - d0[4] * d1[0] );
			tangent[1] = ( d0[1] * d1[4] - d0[4] * d1[1] );
			tangent[2] = ( d0[2] * d1[4] - d0[4] * d1[2] );
			len = tangent.Length();
			if( len < 0.001f )
			{
				len = 0.001f;
			}
			dt[vertNum].normalizationScale[0] = ( area > 0 ? 1 : -1 ) / len;	// tangents[0]

			bitangent[0] = ( d0[3] * d1[0] - d0[0] * d1[3] );
			bitangent[1] = ( d0[3] * d1[1] - d0[1] * d1[3] );
			bitangent[2] = ( d0[3] * d1[2] - d0[2] * d1[3] );
			len = bitangent.Length();
			if( len < 0.001f )
			{
				len = 0.001f;
			}
#ifdef DERIVE_UNSMOOTHED_BITANGENT
			dt[vertNum].normalizationScale[1] = ( area > 0 ? 1 : -1 );
#else
			dt[vertNum].normalizationScale[1] = ( area > 0 ? 1 : -1 ) / len;	// tangents[1]
#endif
		}
	}

	R_StaticFree( ind );
}

/*
==================
R_DeriveTangents

This is called once for static surfaces, and every frame for deforming surfaces

Builds tangents, normals, and face planes
==================
*/
void R_DeriveTangents( srfTriangles_t* tri )
{
	if( tri->tangentsCalculated )
	{
		return;
	}

#if !defined( DMAP )
	tr.pc.c_tangentIndexes += tri->numIndexes;
#endif

	if( tri->dominantTris != NULL )
	{
		R_DeriveUnsmoothedNormalsAndTangents( tri );
	}
	else
	{
		R_DeriveNormalsAndTangents( tri );
	}
	tri->tangentsCalculated = true;
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 1559-1597 (R_RemoveDegenerateTriangles) ====
/*
=================
R_RemoveDegenerateTriangles

silIndexes must have already been calculated
=================
*/
void R_RemoveDegenerateTriangles( srfTriangles_t* tri )
{
	int		c_removed;
	int		i;
	int		a, b, c;

	assert( tri->silIndexes != NULL );

	// check for completely degenerate triangles
	c_removed = 0;
	for( i = 0; i < tri->numIndexes; i += 3 )
	{
		a = tri->silIndexes[i];
		b = tri->silIndexes[i + 1];
		c = tri->silIndexes[i + 2];
		if( a == b || a == c || b == c )
		{
			c_removed++;
			memmove( tri->indexes + i, tri->indexes + i + 3, ( tri->numIndexes - i - 3 ) * sizeof( tri->indexes[0] ) );
			memmove( tri->silIndexes + i, tri->silIndexes + i + 3, ( tri->numIndexes - i - 3 ) * sizeof( tri->silIndexes[0] ) );
			tri->numIndexes -= 3;
			i -= 3;
		}
	}

	// this doesn't free the memory used by the unused verts

	if( c_removed )
	{
		common->Printf( "removed %i degenerate triangles\n", c_removed );
	}
}
// ==== engine neo/renderer/tr_trisurf.cpp lines 1769-1801 (R_ReverseTriangles) ====
/*
=================
R_ReverseTriangles

Lit two sided surfaces need to have the triangles actually duplicated,
they can't just turn on two sided lighting, because the normal and tangents
are wrong on the other sides.

This should be called before R_CleanupTriangles
=================
*/
void R_ReverseTriangles( srfTriangles_t* tri )
{
	int			i;

	// flip the normal on each vertex
	// If the surface is going to have generated normals, this won't matter,
	// but if it has explicit normals, this will keep it on the correct side
	for( i = 0; i < tri->numVerts; i++ )
	{
		tri->verts[i].SetNormal( vec3_origin - tri->verts[i].GetNormal() );
	}

	// flip the index order to make them back sided
	for( i = 0; i < tri->numIndexes; i += 3 )
	{
		triIndex_t	temp;

		temp = tri->indexes[ i + 0 ];
		tri->indexes[ i + 0 ] = tri->indexes[ i + 1 ];
		tri->indexes[ i + 1 ] = temp;
	}
}
