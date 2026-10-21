// ==== engine neo/framework/Common.h lines 1-32 (licence and include guard) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2025 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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

#ifndef __COMMON_H__
#define __COMMON_H__
// ==== engine neo/framework/Common.h lines 196-212 (toolFlag_t) ====
typedef enum
{
	EDITOR_NONE					= 0,
	EDITOR_GUI					= BIT( 1 ),
	EDITOR_DEBUGGER				= BIT( 2 ),
	EDITOR_SCRIPT				= BIT( 3 ),
	EDITOR_LIGHT				= BIT( 4 ),
	EDITOR_SOUND				= BIT( 5 ),
	EDITOR_DECL					= BIT( 6 ),
	EDITOR_AF					= BIT( 7 ),
	EDITOR_PARTICLE				= BIT( 8 ),
	EDITOR_PDA					= BIT( 9 ),
	EDITOR_AAS					= BIT( 10 ),
	EDITOR_MATERIAL				= BIT( 11 ),
	EDITOR_EXPORTDEFS			= BIT( 12 ), // RB
	EDITOR_DMAP					= BIT( 13 ), // set while the in-engine dmap runs
} toolFlag_t;
// ==== engine neo/framework/Common.h lines 226-226 (com_editors) ====
extern int			com_editors;			// currently opened editor(s)
// ==== engine neo/framework/Common.h lines 451-451 (include guard end) ====
#endif /* !__COMMON_H__ */
