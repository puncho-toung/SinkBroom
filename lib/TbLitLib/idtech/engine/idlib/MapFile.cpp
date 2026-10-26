// ==== engine neo/idlib/MapFile.cpp lines 1-30 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2025 Robert Beckebans
Copyright (C) 2020 Admer (id Tech Fox)
Copyright (C) 2022 Harrie van Ginneken

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
// ==== engine neo/idlib/MapFile.cpp lines 2329-2338 (MapPolygonMesh::MapPolygonMesh) ====
// RB begin
MapPolygonMesh::MapPolygonMesh()
{
	type = TYPE_MESH;
	originalType = TYPE_MESH;
	polygons.Resize( 8, 4 );

	contents = CONTENTS_SOLID;
	opaque = true;
}
// ==== engine neo/idlib/MapFile.cpp lines 2966-3007 (MapPolygonMesh::SetContents) ====
void MapPolygonMesh::SetContents()
{
	if( polygons.Num() < 1 )
	{
		contents = CONTENTS_SOLID;
		opaque = true;

		return;
	}

	int			c2;

	MapPolygon* poly = &polygons[0];

	const idMaterial* mat = declManager->FindMaterial( poly->GetMaterial() );
	contents = mat->GetContentFlags();

	//b->contentShader = s->material;
	bool mixed = false;

	// a brush is only opaque if all sides are opaque
	opaque = true;

	for( int i = 1 ; i < polygons.Num() ; i++ )
	{
		poly = &polygons[i];

		const idMaterial* mat2 = declManager->FindMaterial( poly->GetMaterial() );

		c2 = mat2->GetContentFlags();
		if( c2 != contents )
		{
			mixed = true;
			contents |= c2;
		}

		if( mat2->Coverage() != MC_OPAQUE )
		{
			opaque = false;
		}
	}
}
