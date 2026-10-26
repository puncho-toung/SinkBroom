// ==== engine neo/idlib/MapFile.h lines 1-81 (licence, include guard, the map versions and curve constants, idMapPrimitive) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2023 Robert Beckebans

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

#ifndef __MAPFILE_H__
#define __MAPFILE_H__
#include "gltfProperties.h"

/*
===============================================================================

	Reads or writes the contents of .map files into a standard internal
	format, which can then be moved into private formats for collision
	detection, map processing, or editor use.

	No validation (duplicate planes, null area brushes, etc) is performed.
	There are no limits to the number of any of the elements in maps.
	The order of entities, brushes, and sides is maintained.

===============================================================================
*/

const int OLD_MAP_VERSION					= 1;
// RB: added new map format
const int DOOM3_MAP_VERSION					= 2;
const int CURRENT_MAP_VERSION				= 3;
// RB end
const int DEFAULT_CURVE_SUBDIVISION			= 4;
const float DEFAULT_CURVE_MAX_ERROR			= 4.0f;
const float DEFAULT_CURVE_MAX_ERROR_CD		= 24.0f;
const float DEFAULT_CURVE_MAX_LENGTH		= -1.0f;
const float DEFAULT_CURVE_MAX_LENGTH_CD		= -1.0f;


class idMapPrimitive
{
public:
	// RB: new mesh primitive to work with Blender Ngons
	enum { TYPE_INVALID = -1, TYPE_BRUSH, TYPE_PATCH, TYPE_MESH };
	// RB end

	idDict					epairs;

	idMapPrimitive()
	{
		type = TYPE_INVALID;
	}
	virtual					~idMapPrimitive() { }
	int						GetType() const
	{
		return type;
	}

protected:
	int						type;
};
// ==== engine neo/idlib/MapFile.h lines 291-430 (MapPolygon, MapPolygonMesh) ====
// RB begin
class MapPolygon
{
	friend class MapPolygonMesh;

public:
	MapPolygon();
	MapPolygon( int numIndexes );
	~MapPolygon() { }

	const char* 			GetMaterial() const
	{
		return material;
	}

	void					SetMaterial( const char* p )
	{
		material = p;
	}

	void					AddIndex( int index )
	{
		indexes.Append( index );
	}

	void					SetIndexes( const idTempArray<int>& _indexes )
	{
		indexes.Resize( _indexes.Num() );

		for( unsigned int i = 0; i < _indexes.Num(); i++ )
		{
			indexes[i] = _indexes[i];
		}
	}

	const idList<int>&		GetIndexes() const
	{
		return indexes;
	}


protected:
	idStr					material;
	idList<int>				indexes;		// [3..n] references to vertices for each face
};

ID_INLINE MapPolygon::MapPolygon()
{
}

ID_INLINE MapPolygon::MapPolygon( int numIndexes )
{
	//indexes.AssureSize( 3 );
}


class MapPolygonMesh : public idMapPrimitive
{
public:
	MapPolygonMesh();
	~MapPolygonMesh()
	{
		//verts.DeleteContents();
		//polygons.DeleteContents( true );
	}

	void					ConvertFromBrush( const idMapBrush* brush, int entityNum, int primitiveNum );
	void					ConvertFromPatch( const idMapPatch* patch, int entityNum, int primitiveNum );
	static MapPolygonMesh*	ConvertFromMeshGltf( const gltfMesh_Primitive* prim, gltfData* _data, const idMat4& transform );
	static MapPolygonMesh*	Parse( idLexer& src, const idVec3& origin, float version = CURRENT_MAP_VERSION );
	bool					Write( idFile* fp, int primitiveNum, const idVec3& origin ) const;

	static MapPolygonMesh*	ParseJSON( idLexer& src );
	bool					WriteJSON( idFile* fp, int primitiveNum, const idVec3& origin ) const;



	int						GetNumVertices() const
	{
		return verts.Num();
	}

	int						AddVertex( const idDrawVert& v )
	{
		return verts.Append( v );
	}

	int						AddVertices( const idList<idDrawVert>& v )
	{
		return verts.Append( v );
	}

	int						GetNumPolygons() const
	{
		return polygons.Num();
	}

	//int						AddPolygon( MapPolygon* face )
	//{
	//	return polygons.Append( face );
	//}

	const MapPolygon& 			GetFace( int i ) const
	{
		return polygons[i];
	}

	unsigned int			GetGeometryCRC() const;

	const idList<idDrawVert>&	GetDrawVerts() const
	{
		return verts;
	}

	bool					IsOpaque() const
	{
		return opaque;
	}

	bool					IsAreaportal() const;

	void					GetBounds( idBounds& bounds ) const;

private:
	void					SetContents();

	int						originalType;

protected:

	idList<idDrawVert>		verts;			// vertices can be shared between polygons
	idList<MapPolygon>		polygons;

	// derived data after parsing

	// material surface flags
	int						contents;
	bool					opaque;
};
// RB end
// ==== engine neo/idlib/MapFile.h lines 592-592 (include guard end) ====
#endif /* !__MAPFILE_H__ */
