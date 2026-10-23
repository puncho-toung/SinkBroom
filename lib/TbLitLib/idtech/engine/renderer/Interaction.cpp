// ==== engine neo/renderer/Interaction.cpp lines 1-27 (licence) ====
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
// ==== engine neo/renderer/Interaction.cpp lines 102-117 (R_CalcInteractionCullBits: the light frustum's planes in surface space, the bounds' front bits) ====
	idPlane frustumPlanes[6];
	idRenderMatrix::GetFrustumPlanes( frustumPlanes, light->baseLightProject, true, true );

	int frontBits = 0;

	// cull the triangle surface bounding box
	for( int i = 0; i < 6; i++ )
	{
		R_GlobalPlaneToLocal( ent->modelMatrix, frustumPlanes[i], cullInfo.localClipPlanes[i] );

		// get front bits for the whole surface
		if( tri->bounds.PlaneDistance( cullInfo.localClipPlanes[i] ) >= LIGHT_CLIP_EPSILON )
		{
			frontBits |= 1 << i;
		}
	}
// ==== engine neo/renderer/Interaction.cpp lines 129-141 (R_CalcInteractionCullBits: the vertices' cull bits) ====
	for( int i = 0; i < 6; i++ )
	{
		// if completely infront of this clipping plane
		if( frontBits & ( 1 << i ) )
		{
			continue;
		}
		for( int j = 0; j < tri->numVerts; j++ )
		{
			float d = cullInfo.localClipPlanes[i].Distance( tri->verts[j].xyz );
			cullInfo.cullBits[j] |= ( d < LIGHT_CLIP_EPSILON ) << i;
		}
	}
// ==== engine neo/renderer/Interaction.cpp lines 289-325 (R_CreateInteractionLightTris: the triangle cull) ====
		indexes = newTri->indexes;
		const byte* facing = cullInfo.facing;
		const byte* cullBits = cullInfo.cullBits;
		for( faceNum = i = 0; i < tri->numIndexes; i += 3, faceNum++ )
		{
			int i1, i2, i3;

			// if we aren't self shadowing, let back facing triangles get
			// through so the smooth shaded bump maps light all the way around
			if( !includeBackFaces )
			{
				// back face cull
				if( !facing[ faceNum ] )
				{
					c_backfaced++;
					continue;
				}
			}

			i1 = tri->indexes[i + 0];
			i2 = tri->indexes[i + 1];
			i3 = tri->indexes[i + 2];

			// fast cull outside the frustum
			// if all three points are off one plane side, it definately isn't visible
			if( cullBits[i1] & cullBits[i2] & cullBits[i3] )
			{
				c_distance++;
				continue;
			}

			// add to the list
			indexes[numIndexes + 0] = i1;
			indexes[numIndexes + 1] = i2;
			indexes[numIndexes + 2] = i3;
			numIndexes += 3;
		}
