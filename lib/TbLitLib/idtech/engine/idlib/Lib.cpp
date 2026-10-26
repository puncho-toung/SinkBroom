// ==== engine neo/idlib/Lib.cpp lines 1-27 (licence) ====
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
// ==== engine neo/idlib/Lib.cpp lines 154-166 (PackColor) ====
/*
================
PackColor
================
*/
dword PackColor( const idVec4& color )
{
	byte dx = idMath::Ftob( color.x * 255.0f );
	byte dy = idMath::Ftob( color.y * 255.0f );
	byte dz = idMath::Ftob( color.z * 255.0f );
	byte dw = idMath::Ftob( color.w * 255.0f );
	return ( dx << 0 ) | ( dy << 8 ) | ( dz << 16 ) | ( dw << 24 );
}
// ==== engine neo/idlib/Lib.cpp lines 490-537 (RevBytesSwap) ====
/*
=====================================================================
RevBytesSwap

Reverses byte order in place.

INPUTS
   bp       bytes to reverse
   elsize   size of the underlying data type
   elcount  number of elements to swap

RESULTS
   Reverses the byte order in each of elcount elements.
===================================================================== */
void RevBytesSwap( void* bp, int elsize, int elcount )
{
	unsigned char* p, *q;

	p = ( unsigned char* ) bp;

	if( elsize == 2 )
	{
		q = p + 1;
		while( elcount-- )
		{
			*p ^= *q;
			*q ^= *p;
			*p ^= *q;
			p += 2;
			q += 2;
		}
		return;
	}

	while( elcount-- )
	{
		q = p + elsize - 1;
		while( p < q )
		{
			*p ^= *q;
			*q ^= *p;
			*p ^= *q;
			++p;
			--q;
		}
		p += elsize >> 1;
	}
}
// ==== engine neo/idlib/Lib.cpp lines 574-582 (RevBytesNoSwap) ====
/*
================
RevBytesNoSwap
================
*/
void RevBytesNoSwap( void* bp, int elsize, int elcount )
{
	return;
}
