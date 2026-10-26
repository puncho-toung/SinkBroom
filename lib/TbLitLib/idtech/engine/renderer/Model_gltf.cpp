// ==== engine neo/renderer/Model_gltf.cpp lines 1-31 (licence, precompiled.h (Model_gltf.h, Model_local.h, RenderCommon.h and the d3xp headers left out: shim/src/ShimGltf.cpp declares idRenderModelGLTF)) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2022-2023 Harrie van Ginneken
Copyright (C) 2022-2025 Robert Beckebans

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
// ==== engine neo/renderer/Model_gltf.cpp lines 44-57 (the cvars and the statics (GLMB_MAGIC, blenderToDoomTransform, lastMeshFromFile)) ====
idCVar r_useCachedDynamicModels( "r_useCachedDynamicModels", "1", CVAR_RENDERER | CVAR_BOOL, "cache snapshots of dynamic models" );

idCVar gltf_forceBspMeshTexture( "gltf_forceBspMeshTexture", "0", CVAR_SYSTEM | CVAR_BOOL, "all world geometry has the same forced texture" );
idCVar gltf_modelSceneName( "gltf_modelSceneName", "Scene", CVAR_SYSTEM | CVAR_NEW, "Scene to use when loading specific models" );

idCVar gltf_animSampleRate( "gltf_animSampleRate", "24", CVAR_SYSTEM | CVAR_INTEGER | CVAR_NEW, "The frame rate of the converted md5anim" );


static const byte GLMB_VERSION = 106;
static const unsigned int GLMB_MAGIC = ( 'M' << 24 ) | ( 'L' << 16 ) | ( 'G' << 8 ) | GLMB_VERSION;
static const char* GLTF_SnapshotName = "_GLTF_Snapshot_";
static const idMat4 blenderToDoomTransform( idAngles( 0.0f, 0.0f, 90 ).ToMat3(), vec3_origin );
//static const idMat4 blenderToDoomTransform = mat4_identity;
static idRenderModelGLTF* lastMeshFromFile = nullptr;
// ==== engine neo/renderer/Model_gltf.cpp lines 64-136 (idRenderModelGLTF::ProcessNode_r) ====
void idRenderModelGLTF::ProcessNode_r( gltfNode* modelNode, const idMat4& parentTransform, const idMat4& globalTransform, gltfData* data )
{
	auto& meshList = data->MeshList();
	auto& nodeList = data->NodeList();

	gltfData::ResolveNodeMatrix( modelNode );

	idMat4 nodeToWorldTransform = parentTransform * modelNode->matrix;

	if( modelNode->mesh >= 0 )
	{
		gltfMesh* targetMesh = meshList[modelNode->mesh];

		for( auto prim : targetMesh->primitives )
		{
			// FIXME ConvertFromMeshGltf should only be used for the map
			// here ConvertGltfMeshToModelsurfaces should be used.
			MapPolygonMesh* mesh = MapPolygonMesh::ConvertFromMeshGltf( prim, data, globalTransform * nodeToWorldTransform );
			modelSurface_t	surf;

			gltfMaterial* mat = NULL;
			if( prim->material != -1 )
			{
				mat = data->MaterialList()[prim->material];
			}
			if( mat != NULL && !gltf_forceBspMeshTexture.GetBool() )
			{
				surf.shader = declManager->FindMaterial( mat->name );
			}
			else
			{
				surf.shader = declManager->FindMaterial( "textures/base_wall/snpanel2rust" );
			}
			surf.id = this->NumSurfaces();

			srfTriangles_t* tri = R_AllocStaticTriSurf();
			tri->numIndexes = mesh->GetNumPolygons() * 3;
			tri->numVerts = mesh->GetNumVertices();

			R_AllocStaticTriSurfIndexes( tri, tri->numIndexes );
			R_AllocStaticTriSurfVerts( tri, tri->numVerts );

			int indx = 0;
			for( int i = 0; i < mesh->GetNumPolygons(); i++ )
			{
				auto& face = mesh->GetFace( i );
				auto& faceIdxs = face.GetIndexes();
				tri->indexes[indx] = faceIdxs[0];
				tri->indexes[indx + 1] = faceIdxs[1];
				tri->indexes[indx + 2] = faceIdxs[2];
				indx += 3;
			}

			tri->bounds.Clear();
			for( int i = 0; i < tri->numVerts; ++i )
			{
				tri->verts[i] = mesh->GetDrawVerts()[i];
				tri->bounds.AddPoint( tri->verts[i].xyz );
			}

			bounds.AddBounds( tri->bounds );

			surf.geometry = tri;
			AddSurface( surf );
			delete mesh;
		}
	}

	for( auto& child : modelNode->children )
	{
		ProcessNode_r( nodeList[child], nodeToWorldTransform, globalTransform, data );
	}
}
// ==== engine neo/renderer/Model_gltf.cpp lines 317-465 (FindModelRoot) ====
// return Armature/Rig node
static gltfNode* FindModelRoot( gltfData* data, const idImportOptions* options, idStr& rootName, int* rootID, gltfSkin** rootSkin )
{
	// According to CG Dive is the most common workflow to get skeletal models from Blender into UE4/5
	// by looking for an Armature that is called "root". Usually everything else is supposed to fail.

	// So the expectations in common .glb files is that the first scene node in the hierarchy is the Armature
	// and skinned meshes are supposed to be children of the Rig.

	// 1. If there was an explicit name given in the identifer try it
	gltfNode* root = nullptr;
	auto nodes = data->NodeList();

	if( options != nullptr && !options->armature.IsEmpty() )
	{
		idLib::Printf( "Looking for armature %s\n", options->armature.c_str() );

		auto skin = data->GetSkin( options->armature );
		if( skin && ( skin->skeleton > -1 && skin->skeleton < nodes.Num() ) )
		{
			*rootID = skin->skeleton;
			root = nodes[skin->skeleton];
			rootName = options->armature; // because options aren't available in LoadModel()

			if( rootSkin )
			{
				*rootSkin = skin;
			}

			idLib::Printf( "Found glTF2 Armature: name = '%s' ID=%i\n", rootName.c_str(), *rootID );

			return root;
		}
	}

	// 2. If an explict root node name is given use that even if it might be not an Armature
	if( root == nullptr && !rootName.IsEmpty() )
	{
		// try explicit node which can be an Armature name
		root = data->GetNode( rootName, rootID );

		if( root )
		{
			idList<int, TAG_MODEL> meshNodeIDs;
			data->GetAllSkinnedMeshes( root, meshNodeIDs );

			int skinID = -1;
			for( int meshID : meshNodeIDs )
			{
				gltfNode* meshNode = nodes[meshID];
				if( meshNode->skin >= 0 )
				{
					skinID = meshNode->skin;
					break;
				}
			}

			if( skinID != -1 )
			{
				auto skin = data->SkinList()[skinID];
				*rootID = skin->skeleton;
				root = nodes[skin->skeleton];
				rootName = nodes[skin->skeleton]->name;

				if( rootSkin )
				{
					*rootSkin = skin;
				}

				idLib::Printf( "Found glTF2 Rig: name = '%s' ID=%i\n", rootName.c_str(), *rootID );

				return root;
			}
		}


		// Special case where you want multiple hiearchical models in the same .glb that are static models
		if( !root )
		{
			root = data->GetMeshNode( rootName, rootID );

			idLib::Printf( "Found glTF2 Mesh: name = '%s' ID=%i\n", rootName.c_str(), *rootID );
		}
	}

	// 3. Find Armature using skin->skeleton using the child meshes of the armature
	if( root == nullptr )
	{
		// get all skinned mesh IDs without a given root node

		idList<int, TAG_MODEL> meshNodeIDs;
		data->GetAllSkinnedMeshes( meshNodeIDs );

		int skinID = -1;
		for( int meshID : meshNodeIDs )
		{
			gltfNode* meshNode = nodes[meshID];
			if( meshNode->skin >= 0 )
			{
				skinID = meshNode->skin;
				break;
			}
		}

		if( skinID != -1 )
		{
			auto skin = data->SkinList()[skinID];
			*rootID = skin->skeleton;
			root = nodes[skin->skeleton];
			rootName = nodes[skin->skeleton]->name;

			if( rootSkin )
			{
				*rootSkin = skin;
			}

			idLib::Printf( "Found glTF2 Rig: name = '%s' ID=%i\n", rootName.c_str(), *rootID );

			return root;
		}
	}

	// 4. There was no Armature so far so just try to use the first Mesh from the default scene
	/*
	if( root == nullptr )
	{
		// try the first mesh from the default scene
		if( data->MeshList().Num() > 0 )
		{
			int sceneId = data->DefaultScene();
			assert( sceneId >= 0 );

			auto scene = data->SceneList()[sceneId];
			assert( scene );

			gltfMesh* firstMesh = data->MeshList()[0];

			//fileExclusive = true;
			root = data->GetNode( scene, firstMesh, rootID );
			if( rootID != nullptr )
			{
				rootName = root->name;
			}
		}
	}
	*/

	return root;
}
