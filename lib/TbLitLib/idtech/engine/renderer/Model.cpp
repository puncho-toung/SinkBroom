// ==== engine neo/renderer/Model.cpp lines 1-35 (licence and includes (Model_local.h and the other loaders' headers are left out: shim/src/ShimModels.cpp declares idRenderModelStatic)) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2016 Kot in Action Creative Artel
Copyright (C) 2012-2024 Robert Beckebans

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
// ==== engine neo/renderer/Model.cpp lines 37-37 (#include "Model_ase.h") ====
#include "Model_ase.h"
// ==== engine neo/renderer/Model.cpp lines 40-40 (#include "Model_obj.h") ====
#include "Model_obj.h"
// ==== engine neo/renderer/Model.cpp lines 42-45 (r_mergeModelSurfaces, r_slopVertex, r_slopTexCoord, r_slopNormal) ====
idCVar idRenderModelStatic::r_mergeModelSurfaces( "r_mergeModelSurfaces", "1", CVAR_BOOL | CVAR_RENDERER, "combine model surfaces with the same material" );
idCVar idRenderModelStatic::r_slopVertex( "r_slopVertex", "0.01", CVAR_RENDERER, "merge xyz coordinates this far apart" );
idCVar idRenderModelStatic::r_slopTexCoord( "r_slopTexCoord", "0.001", CVAR_RENDERER, "merge texture coordinates this far apart" );
idCVar idRenderModelStatic::r_slopNormal( "r_slopNormal", "0.02", CVAR_RENDERER, "merge normals that dot less than this" );
// ==== engine neo/renderer/Model.cpp lines 54-90 (idRenderModelStatic::idRenderModelStatic, ~idRenderModelStatic) ====
/*
================
idRenderModelStatic::idRenderModelStatic
================
*/
idRenderModelStatic::idRenderModelStatic()
{
	name = "<undefined>";
	bounds.Clear();
	lastModifiedFrame = 0;
	lastArchivedFrame = 0;
	overlaysAdded = 0;
	isStaticWorldModel = false;
	defaulted = false;
	purged = false;
	fastLoad = false;
	reloadable = true;
	levelLoadReferenced = false;
	hasDrawingSurfaces = true;
	hasInteractingSurfaces = true;
	hasShadowCastingSurfaces = true;
	timeStamp = 0;
	declTimeStamp = 0;
	numInvertedJoints = 0;
	jointsInverted = NULL;
	jointsInvertedBuffer = 0;
}

/*
================
idRenderModelStatic::~idRenderModelStatic
================
*/
idRenderModelStatic::~idRenderModelStatic()
{
	PurgeModel();
}
// ==== engine neo/renderer/Model.cpp lines 221-296 (AddCubeFace, idRenderModelStatic::MakeDefaultModel) ====
/*
================
AddCubeFace
================
*/
static void AddCubeFace( srfTriangles_t* tri, idVec3 v1, idVec3 v2, idVec3 v3, idVec3 v4 )
{
	tri->verts[tri->numVerts + 0].Clear();
	tri->verts[tri->numVerts + 0].xyz = v1 * 8;
	tri->verts[tri->numVerts + 0].SetTexCoord( 0, 0 );

	tri->verts[tri->numVerts + 1].Clear();
	tri->verts[tri->numVerts + 1].xyz = v2 * 8;
	tri->verts[tri->numVerts + 1].SetTexCoord( 1, 0 );

	tri->verts[tri->numVerts + 2].Clear();
	tri->verts[tri->numVerts + 2].xyz = v3 * 8;
	tri->verts[tri->numVerts + 2].SetTexCoord( 1, 1 );

	tri->verts[tri->numVerts + 3].Clear();
	tri->verts[tri->numVerts + 3].xyz = v4 * 8;
	tri->verts[tri->numVerts + 3].SetTexCoord( 0, 1 );

	tri->indexes[tri->numIndexes + 0] = tri->numVerts + 0;
	tri->indexes[tri->numIndexes + 1] = tri->numVerts + 1;
	tri->indexes[tri->numIndexes + 2] = tri->numVerts + 2;
	tri->indexes[tri->numIndexes + 3] = tri->numVerts + 0;
	tri->indexes[tri->numIndexes + 4] = tri->numVerts + 2;
	tri->indexes[tri->numIndexes + 5] = tri->numVerts + 3;

	tri->numVerts += 4;
	tri->numIndexes += 6;
}

/*
================
idRenderModelStatic::MakeDefaultModel
================
*/
void idRenderModelStatic::MakeDefaultModel()
{

	defaulted = true;

	// throw out any surfaces we already have
	PurgeModel();

	// create one new surface
	modelSurface_t	surf;

	srfTriangles_t* tri = R_AllocStaticTriSurf();

#if defined( DMAP )
	surf.shader = declManager->FindMaterial( "_default", false );
#else
	surf.shader = tr.defaultMaterial;
#endif
	surf.geometry = tri;

	R_AllocStaticTriSurfVerts( tri, 24 );
	R_AllocStaticTriSurfIndexes( tri, 36 );

	AddCubeFace( tri, idVec3( -1, 1, 1 ), idVec3( 1, 1, 1 ), idVec3( 1, -1, 1 ), idVec3( -1, -1, 1 ) );
	AddCubeFace( tri, idVec3( -1, 1, -1 ), idVec3( -1, -1, -1 ), idVec3( 1, -1, -1 ), idVec3( 1, 1, -1 ) );

	AddCubeFace( tri, idVec3( 1, -1, 1 ), idVec3( 1, 1, 1 ), idVec3( 1, 1, -1 ), idVec3( 1, -1, -1 ) );
	AddCubeFace( tri, idVec3( -1, -1, 1 ), idVec3( -1, -1, -1 ), idVec3( -1, 1, -1 ), idVec3( -1, 1, 1 ) );

	AddCubeFace( tri, idVec3( -1, -1, 1 ), idVec3( 1, -1, 1 ), idVec3( 1, -1, -1 ), idVec3( -1, -1, -1 ) );
	AddCubeFace( tri, idVec3( -1, 1, 1 ), idVec3( -1, 1, -1 ), idVec3( 1, 1, -1 ), idVec3( 1, 1, 1 ) );

	tri->generateNormals = true;

	AddSurface( surf );
	FinishSurfaces( false );
}
// ==== engine neo/renderer/Model.cpp lines 910-949 (idRenderModelStatic::InitEmpty, AddSurface) ====
/*
================
idRenderModelStatic::InitEmpty
================
*/
void idRenderModelStatic::InitEmpty( const char* fileName )
{
	// model names of the form _area* are static parts of the
	// world, and have already been considered for optimized shadows
	// other model names are inline entity models, and need to be
	// shadowed normally
	if( !idStr::Cmpn( fileName, "_area", 5 ) )
	{
		isStaticWorldModel = true;
	}
	else
	{
		isStaticWorldModel = false;
	}

	name = fileName;
	reloadable = false;	// if it didn't come from a file, we can't reload it
	PurgeModel();
	purged = false;
	bounds.Zero();
}

/*
================
idRenderModelStatic::AddSurface
================
*/
void idRenderModelStatic::AddSurface( modelSurface_t surface )
{
	surfaces.Append( surface );
	if( surface.geometry )
	{
		bounds += surface.geometry->bounds;
	}
}
// ==== engine neo/renderer/Model.cpp lines 983-991 (idRenderModelStatic::NumSurfaces) ====
/*
================
idRenderModelStatic::NumSurfaces
================
*/
int idRenderModelStatic::NumSurfaces() const
{
	return surfaces.Num();
}
// ==== engine neo/renderer/Model.cpp lines 1167-1367 (idRenderModelStatic::FinishSurfaces) ====
/*
================
idRenderModelStatic::FinishSurfaces

The mergeShadows option allows surfaces with different textures to share
silhouette edges for shadow calculation, instead of leaving shared edges
hanging.

If any of the original shaders have the noSelfShadow flag set, the surfaces
can't be merged, because they will need to be drawn in different order.

If there is only one surface, a separate merged surface won't be generated.

A model with multiple surfaces can't later have a skinned shader change the
state of the noSelfShadow flag.

-----------------

Creates mirrored copies of two sided surfaces with normal maps, which would
otherwise light funny.

Extends the bounds of deformed surfaces so they don't cull incorrectly at screen edges.

================
*/
void idRenderModelStatic::FinishSurfaces( bool useMikktspace )
{
	int			i;
	int			totalVerts, totalIndexes;

	hasDrawingSurfaces = false;
	hasInteractingSurfaces = false;
	hasShadowCastingSurfaces = false;
	purged = false;

	// make sure we don't have a huge bounds even if we don't finish everything
	bounds.Zero();


	if( surfaces.Num() == 0 )
	{
		return;
	}

	// renderBump doesn't care about most of this
	if( fastLoad )
	{
		bounds.Zero();
		for( i = 0; i < surfaces.Num(); i++ )
		{
			const modelSurface_t*	surf = &surfaces[i];

			R_BoundTriSurf( surf->geometry );
			bounds.AddBounds( surf->geometry->bounds );
		}

		return;
	}

	// cleanup all the final surfaces, but don't create sil edges
	totalVerts = 0;
	totalIndexes = 0;

	// decide if we are going to merge all the surfaces into one shadower
	int	numOriginalSurfaces = surfaces.Num();

	// make sure there aren't any NULL shaders or geometry
	for( i = 0; i < numOriginalSurfaces; i++ )
	{
		const modelSurface_t*	surf = &surfaces[i];

		if( surf->geometry == NULL || surf->shader == NULL )
		{
			MakeDefaultModel();
			common->Error( "Model %s, surface %i had NULL geometry", name.c_str(), i );
		}
		if( surf->shader == NULL )
		{
			MakeDefaultModel();
			common->Error( "Model %s, surface %i had NULL shader", name.c_str(), i );
		}
	}

	// duplicate and reverse triangles for two sided bump mapped surfaces
	// note that this won't catch surfaces that have their shaders dynamically
	// changed, and won't work with animated models.
	// It is better to create completely separate surfaces, rather than
	// add vertexes and indexes to the existing surface, because the
	// tangent generation wouldn't like the acute shared edges
	for( i = 0; i < numOriginalSurfaces; i++ )
	{
		const modelSurface_t*	surf = &surfaces[i];

		if( surf->shader->ShouldCreateBackSides() )
		{
			srfTriangles_t* newTri;

			newTri = R_CopyStaticTriSurf( surf->geometry );
			R_ReverseTriangles( newTri );

			modelSurface_t	newSurf;

			newSurf.shader = surf->shader;
			newSurf.geometry = newTri;

			AddSurface( newSurf );
		}
	}

	// clean the surfaces
	for( i = 0; i < surfaces.Num(); i++ )
	{
		const modelSurface_t*	surf = &surfaces[i];

		bool mikktspace = useMikktspace || surf->shader->UseMikkTSpace();

		R_CleanupTriangles( surf->geometry, surf->geometry->generateNormals, true, surf->shader->UseUnsmoothedTangents(), mikktspace );
		if( surf->shader->SurfaceCastsShadow() )
		{
			totalVerts += surf->geometry->numVerts;
			totalIndexes += surf->geometry->numIndexes;
		}
	}

	// add up the total surface area for development information
	for( i = 0; i < surfaces.Num(); i++ )
	{
		const modelSurface_t*	surf = &surfaces[i];
		srfTriangles_t*	tri = surf->geometry;

		for( int j = 0; j < tri->numIndexes; j += 3 )
		{
			float	area = idWinding::TriangleArea( tri->verts[tri->indexes[j]].xyz,
													tri->verts[tri->indexes[j + 1]].xyz,  tri->verts[tri->indexes[j + 2]].xyz );
			const_cast<idMaterial*>( surf->shader )->AddToSurfaceArea( area );
		}
	}

	// set flags for whole-model rejection
	for( i = 0; i < surfaces.Num(); i++ )
	{
		const modelSurface_t*	surf = &surfaces[i];
		if( surf->shader->IsDrawn() )
		{
			hasDrawingSurfaces = true;
		}
		if( surf->shader->SurfaceCastsShadow() )
		{
			hasShadowCastingSurfaces = true;
		}
		if( surf->shader->ReceivesLighting() )
		{
			hasInteractingSurfaces = true;
		}
		if( strstr( surf->shader->GetName(), "trigger" ) )
		{
			static int breakHere;
			breakHere++;
		}
	}

	// calculate the bounds
	if( surfaces.Num() == 0 )
	{
		bounds.Zero();
	}
	else
	{
		bounds.Clear();
		for( i = 0; i < surfaces.Num(); i++ )
		{
			modelSurface_t*	surf = &surfaces[i];

			// if the surface has a deformation, increase the bounds
			// the amount here is somewhat arbitrary, designed to handle
			// autosprites and flares, but could be done better with exact
			// deformation information.
			// Note that this doesn't handle deformations that are skinned in
			// at run time...
			if( surf->shader->Deform() != DFRM_NONE )
			{
				srfTriangles_t*	tri = surf->geometry;
				idVec3	mid = ( tri->bounds[1] + tri->bounds[0] ) * 0.5f;
				float	radius = ( tri->bounds[0] - mid ).Length();
				radius += 20.0f;

				tri->bounds[0][0] = mid[0] - radius;
				tri->bounds[0][1] = mid[1] - radius;
				tri->bounds[0][2] = mid[2] - radius;

				tri->bounds[1][0] = mid[0] + radius;
				tri->bounds[1][1] = mid[1] + radius;
				tri->bounds[1][2] = mid[2] + radius;
			}

			// add to the model bounds
			bounds.AddBounds( surf->geometry->bounds );

		}
	}
}
// ==== engine neo/renderer/Model.cpp lines 1370-1733 (matchVert_t, idRenderModelStatic::ConvertOBJToModelSurfaces) ====
typedef struct matchVert_s
{
	struct matchVert_s*	next;
	int		v, tv;
	byte	color[4];
	idVec3	normal;
} matchVert_t;

/*
=================
idRenderModelStatic::ConvertOBJToModelSurfaces
=================
*/
bool idRenderModelStatic::ConvertOBJToModelSurfaces( const objModel_t* model )
{
	objObject_t* 	mesh;
	const idMaterial* im1, *im2;
	srfTriangles_t* tri;
	int				objectNum;
	int				i, j;
	int				v, tv;
	int* 			vRemap;
	int* 			tvRemap;
	matchVert_t* 	mvTable;	// all of the match verts
	matchVert_t** 	mvHash;		// points inside mvTable for each xyz index
	matchVert_t* 	lastmv;
	matchVert_t* 	mv;
	idVec3			normal;
	int* 			mergeTo;
	byte* 			color;
	static byte	identityColor[4] = { 255, 255, 255, 255 };
	modelSurface_t	surf, *modelSurf;

	if( !model )
	{
		return false;
	}
	if( model->objects.Num() < 1 )
	{
		return false;
	}

	timeStamp = model->timeStamp;

	// the modeling programs can save out multiple surfaces with a common
	// material, but we would like to mege them together where possible
	// meaning that this->NumSurfaces() <= ase->objects.currentElements
	mergeTo = ( int* )_alloca( model->objects.Num() * sizeof( *mergeTo ) );
	surf.geometry = NULL;
	/*
	if( model->materials.Num() == 0 )
	{
		// if we don't have any materials, dump everything into a single surface
		surf.shader = tr.defaultMaterial;
		surf.id = 0;
		this->AddSurface( surf );
		for( i = 0; i < ase->objects.Num(); i++ )
		{
			mergeTo[i] = 0;
		}
	}
	else
	*/
	if( !r_mergeModelSurfaces.GetBool() )
	{
		// don't merge any
		for( i = 0; i < model->objects.Num(); i++ )
		{
			mergeTo[i] = i;
			mesh = model->objects[i];

			surf.shader = declManager->FindMaterial( mesh->material );
			surf.id = this->NumSurfaces();
			this->AddSurface( surf );
		}
	}
	else
	{
		// search for material matches
		for( i = 0; i < model->objects.Num(); i++ )
		{
			mesh = model->objects[i];

			im1 = declManager->FindMaterial( mesh->material );
			if( im1->IsDiscrete() )
			{
				// flares, autosprites, etc
				j = this->NumSurfaces();
			}
			else
			{
				for( j = 0; j < this->NumSurfaces(); j++ )
				{
					modelSurf = &this->surfaces[j];
					im2 = modelSurf->shader;
					if( im1 == im2 )
					{
						// merge this
						mergeTo[i] = j;
						break;
					}
				}
			}
			if( j == this->NumSurfaces() )
			{
				// didn't merge
				mergeTo[i] = j;
				surf.shader = im1;
				surf.id = this->NumSurfaces();
				this->AddSurface( surf );
			}
		}
	}

	idVectorSubset<idVec3, 3> vertexSubset;
	idVectorSubset<idVec2, 2> texCoordSubset;

	// build the surfaces
	for( objectNum = 0; objectNum < model->objects.Num(); objectNum++ )
	{
		mesh = model->objects[objectNum];

		im1 = declManager->FindMaterial( mesh->material );

		bool normalsParsed = mesh->normals.Num();

		// completely ignore any explict normals on surfaces with a renderbump command
		// which will guarantee the best contours and least vertexes.
		const char* rb = im1->GetRenderBump();
		if( rb != NULL && rb[0] != '\0' )
		{
			normalsParsed = false;
		}

		// It seems like the tools our artists are using often generate
		// verts and texcoords slightly separated that should be merged
		// note that we really should combine the surfaces with common materials
		// before doing this operation, because we can miss a slop combination
		// if they are in different surfaces

		vRemap = ( int* )R_StaticAlloc( mesh->vertexes.Num() * sizeof( vRemap[0] ), TAG_MODEL );

		if( fastLoad )
		{
			// renderbump doesn't care about vertex count
			for( j = 0; j < mesh->vertexes.Num(); j++ )
			{
				vRemap[j] = j;
			}
		}
		else
		{
			float vertexEpsilon = r_slopVertex.GetFloat();
			float expand = 2 * 32 * vertexEpsilon;
			idVec3 mins, maxs;

			SIMDProcessor->MinMax( mins, maxs, &mesh->vertexes[0], mesh->vertexes.Num() );

			mins -= idVec3( expand, expand, expand );
			maxs += idVec3( expand, expand, expand );

			vertexSubset.Init( mins, maxs, 32, 1024 );
			for( j = 0; j < mesh->vertexes.Num(); j++ )
			{
				vRemap[j] = vertexSubset.FindVector( &mesh->vertexes[0], j, vertexEpsilon );
			}
		}

		tvRemap = ( int* )R_StaticAlloc( mesh->texcoords.Num() * sizeof( tvRemap[0] ), TAG_MODEL );

		if( fastLoad )
		{
			// renderbump doesn't care about vertex count
			for( j = 0; j < mesh->texcoords.Num(); j++ )
			{
				tvRemap[j] = j;
			}
		}
		else
		{
			float texCoordEpsilon = r_slopTexCoord.GetFloat();
			float expand = 2 * 32 * texCoordEpsilon;
			idVec2 mins, maxs;

			SIMDProcessor->MinMax( mins, maxs, &mesh->texcoords[0], mesh->texcoords.Num() );

			mins -= idVec2( expand, expand );
			maxs += idVec2( expand, expand );

			texCoordSubset.Init( mins, maxs, 32, 1024 );

			for( j = 0; j < mesh->texcoords.Num(); j++ )
			{
				tvRemap[j] = texCoordSubset.FindVector( &mesh->texcoords[0], j, texCoordEpsilon );
			}
		}

		// we need to find out how many unique vertex / texcoord combinations
		// there are, because ASE tracks them separately but we need them unified

		// the maximum possible number of combined vertexes is the number of indexes
		mvTable = ( matchVert_t* )R_ClearedStaticAlloc( mesh->indexes.Num() * sizeof( mvTable[0] ) );

		// we will have a hash chain based on the xyz values
		mvHash = ( matchVert_t** )R_ClearedStaticAlloc( mesh->vertexes.Num() * sizeof( mvHash[0] ) );

		// allocate triangle surface
		tri = R_AllocStaticTriSurf();
		tri->numVerts = 0;
		tri->numIndexes = 0;
		R_AllocStaticTriSurfIndexes( tri, mesh->indexes.Num() );
		tri->generateNormals = !normalsParsed;

		// init default normal, color and tex coord index
		normal.Zero();
		color = identityColor;
		tv = 0;

		// find all the unique combinations
		float normalEpsilon = 1.0f - r_slopNormal.GetFloat();

		for( j = 0; j < ( mesh->indexes.Num() / 3 ); j++ )
		{
			// construct triangles in reverse order
			for( int k = 2; k >= 0; k-- )
			{
				int index = j * 3 + k;

				if( index < 0 || index >= mesh->vertexes.Num() )
				{
					common->Error( "ConvertOBJToModelSurfaces: bad vertex index in ASE file %s", name.c_str() );
				}

				// collapse the position if it was slightly offset
				v = vRemap[index];

				// we may or may not have texcoords to compare
				if( mesh->texcoords.Num() != 0 )
				{
					tv = index;
					//tv = mesh->faces[j].tVertexNum[k];

					if( tv < 0 || tv >= mesh->texcoords.Num() )
					{
						common->Error( "ConvertOBJToModelSurfaces: bad tex coord index in ASE file %s", name.c_str() );
					}
					// collapse the tex coord if it was slightly offset
					tv = tvRemap[tv];
				}

				// we may or may not have normals to compare
				if( normalsParsed )
				{
					normal = mesh->normals[ index ];
				}

				// we may or may not have colors to compare
				/*
				if( mesh->colorsParsed )
				{
					color = mesh->faces[j].vertexColors[k];
				}
				*/

				// find a matching vert
				for( lastmv = NULL, mv = mvHash[v]; mv != NULL; lastmv = mv, mv = mv->next )
				{
					if( mv->tv != tv )
					{
						continue;
					}
					if( *( unsigned* )mv->color != *( unsigned* )color )
					{
						continue;
					}
					if( !normalsParsed )
					{
						// if we are going to create the normals, just
						// matching texcoords is enough
						break;
					}
					if( mv->normal * normal > normalEpsilon )
					{
						break;		// we already have this one
					}
				}
				if( !mv )
				{
					// allocate a new match vert and link to hash chain
					mv = &mvTable[ tri->numVerts ];
					mv->v = v;
					mv->tv = tv;
					mv->normal = normal;
					*( unsigned* )mv->color = *( unsigned* )color;
					mv->next = NULL;
					if( lastmv )
					{
						lastmv->next = mv;
					}
					else
					{
						mvHash[v] = mv;
					}
					tri->numVerts++;
				}

				tri->indexes[tri->numIndexes] = mv - mvTable;
				tri->numIndexes++;
			}
		}

		// allocate space for the indexes and copy them
		if( tri->numIndexes > mesh->indexes.Num() )
		{
			common->FatalError( "ConvertOBJToModelSurfaces: index miscount in OBJ file %s", name.c_str() );
		}
		if( tri->numVerts > mesh->indexes.Num() )
		{
			common->FatalError( "ConvertOBJToModelSurfaces: vertex miscount in OBJ file %s", name.c_str() );
		}

		// now allocate and generate the combined vertexes
		R_AllocStaticTriSurfVerts( tri, tri->numVerts );

		for( j = 0; j < tri->numVerts; j++ )
		{
			mv = &mvTable[j];
			tri->verts[ j ].Clear();
			tri->verts[ j ].xyz = mesh->vertexes[ mv->v ];
			tri->verts[ j ].SetNormal( mv->normal );

			*( unsigned* )tri->verts[j].color = *( unsigned* )mv->color;

			if( mesh->texcoords.Num() != 0 )
			{
				const idVec2& tv = mesh->texcoords[ mv->tv ];
				float u = tv.x;
				float v = tv.y;
				tri->verts[j].SetTexCoord( u, v );
			}
		}

		R_StaticFree( mvTable );
		R_StaticFree( mvHash );
		R_StaticFree( tvRemap );
		R_StaticFree( vRemap );

		// see if we need to merge with a previous surface of the same material
		modelSurf = &this->surfaces[mergeTo[ objectNum ]];
		srfTriangles_t*	mergeTri = modelSurf->geometry;
		if( !mergeTri )
		{
			modelSurf->geometry = tri;
		}
		else
		{
			modelSurf->geometry = R_MergeTriangles( mergeTri, tri );
			R_FreeStaticTriSurf( tri );
			R_FreeStaticTriSurf( mergeTri );
		}
	}

	return true;
}
// ==== engine neo/renderer/Model.cpp lines 1736-2107 (idRenderModelStatic::ConvertASEToModelSurfaces) ====
/*
=================
idRenderModelStatic::ConvertASEToModelSurfaces
=================
*/
bool idRenderModelStatic::ConvertASEToModelSurfaces( const struct aseModel_s* ase )
{
	aseObject_t* 	object;
	aseMesh_t* 		mesh;
	aseMaterial_t* 	material;
	const idMaterial* im1, *im2;
	srfTriangles_t* tri;
	int				objectNum;
	int				i, j, k;
	int				v, tv;
	int* 			vRemap;
	int* 			tvRemap;
	matchVert_t* 	mvTable;	// all of the match verts
	matchVert_t** 	mvHash;		// points inside mvTable for each xyz index
	matchVert_t* 	lastmv;
	matchVert_t* 	mv;
	idVec3			normal;
	float			uOffset, vOffset, textureSin, textureCos;
	float			uTiling, vTiling;
	int* 			mergeTo;
	byte* 			color;
	static byte	identityColor[4] = { 255, 255, 255, 255 };
	modelSurface_t	surf, *modelSurf;

	if( !ase )
	{
		return false;
	}
	if( ase->objects.Num() < 1 )
	{
		return false;
	}

	timeStamp = ase->timeStamp;

	// the modeling programs can save out multiple surfaces with a common
	// material, but we would like to mege them together where possible
	// meaning that this->NumSurfaces() <= ase->objects.currentElements
	mergeTo = ( int* )_alloca( ase->objects.Num() * sizeof( *mergeTo ) );
	surf.geometry = NULL;
	if( ase->materials.Num() == 0 )
	{
		// if we don't have any materials, dump everything into a single surface
#if defined( DMAP )
		surf.shader = declManager->FindMaterial( "_default", false );
#else
		surf.shader = tr.defaultMaterial;
#endif
		surf.id = 0;
		this->AddSurface( surf );
		for( i = 0; i < ase->objects.Num(); i++ )
		{
			mergeTo[i] = 0;
		}
	}
	else if( !r_mergeModelSurfaces.GetBool() )
	{
		// don't merge any
		for( i = 0; i < ase->objects.Num(); i++ )
		{
			mergeTo[i] = i;
			object = ase->objects[i];
			material = ase->materials[object->materialRef];
			surf.shader = declManager->FindMaterial( material->name );
			surf.id = this->NumSurfaces();
			this->AddSurface( surf );
		}
	}
	else
	{
		// search for material matches
		for( i = 0; i < ase->objects.Num(); i++ )
		{
			object = ase->objects[i];
			material = ase->materials[object->materialRef];
			im1 = declManager->FindMaterial( material->name );
			if( im1->IsDiscrete() )
			{
				// flares, autosprites, etc
				j = this->NumSurfaces();
			}
			else
			{
				for( j = 0; j < this->NumSurfaces(); j++ )
				{
					modelSurf = &this->surfaces[j];
					im2 = modelSurf->shader;
					if( im1 == im2 )
					{
						// merge this
						mergeTo[i] = j;
						break;
					}
				}
			}
			if( j == this->NumSurfaces() )
			{
				// didn't merge
				mergeTo[i] = j;
				surf.shader = im1;
				surf.id = this->NumSurfaces();
				this->AddSurface( surf );
			}
		}
	}

	idVectorSubset<idVec3, 3> vertexSubset;
	idVectorSubset<idVec2, 2> texCoordSubset;

	// build the surfaces
	for( objectNum = 0; objectNum < ase->objects.Num(); objectNum++ )
	{
		object = ase->objects[objectNum];
		mesh = &object->mesh;
		//material = ase->materials[object->materialRef];
		//im1 = declManager->FindMaterial( material->name );
		//
		// caedes dhewm3 fix for ASE meshes without materials (a lot of Doom 3 mods have this issue) 05-18-2021
		material = ( ase->materials.Num() > object->materialRef ) ? ase->materials[object->materialRef] : NULL;
		im1 = declManager->FindMaterial( material ? material->name : NULL );

		bool normalsParsed = mesh->normalsParsed;

		// completely ignore any explict normals on surfaces with a renderbump command
		// which will guarantee the best contours and least vertexes.
		const char* rb = im1->GetRenderBump();
		if( rb != NULL && rb[0] != '\0' )
		{
			normalsParsed = false;
		}

		// It seems like the tools our artists are using often generate
		// verts and texcoords slightly separated that should be merged
		// note that we really should combine the surfaces with common materials
		// before doing this operation, because we can miss a slop combination
		// if they are in different surfaces

		vRemap = ( int* )R_StaticAlloc( mesh->numVertexes * sizeof( vRemap[0] ), TAG_MODEL );

		if( fastLoad )
		{
			// renderbump doesn't care about vertex count
			for( j = 0; j < mesh->numVertexes; j++ )
			{
				vRemap[j] = j;
			}
		}
		else
		{
			float vertexEpsilon = r_slopVertex.GetFloat();
			float expand = 2 * 32 * vertexEpsilon;
			idVec3 mins, maxs;

			SIMDProcessor->MinMax( mins, maxs, mesh->vertexes, mesh->numVertexes );
			mins -= idVec3( expand, expand, expand );
			maxs += idVec3( expand, expand, expand );
			vertexSubset.Init( mins, maxs, 32, 1024 );
			for( j = 0; j < mesh->numVertexes; j++ )
			{
				vRemap[j] = vertexSubset.FindVector( mesh->vertexes, j, vertexEpsilon );
			}
		}

		tvRemap = ( int* )R_StaticAlloc( mesh->numTVertexes * sizeof( tvRemap[0] ), TAG_MODEL );

		if( fastLoad )
		{
			// renderbump doesn't care about vertex count
			for( j = 0; j < mesh->numTVertexes; j++ )
			{
				tvRemap[j] = j;
			}
		}
		else
		{
			float texCoordEpsilon = r_slopTexCoord.GetFloat();
			float expand = 2 * 32 * texCoordEpsilon;
			idVec2 mins, maxs;

			SIMDProcessor->MinMax( mins, maxs, mesh->tvertexes, mesh->numTVertexes );
			mins -= idVec2( expand, expand );
			maxs += idVec2( expand, expand );
			texCoordSubset.Init( mins, maxs, 32, 1024 );
			for( j = 0; j < mesh->numTVertexes; j++ )
			{
				tvRemap[j] = texCoordSubset.FindVector( mesh->tvertexes, j, texCoordEpsilon );
			}
		}

		// we need to find out how many unique vertex / texcoord combinations
		// there are, because ASE tracks them separately but we need them unified

		// the maximum possible number of combined vertexes is the number of indexes
		mvTable = ( matchVert_t* )R_ClearedStaticAlloc( mesh->numFaces * 3 * sizeof( mvTable[0] ) );

		// we will have a hash chain based on the xyz values
		mvHash = ( matchVert_t** )R_ClearedStaticAlloc( mesh->numVertexes * sizeof( mvHash[0] ) );

		// allocate triangle surface
		tri = R_AllocStaticTriSurf();
		tri->numVerts = 0;
		tri->numIndexes = 0;
		R_AllocStaticTriSurfIndexes( tri, mesh->numFaces * 3 );
		tri->generateNormals = !normalsParsed;

		// init default normal, color and tex coord index
		normal.Zero();
		color = identityColor;
		tv = 0;

		// find all the unique combinations
		float normalEpsilon = 1.0f - r_slopNormal.GetFloat();
		for( j = 0; j < mesh->numFaces; j++ )
		{
			for( k = 0; k < 3; k++ )
			{
				v = mesh->faces[j].vertexNum[k];

				if( v < 0 || v >= mesh->numVertexes )
				{
					common->Error( "ConvertASEToModelSurfaces: bad vertex index in ASE file %s", name.c_str() );
				}

				// collapse the position if it was slightly offset
				v = vRemap[v];

				// we may or may not have texcoords to compare
				if( mesh->numTVFaces == mesh->numFaces && mesh->numTVertexes != 0 )
				{
					tv = mesh->faces[j].tVertexNum[k];
					if( tv < 0 || tv >= mesh->numTVertexes )
					{
						common->Error( "ConvertASEToModelSurfaces: bad tex coord index in ASE file %s", name.c_str() );
					}
					// collapse the tex coord if it was slightly offset
					tv = tvRemap[tv];
				}

				// we may or may not have normals to compare
				if( normalsParsed )
				{
					normal = mesh->faces[j].vertexNormals[k];
				}

				// we may or may not have colors to compare
				if( mesh->colorsParsed )
				{
					color = mesh->faces[j].vertexColors[k];
				}

				// find a matching vert
				for( lastmv = NULL, mv = mvHash[v]; mv != NULL; lastmv = mv, mv = mv->next )
				{
					if( mv->tv != tv )
					{
						continue;
					}
					if( *( unsigned* )mv->color != *( unsigned* )color )
					{
						continue;
					}
					if( !normalsParsed )
					{
						// if we are going to create the normals, just
						// matching texcoords is enough
						break;
					}
					if( mv->normal * normal > normalEpsilon )
					{
						break;		// we already have this one
					}
				}
				if( !mv )
				{
					// allocate a new match vert and link to hash chain
					mv = &mvTable[ tri->numVerts ];
					mv->v = v;
					mv->tv = tv;
					mv->normal = normal;
					*( unsigned* )mv->color = *( unsigned* )color;
					mv->next = NULL;
					if( lastmv )
					{
						lastmv->next = mv;
					}
					else
					{
						mvHash[v] = mv;
					}
					tri->numVerts++;
				}

				tri->indexes[tri->numIndexes] = mv - mvTable;
				tri->numIndexes++;
			}
		}

		// allocate space for the indexes and copy them
		if( tri->numIndexes > mesh->numFaces * 3 )
		{
			common->FatalError( "ConvertASEToModelSurfaces: index miscount in ASE file %s", name.c_str() );
		}
		if( tri->numVerts > mesh->numFaces * 3 )
		{
			common->FatalError( "ConvertASEToModelSurfaces: vertex miscount in ASE file %s", name.c_str() );
		}

		// an ASE allows the texture coordinates to be scaled, translated, and rotated
		if( ase->materials.Num() == 0 )
		{
			uOffset = vOffset = 0.0f;
			uTiling = vTiling = 1.0f;
			textureSin = 0.0f;
			textureCos = 1.0f;
		}
		else
		{
			material = ase->materials[object->materialRef];
			uOffset = -material->uOffset;
			vOffset = material->vOffset;
			uTiling = material->uTiling;
			vTiling = material->vTiling;
			textureSin = idMath::Sin( material->angle );
			textureCos = idMath::Cos( material->angle );
		}

		// now allocate and generate the combined vertexes
		R_AllocStaticTriSurfVerts( tri, tri->numVerts );

		for( j = 0; j < tri->numVerts; j++ )
		{
			mv = &mvTable[j];
			tri->verts[ j ].Clear();
			tri->verts[ j ].xyz = mesh->vertexes[ mv->v ];
			tri->verts[ j ].SetNormal( mv->normal );
			*( unsigned* )tri->verts[j].color = *( unsigned* )mv->color;
			if( mesh->numTVFaces == mesh->numFaces && mesh->numTVertexes != 0 )
			{
				const idVec2& tv = mesh->tvertexes[ mv->tv ];
				float u = tv.x * uTiling + uOffset;
				float v = tv.y * vTiling + vOffset;
				tri->verts[j].SetTexCoord( u * textureCos + v * textureSin, u * -textureSin + v * textureCos );
			}
		}

		R_StaticFree( mvTable );
		R_StaticFree( mvHash );
		R_StaticFree( tvRemap );
		R_StaticFree( vRemap );

		// see if we need to merge with a previous surface of the same material
		modelSurf = &this->surfaces[mergeTo[ objectNum ]];
		srfTriangles_t*	mergeTri = modelSurf->geometry;
		if( !mergeTri )
		{
			modelSurf->geometry = tri;
		}
		else
		{
			modelSurf->geometry = R_MergeTriangles( mergeTri, tri );
			R_FreeStaticTriSurf( tri );
			R_FreeStaticTriSurf( mergeTri );
		}
	}

	return true;
}
// ==== engine neo/renderer/Model.cpp lines 3234-3259 (idRenderModelStatic::PurgeModel) ====
/*
================
idRenderModelStatic::PurgeModel
================
*/
void idRenderModelStatic::PurgeModel()
{
	for( int i = 0; i < surfaces.Num(); i++ )
	{
		modelSurface_t* surf = &surfaces[i];

		if( surf->geometry )
		{
			R_FreeStaticTriSurf( surf->geometry );
		}
	}
	surfaces.Clear();

	if( jointsInverted != NULL )
	{
		Mem_Free( jointsInverted );
		jointsInverted = NULL;
	}

	purged = true;
}
