// ==== engine neo/idlib/Heap.h lines 1-48 (licence, include guard, memTag_t, MAX_TAGS) ====
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

#ifndef __HEAP_H__
#define __HEAP_H__

/*
===============================================================================

	Memory Management

===============================================================================
*/

// memory tag names are used to sort allocations for sys_dumpMemory and other reporting functions
enum memTag_t
{
#define MEM_TAG( x )	TAG_##x,
#include "sys/sys_alloc_tags.h"
	TAG_NUM_TAGS,
};

static const int MAX_TAGS = 256;
// ==== engine neo/idlib/Heap.h lines 52-67 (Mem_Alloc16, Mem_Free16, Mem_Alloc, Mem_Free, Mem_ClearedAlloc, Mem_CopyString) ====
// RB: 64 bit fixes, changed int to size_t
void* 		Mem_Alloc16( const size_t size, const memTag_t tag );
void		Mem_Free16( void* ptr );

ID_INLINE void* 	Mem_Alloc( const size_t size, const memTag_t tag )
{
	return Mem_Alloc16( size, tag );
}

ID_INLINE void		Mem_Free( void* ptr )
{
	Mem_Free16( ptr );
}

void* 		Mem_ClearedAlloc( const size_t size, const memTag_t tag );
char* 		Mem_CopyString( const char* in );
// ==== engine neo/idlib/Heap.h lines 122-217 (idTempArray) ====
/*
================================================
idTempArray is an array that is automatically free'd when it goes out of scope.
There is no "cast" operator because these are very unsafe.

The template parameter MUST BE POD!

Compile time asserting POD-ness of the template parameter is complicated due
to our vector classes that need a default constructor but are otherwise
considered POD.
================================================
*/
template < class T >
class idTempArray
{
public:
	idTempArray( idTempArray<T>& other );
	idTempArray( unsigned int num );

	~idTempArray();

	T& operator []( unsigned int i )
	{
		assert( i < num );
		return buffer[i];
	}
	const T& operator []( unsigned int i ) const
	{
		assert( i < num );
		return buffer[i];
	}

	T* Ptr()
	{
		return buffer;
	}
	const T* Ptr() const
	{
		return buffer;
	}

	size_t Size() const
	{
		return num * sizeof( T );
	}
	unsigned int Num() const
	{
		return num;
	}

	void Zero()
	{
		memset( Ptr(), 0, Size() );
	}

private:
	T* 				buffer;		// Ensure this buffer comes first, so this == &this->buffer
	unsigned int	num;
};

/*
========================
idTempArray::idTempArray
========================
*/
template < class T >
ID_INLINE idTempArray<T>::idTempArray( idTempArray<T>& other )
{
	this->num = other.num;
	this->buffer = other.buffer;
	other.num = 0;
	other.buffer = NULL;
}

/*
========================
idTempArray::idTempArray
========================
*/
template < class T >
ID_INLINE idTempArray<T>::idTempArray( unsigned int num )
{
	this->num = num;
	buffer = ( T* )Mem_Alloc( num * sizeof( T ), TAG_TEMP );
}

/*
========================
idTempArray::~idTempArray
========================
*/
template < class T >
ID_INLINE idTempArray<T>::~idTempArray()
{
	Mem_Free( buffer );
}
// ==== engine neo/idlib/Heap.h lines 1340-1340 (include guard end) ====
#endif /* !__HEAP_H__ */
