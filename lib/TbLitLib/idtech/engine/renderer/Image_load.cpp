// ==== engine neo/renderer/Image_load.cpp lines 1-31 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2025 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel
Copyright (C) 2016-2017 Dustin Land
Copyright (C) 2022 Stephen Pridham

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
// ==== engine neo/renderer/Image_load.cpp lines 39-143 (BitsForFormat, BlockSizeForFormat, GetRowPitch) ====
/*
================
BitsForFormat
================
*/
int BitsForFormat( textureFormat_t format )
{
	switch( format )
	{
		case FMT_NONE:
			return 0;
		case FMT_RGBA8:
			return 32;
		case FMT_XRGB8:
			return 32;
		case FMT_RGB565:
			return 16;
		case FMT_L8A8:
			return 16;
		case FMT_ALPHA:
			return 8;
		case FMT_LUM8:
			return 8;
		case FMT_INT8:
			return 8;
		case FMT_DXT1:
			return 4;
		case FMT_DXT5:
			return 8;
		// RB: added ETC compression
		case FMT_ETC1_RGB8_OES:
			return 4;
		case FMT_SHADOW_ARRAY:
			return ( 32 * 6 );
		case FMT_RG16F:
			return 32;
		case FMT_RGBA16F:
			return 64;
		case FMT_RGBA16S:
			return 64;
		case FMT_RGBA32F:
			return 128;
		case FMT_R32F:
			return 32;
		case FMT_R11G11B10F:
			return 32;
		case FMT_BC6H:
			return 8;
		case FMT_BC7:
			return 8;
		// RB end
		case FMT_DEPTH:
			return 32;
		case FMT_DEPTH_STENCIL:
			return 32;
		case FMT_X16:
			return 16;
		case FMT_Y16_X16:
			return 32;
		case FMT_R8F:
			return 4;
		default:
			assert( 0 );
			return 0;
	}
}

int BlockSizeForFormat( const textureFormat_t& format )
{
	switch( format )
	{
		case FMT_NONE:
			return 0;
		case FMT_DXT1:
			return 8;
		case FMT_DXT5:
			return 16;
		case FMT_BC6H:
			return 16;
		case FMT_BC7:
			return 16;
		default:
			return 1;
	}
}

/*
=========================
GetRowBytes
Returns the row bytes for the given image.
=========================
*/
int GetRowPitch( const textureFormat_t& format, int width )
{
	bool bc = ( format == FMT_DXT1 || format == FMT_DXT5 || format == FMT_BC6H || format == FMT_BC7 );

	if( bc )
	{
		int blockSize = BlockSizeForFormat( format );
		return std::max( 1, ( width + 3 ) / 4 ) * blockSize;
	}

	int bpe = BitsForFormat( format );
	return width * ( bpe / 8 );
}
