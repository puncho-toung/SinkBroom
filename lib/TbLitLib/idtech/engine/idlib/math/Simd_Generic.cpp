// ==== engine neo/idlib/math/Simd_Generic.cpp lines 1-27 (licence) ====
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
// ==== engine neo/idlib/math/Simd_Generic.cpp lines 39-39 (UNROLL1) ====
#define UNROLL1(Y) { int _IX; for (_IX=0;_IX<count;_IX++) {Y(_IX);} }
// ==== engine neo/idlib/math/Simd_Generic.cpp lines 76-88 (idSIMD_Generic::MinMax (idVec2 points)) ====
/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax( idVec2& min, idVec2& max, const idVec2* src, const int count )
{
	min[0] = min[1] = idMath::INFINITUM;
	max[0] = max[1] = -idMath::INFINITUM;
#define OPER(X) const idVec2 &v = src[(X)]; if ( v[0] < min[0] ) { min[0] = v[0]; } if ( v[0] > max[0] ) { max[0] = v[0]; } if ( v[1] < min[1] ) { min[1] = v[1]; } if ( v[1] > max[1] ) { max[1] = v[1]; }
	UNROLL1( OPER )
#undef OPER
}
// ==== engine neo/idlib/math/Simd_Generic.cpp lines 104-116 (idSIMD_Generic::MinMax (idDrawVert positions)) ====
/*
============
idSIMD_Generic::MinMax
============
*/
void VPCALL idSIMD_Generic::MinMax( idVec3& min, idVec3& max, const idDrawVert* src, const int count )
{
	min[0] = min[1] = min[2] = idMath::INFINITUM;
	max[0] = max[1] = max[2] = -idMath::INFINITUM;
#define OPER(X) const idVec3 &v = src[(X)].xyz; if ( v[0] < min[0] ) { min[0] = v[0]; } if ( v[0] > max[0] ) { max[0] = v[0]; } if ( v[1] < min[1] ) { min[1] = v[1]; } if ( v[1] > max[1] ) { max[1] = v[1]; } if ( v[2] < min[2] ) { min[2] = v[2]; } if ( v[2] > max[2] ) { max[2] = v[2]; }
	UNROLL1( OPER )
#undef OPER
}
