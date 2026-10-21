// ==== engine neo/idlib/Dict.cpp lines 1-115 (licence, include, globalKeys, globalValues, operator=, Copy) ====
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

#include "precompiled.h"
#pragma hdrstop

idStrPool		idDict::globalKeys;
idStrPool		idDict::globalValues;

/*
================
idDict::operator=

  clear existing key/value pairs and copy all key/value pairs from other
================
*/
idDict& idDict::operator=( const idDict& other )
{
	int i;

	// check for assignment to self
	if( this == &other )
	{
		return *this;
	}

	Clear();

	args = other.args;
	argHash = other.argHash;

	for( i = 0; i < args.Num(); i++ )
	{
		args[i].key = globalKeys.CopyString( args[i].key );
		args[i].value = globalValues.CopyString( args[i].value );
	}

	return *this;
}

/*
================
idDict::Copy

  copy all key value pairs without removing existing key/value pairs not present in the other dict
================
*/
void idDict::Copy( const idDict& other )
{
	int i, n, *found;
	idKeyValue kv;

	// check for assignment to self
	if( this == &other )
	{
		return;
	}

	n = other.args.Num();

	if( args.Num() )
	{
		found = ( int* ) _alloca16( other.args.Num() * sizeof( int ) );
		for( i = 0; i < n; i++ )
		{
			found[i] = FindKeyIndex( other.args[i].GetKey() );
		}
	}
	else
	{
		found = NULL;
	}

	for( i = 0; i < n; i++ )
	{
		if( found && found[i] != -1 )
		{
			// first set the new value and then free the old value to allow proper self copying
			const idPoolStr* oldValue = args[found[i]].value;
			args[found[i]].value = globalValues.CopyString( other.args[i].value );
			globalValues.FreeString( oldValue );
		}
		else
		{
			kv.key = globalKeys.CopyString( other.args[i].key );
			kv.value = globalValues.CopyString( other.args[i].value );
			argHash.Add( argHash.GenerateKey( kv.GetKey(), false ), args.Append( kv ) );
		}
	}
}
// ==== engine neo/idlib/Dict.cpp lines 197-256 (idDict::SetDefaults, Clear, Print) ====
/*
================
idDict::SetDefaults
================
*/
void idDict::SetDefaults( const idDict* dict )
{
	int i, n;
	const idKeyValue* kv, *def;
	idKeyValue newkv;

	n = dict->args.Num();
	for( i = 0; i < n; i++ )
	{
		def = &dict->args[i];
		kv = FindKey( def->GetKey() );
		if( !kv )
		{
			newkv.key = globalKeys.CopyString( def->key );
			newkv.value = globalValues.CopyString( def->value );
			argHash.Add( argHash.GenerateKey( newkv.GetKey(), false ), args.Append( newkv ) );
		}
	}
}

/*
================
idDict::Clear
================
*/
void idDict::Clear()
{
	int i;

	for( i = 0; i < args.Num(); i++ )
	{
		globalKeys.FreeString( args[i].key );
		globalValues.FreeString( args[i].value );
	}

	args.Clear();
	argHash.Free();
}

/*
================
idDict::Print
================
*/
void idDict::Print() const
{
	int i;
	int n;

	n = args.Num();
	for( i = 0; i < n; i++ )
	{
		idLib::common->Printf( "%s = %s\n", args[i].GetKey().c_str(), args[i].GetValue().c_str() );
	}
}
// ==== engine neo/idlib/Dict.cpp lines 288-546 (idDict::Allocated, Set and the Get* functions) ====
/*
================
idDict::Allocated
================
*/
size_t idDict::Allocated() const
{
	int		i;
	size_t	size;

	size = args.Allocated() + argHash.Allocated();
	for( i = 0; i < args.Num(); i++ )
	{
		size += args[i].Size();
	}

	return size;
}

/*
================
idDict::Set
================
*/
void idDict::Set( const char* key, const char* value )
{
	int i;
	idKeyValue kv;

	if( key == NULL || key[0] == '\0' )
	{
		return;
	}

	i = FindKeyIndex( key );
	if( i != -1 )
	{
		// first set the new value and then free the old value to allow proper self copying
		const idPoolStr* oldValue = args[i].value;
		args[i].value = globalValues.AllocString( value );
		globalValues.FreeString( oldValue );
	}
	else
	{
		kv.key = globalKeys.AllocString( key );
		kv.value = globalValues.AllocString( value );
		argHash.Add( argHash.GenerateKey( kv.GetKey(), false ), args.Append( kv ) );
	}
}

/*
================
idDict::GetFloat
================
*/
bool idDict::GetFloat( const char* key, const char* defaultString, float& out ) const
{
	const char*	s;
	bool		found;

	found = GetString( key, defaultString, &s );
	out = atof( s );
	return found;
}

/*
================
idDict::GetInt
================
*/
bool idDict::GetInt( const char* key, const char* defaultString, int& out ) const
{
	const char*	s;
	bool		found;

	found = GetString( key, defaultString, &s );
	out = atoi( s );
	return found;
}

/*
================
idDict::GetBool
================
*/
bool idDict::GetBool( const char* key, const char* defaultString, bool& out ) const
{
	const char*	s;
	bool		found;

	found = GetString( key, defaultString, &s );
	out = ( atoi( s ) != 0 );
	return found;
}

/*
================
idDict::GetFloat
================
*/
bool idDict::GetFloat( const char* key, const float defaultFloat, float& out ) const
{
	const idKeyValue* kv = FindKey( key );
	if( kv )
	{
		out = atof( kv->GetValue() );
		return true;
	}
	else
	{
		out = defaultFloat;
		return false;
	}
}

/*
================
idDict::GetInt
================
*/
bool idDict::GetInt( const char* key, const int defaultInt, int& out ) const
{
	const idKeyValue* kv = FindKey( key );
	if( kv )
	{
		out = atoi( kv->GetValue() );
		return true;
	}
	else
	{
		out = defaultInt;
		return false;
	}
}

/*
================
idDict::GetBool
================
*/
bool idDict::GetBool( const char* key, const bool defaultBool, bool& out ) const
{
	const idKeyValue* kv = FindKey( key );
	if( kv )
	{
		out = ( atoi( kv->GetValue() ) != 0 );
		return true;
	}
	else
	{
		out = defaultBool;
		return false;
	}
}

/*
================
idDict::GetAngles
================
*/
bool idDict::GetAngles( const char* key, const char* defaultString, idAngles& out ) const
{
	bool		found;
	const char*	s;

	if( !defaultString )
	{
		defaultString = "0 0 0";
	}

	found = GetString( key, defaultString, &s );
	out.Zero();
	sscanf( s, "%f %f %f", &out.pitch, &out.yaw, &out.roll );
	return found;
}

/*
================
idDict::GetVector
================
*/
bool idDict::GetVector( const char* key, const char* defaultString, idVec3& out ) const
{
	bool		found;
	const char*	s;

	if( !defaultString )
	{
		defaultString = "0 0 0";
	}

	found = GetString( key, defaultString, &s );
	out.Zero();
	sscanf( s, "%f %f %f", &out.x, &out.y, &out.z );
	return found;
}

/*
================
idDict::GetVec2
================
*/
bool idDict::GetVec2( const char* key, const char* defaultString, idVec2& out ) const
{
	bool		found;
	const char*	s;

	if( !defaultString )
	{
		defaultString = "0 0";
	}

	found = GetString( key, defaultString, &s );
	out.Zero();
	sscanf( s, "%f %f", &out.x, &out.y );
	return found;
}

/*
================
idDict::GetVec4
================
*/
bool idDict::GetVec4( const char* key, const char* defaultString, idVec4& out ) const
{
	bool		found;
	const char*	s;

	if( !defaultString )
	{
		defaultString = "0 0 0 0";
	}

	found = GetString( key, defaultString, &s );
	out.Zero();
	sscanf( s, "%f %f %f %f", &out.x, &out.y, &out.z, &out.w );
	return found;
}

/*
================
idDict::GetMatrix
================
*/
bool idDict::GetMatrix( const char* key, const char* defaultString, idMat3& out ) const
{
	const char*	s;
	bool		found;

	if( !defaultString )
	{
		defaultString = "1 0 0 0 1 0 0 0 1";
	}

	found = GetString( key, defaultString, &s );
	out.Identity();		// sccanf has a bug in it on Mac OS 9.  Sigh.
	sscanf( s, "%f %f %f %f %f %f %f %f %f", &out[0].x, &out[0].y, &out[0].z, &out[1].x, &out[1].y, &out[1].z, &out[2].x, &out[2].y, &out[2].z );
	return found;
}
// ==== engine neo/idlib/Dict.cpp lines 563-696 (idDict::FindKey, FindKeyIndex, Delete, DeleteEmptyKeys, MatchPrefix) ====
/*
================
idDict::FindKey
================
*/
const idKeyValue* idDict::FindKey( const char* key ) const
{
	int i, hash;

	if( key == NULL || key[0] == '\0' )
	{
		idLib::common->DWarning( "idDict::FindKey: empty key" );
		return NULL;
	}

	hash = argHash.GenerateKey( key, false );
	for( i = argHash.First( hash ); i != -1; i = argHash.Next( i ) )
	{
		if( args[i].GetKey().Icmp( key ) == 0 )
		{
			return &args[i];
		}
	}

	return NULL;
}

/*
================
idDict::FindKeyIndex
================
*/
int idDict::FindKeyIndex( const char* key ) const
{

	if( key == NULL || key[0] == '\0' )
	{
		idLib::common->DWarning( "idDict::FindKeyIndex: empty key" );
		return 0;
	}

	int hash = argHash.GenerateKey( key, false );
	for( int i = argHash.First( hash ); i != -1; i = argHash.Next( i ) )
	{
		if( args[i].GetKey().Icmp( key ) == 0 )
		{
			return i;
		}
	}

	return -1;
}

/*
================
idDict::Delete
================
*/
void idDict::Delete( const char* key )
{
	int hash, i;

	hash = argHash.GenerateKey( key, false );
	for( i = argHash.First( hash ); i != -1; i = argHash.Next( i ) )
	{
		if( args[i].GetKey().Icmp( key ) == 0 )
		{
			globalKeys.FreeString( args[i].key );
			globalValues.FreeString( args[i].value );
			args.RemoveIndex( i );
			argHash.RemoveIndex( hash, i );
			break;
		}
	}

#if 0
	// make sure all keys can still be found in the hash index
	for( i = 0; i < args.Num(); i++ )
	{
		assert( FindKey( args[i].GetKey() ) != NULL );
	}
#endif
}

// RB
void idDict::DeleteEmptyKeys()
{
	idList<idKeyValue> orig = args;

	for( int i = 0; i < orig.Num(); i++ )
	{
		const idKeyValue& kv = orig[ i ];

		if( kv.GetValue().Length() == 0 )
		{
			Delete( kv.GetKey() );
		}
	}
}

/*
================
idDict::MatchPrefix
================
*/
const idKeyValue* idDict::MatchPrefix( const char* prefix, const idKeyValue* lastMatch ) const
{
	int	i;
	int len;
	int start;

	assert( prefix );
	len = strlen( prefix );

	start = -1;
	if( lastMatch )
	{
		start = args.FindIndex( *lastMatch );
		assert( start >= 0 );
		if( start < 1 )
		{
			start = 0;
		}
	}

	for( i = start + 1; i < args.Num(); i++ )
	{
		if( !args[i].GetKey().Icmpn( prefix, len ) )
		{
			return &args[i];
		}
	}
	return NULL;
}
// ==== engine neo/idlib/Dict.cpp lines 1043-1063 (idDict::Init, Shutdown) ====
/*
================
idDict::Init
================
*/
void idDict::Init()
{
	globalKeys.SetCaseSensitive( false );
	globalValues.SetCaseSensitive( true );
}

/*
================
idDict::Shutdown
================
*/
void idDict::Shutdown()
{
	globalKeys.Clear();
	globalValues.Clear();
}
