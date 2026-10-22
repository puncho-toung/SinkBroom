// ==== engine neo/renderer/Image.h lines 1-33 (licence and include guard) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2021 Robert Beckebans
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

#ifndef IMAGE_H_
#define IMAGE_H_
// ==== engine neo/renderer/Image.h lines 35-139 (textureType_t, textureFormat_t, the BitsForFormat and GetRowPitch declarations, textureColor_t) ====
enum textureType_t
{
	// RB: renamed to DDT_ to solve conflict with AMD compressonator
	DTT_DISABLED,
	DTT_2D,
	DTT_CUBIC,
	DTT_2D_ARRAY,		// new
	DTT_2D_MULTISAMPLE,	// new
};

/*
================================================
The internal *Texture Format Types*, ::textureFormat_t, are:
================================================
*/
enum textureFormat_t
{
	FMT_NONE,

	//------------------------
	// Standard color image formats
	//------------------------

	FMT_RGBA8,			// 32 bpp
	FMT_XRGB8,			// 32 bpp

	//------------------------
	// Alpha channel only
	//------------------------

	// Alpha ends up being the same as L8A8 in our current implementation, because straight
	// alpha gives 0 for color, but we want 1.
	FMT_ALPHA,

	//------------------------
	// Luminance replicates the value across RGB with a constant A of 255
	// Intensity replicates the value across RGBA
	//------------------------

	FMT_L8A8,			// 16 bpp
	FMT_LUM8,			//  8 bpp
	FMT_INT8,			//  8 bpp

	//------------------------
	// Compressed texture formats
	//------------------------

	FMT_DXT1,			// 4 bpp
	FMT_DXT5,			// 8 bpp

	//------------------------
	// Depth buffer formats
	//------------------------

	FMT_DEPTH,			// 24 bpp

	//------------------------
	//
	//------------------------

	FMT_X16,			// 16 bpp
	FMT_Y16_X16,		// 32 bpp
	FMT_RGB565,			// 16 bpp

	// ^-- used in BFG edition, don't change above for original .bimage compatibility

	FMT_ETC1_RGB8_OES,	// 4 bpp
	FMT_SHADOW_ARRAY,	// 32 bpp * 6
	FMT_RG16F,			// 32 bpp
	FMT_RGBA16F,		// 64 bpp
	FMT_RGBA32F,		// 128 bpp
	FMT_R32F,			// 32 bpp
	FMT_R11G11B10F,		// 32 bpp

	// ^-- used up until RBDOOM-3-BFG 1.5 == BIMAGE_VERSION_BFG

	FMT_BC6H,			// 8 bpp
	FMT_BC7,			// 8 bpp

	// ^-- introduced with RBDOOM-3-BFG 1.6 >= BIMAGE_VERSION_BC6

	//------------------------
	// Render targets only
	//------------------------

	FMT_R8F,			// 8 bpp, RT only
	FMT_DEPTH_STENCIL,  // 32 bpp, RT only
	FMT_RGBA16S,		// 64 bpp, RT only
};

int BitsForFormat( textureFormat_t format );
int GetRowPitch( const textureFormat_t& format, int width );

/*
================================================
DXT5 color formats
================================================
*/
enum textureColor_t
{
	CFM_DEFAULT,			// RGBA
	CFM_NORMAL_DXT5,		// XY format and use the fast DXT5 compressor
	CFM_YCOCG_DXT5,			// convert RGBA to CoCg_Y format
	CFM_GREEN_ALPHA,		// Copy the alpha channel to green
};
// ==== engine neo/renderer/Image.h lines 212-266 (MAX_TEXTURE_LEVELS, textureUsage_t, cubeFiles_t) ====
static const int	MAX_TEXTURE_LEVELS = 14;

// How is this texture used?  Determines the storage and color format
// NOTE: be very careful when editing these because it might break older .bimage files or the lookup name
// Only add new entries at the bottom
typedef enum
{
	TD_SPECULAR,			// may be compressed, and always zeros the alpha channel
	TD_DIFFUSE,				// may be compressed
	TD_DEFAULT,				// generic RGBA texture (particles, etc...)
	TD_BUMP,				// may be compressed with 8 bit lookup
	TD_FONT,				// Font image
	TD_LIGHT,				// Light image
	TD_LOOKUP_TABLE_MONO,	// Mono lookup table (including alpha)
	TD_LOOKUP_TABLE_ALPHA,	// Alpha lookup table with a white color channel
	TD_LOOKUP_TABLE_RGB1,	// RGB lookup table with a solid white alpha
	TD_LOOKUP_TABLE_RGBA,	// RGBA lookup table
	TD_COVERAGE,			// coverage map for fill depth pass when YCoCG is used
	TD_DEPTH,				// depth buffer copy for motion blur
	// RB begin
	TD_SPECULAR_PBR_RMAO,	// may be compressed, and always zeros the alpha channel, linear RGB R = roughness, G = metal, B = ambient occlusion
	TD_SPECULAR_PBR_RMAOD,	// may be compressed, alpha channel contains displacement map
	TD_HIGHQUALITY_CUBE,	// motorsep - Uncompressed cubemap texture (RGB colorspace)
	TD_LOWQUALITY_CUBE,		// motorsep - REMOVED
	TD_SHADOW_ARRAY,		// 2D depth buffer array for shadow mapping
	TD_RG16F,				// BRDF lookup table
	TD_RGBA16F,				// RT = render target format only, not written to disk
	TD_RGBA16S,				// RT only
	TD_RGBA32F,				// RT only
	TD_HDR_LIGHTPROBE,		// RB: 2D HDR octahedron probes stored as R11G11B10F in v1.3 - v1.5 and BC6 now
	// ^-- used up until RBDOOM-3-BFG 1.5
	TD_HDRI,				// RB: R11G11B10F or BC6
	// RB end
	TD_R32F,				// RT only
	TD_R8F,					// SP: RT only, added for ambient occlusion
	TD_LDR,					// SP: RT only, added for SRGB render target when tonemapping
	TD_DEPTH_STENCIL,       // SP: RT only, depth buffer and stencil buffer
} textureUsage_t;

// NOTE: be very careful when editing these because it might break older .bimage files or the lookup name
// Only add new entries at the bottom
typedef enum
{
	CF_2D,			// not a cube map
	CF_NATIVE,		// _px, _nx, _py, etc, directly sent to GL
	CF_CAMERA,		// _forward, _back, etc, rotated and flipped as needed before sending to GL
	CF_PANORAMA,	// RB: latlong encoded HDRI panorama typically used by Substance or Blender
	CF_2D_ARRAY,	// not a cube map but not a single 2d texture either
	CF_2D_PACKED_MIPCHAIN, // usually 2d but can be an octahedron, packed mipmaps into single 2d texture atlas and limited to dim^2
	// ^-- used up until RBDOOM-3-BFG 1.3
	CF_SINGLE,      // SP: A single texture cubemap. All six sides in one image.
	CF_QUAKE1,		// RB: _ft, _bk, etc, rotated and flipped as needed before sending to GL
	CF_NATIVE_HDR,	// HDR float cube

} cubeFiles_t;
// ==== engine neo/renderer/Image.h lines 272-272 (MAX_IMAGE_NAME) ====
#define	MAX_IMAGE_NAME	256
// ==== engine neo/renderer/Image.h lines 679-679 (R_Dropsample declaration) ====
byte* R_Dropsample( const byte* in, int inwidth, int inheight, int outwidth, int outheight );
// ==== engine neo/renderer/Image.h lines 680-699 (the other IMAGEPROCESS declarations, through R_GenerateCubeMapSideFromPanoramaImage) ====
byte* R_ResampleTexture( const byte* in, int inwidth, int inheight, int outwidth, int outheight );
byte* R_MipMapWithAlphaSpecularity( const byte* in, int width, int height );
byte* R_MipMapWithGamma( const byte* in, int width, int height );
byte* R_MipMap( const byte* in, int width, int height );
byte* R_MipMapR11G11B10F( const byte* in, int width, int height );

// these operate in-place on the provided pixels
void R_BlendOverTexture( byte* data, int pixelCount, const byte blend[4] );
void R_HorizontalFlip( byte* data, int width, int height );
void R_VerticalFlip( byte* data, int width, int height );
void R_VerticalFlipRGB16F( byte* data, int width, int height );
void R_RotatePic( byte* data, int width );
void R_ApplyCubeMapTransforms( int i, byte* data, int size );
// SP begin
// This method takes in a cubemap from a single image. Depending on the side (0-5),
// the image will be extracted from data and returned. The dimensions will be size x size.
byte* R_GenerateCubeMapSideFromSingleImage( const byte* in, int srcWidth, int srcHeight, int size, int side );
// SP end
// RB
byte* R_GenerateCubeMapSideFromPanoramaImage( const byte* in, int srcWidth, int srcHeight, int size, int side );
// ==== engine neo/renderer/Image.h lines 704-729 (R_LoadImage, R_LoadCubeImages, R_LoadImageProgram, R_ParsePastImageProgram declarations) ====
/*
====================================================================

IMAGEFILES

====================================================================
*/

// RB: added texture usage for PBR _rmao[d] HACK
void R_LoadImage( const char* name, byte** pic, int* width, int* height, ID_TIME_T* timestamp, bool makePowerOf2, textureUsage_t* usage );

// pic is in top to bottom raster format
bool R_LoadCubeImages( const char* cname, cubeFiles_t extensions, byte* pic[6], int* size, ID_TIME_T* timestamp, int cubeMapSize = 0 );

/*
====================================================================

IMAGEPROGRAM

====================================================================
*/

void R_LoadImageProgram( const char* name, byte** pic, int* width, int* height, ID_TIME_T* timestamp, textureUsage_t* usage = NULL );
const char* R_ParsePastImageProgram( idLexer& src );

#endif
