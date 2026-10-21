// ==== engine neo/renderer/Image_process.cpp lines 1-34 (licence and includes) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2021-2025 Robert Beckebans
Copyright (C) 2021 Stephen Pridham

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
// ==== engine neo/renderer/Image_process.cpp lines 110-144 (R_Dropsample) ====
/*
================
R_Dropsample

Used to resample images in a more general than quartering fashion.
Normal maps and such should not be bilerped.
================
*/
byte* R_Dropsample( const byte* in, int inwidth, int inheight,
					int outwidth, int outheight )
{
	int		i, j, k;
	const byte*	inrow;
	const byte*	pix1;
	byte*		out, *out_p;

	out = ( byte* )R_StaticAlloc( outwidth * outheight * 4, TAG_IMAGE );
	out_p = out;

	for( i = 0 ; i < outheight ; i++, out_p += outwidth * 4 )
	{
		inrow = in + 4 * inwidth * ( int )( ( i + 0.25 ) * inheight / outheight );
		for( j = 0 ; j < outwidth ; j++ )
		{
			k = j * inwidth / outwidth;
			pix1 = inrow + k * 4;
			out_p[j * 4 + 0] = pix1[0];
			out_p[j * 4 + 1] = pix1[1];
			out_p[j * 4 + 2] = pix1[2];
			out_p[j * 4 + 3] = pix1[3];
		}
	}

	return out;
}
