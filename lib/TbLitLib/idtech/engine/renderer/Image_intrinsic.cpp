// ==== engine neo/renderer/Image_intrinsic.cpp lines 1-29 (licence) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2024 Robert Beckebans
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
// ==== engine neo/renderer/Image_intrinsic.cpp lines 793-815 (R_CreateBlueNoise256Image) ====
static void R_CreateBlueNoise256Image( idImage* image, nvrhi::ICommandList* commandList )
{
	static byte	data[BLUENOISE_TEX_HEIGHT][BLUENOISE_TEX_WIDTH][4];

	for( int x = 0; x < BLUENOISE_TEX_WIDTH; x++ )
	{
		for( int y = 0; y < BLUENOISE_TEX_HEIGHT; y++ )
		{
#if 1
			data[x][y][0] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x * 3 + 0 ];
			data[x][y][1] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x * 3 + 1 ];
			data[x][y][2] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x * 3 + 2 ];
#else
			data[x][y][0] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x ];
			data[x][y][1] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x ];
			data[x][y][2] = blueNoiseTexBytes[ y * BLUENOISE_TEX_PITCH + x ];
#endif
			data[x][y][3] = 1;
		}
	}

	image->GenerateImage( ( byte* )data, BLUENOISE_TEX_WIDTH, BLUENOISE_TEX_HEIGHT, TF_NEAREST, TR_REPEAT, TD_LOOKUP_TABLE_RGBA, commandList );
}
