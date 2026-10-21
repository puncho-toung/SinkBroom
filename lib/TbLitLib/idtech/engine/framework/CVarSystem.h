// ==== engine neo/framework/CVarSystem.h lines 1-30 (licence and include guard) ====
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

#ifndef __CVARSYSTEM_H__
#define __CVARSYSTEM_H__
// ==== engine neo/framework/CVarSystem.h lines 82-105 (cvarFlags_t) ====
typedef enum
{
	CVAR_ALL				= -1,		// all flags
	CVAR_BOOL				= BIT( 0 ),	// variable is a boolean
	CVAR_INTEGER			= BIT( 1 ),	// variable is an integer
	CVAR_FLOAT				= BIT( 2 ),	// variable is a float
	CVAR_SYSTEM				= BIT( 3 ),	// system variable
	CVAR_RENDERER			= BIT( 4 ),	// renderer variable
	CVAR_SOUND				= BIT( 5 ),	// sound variable
	CVAR_GUI				= BIT( 6 ),	// gui variable
	CVAR_GAME				= BIT( 7 ),	// game variable
	CVAR_TOOL				= BIT( 8 ),	// tool variable
	// original doom3 used to have CVAR_USERINFO ("sent to servers, available to menu") here
	CVAR_SERVERINFO			= BIT( 10 ),	// sent from servers, available to menu
	CVAR_NETWORKSYNC		= BIT( 11 ),	// cvar is synced from the server to clients
	CVAR_STATIC				= BIT( 12 ),	// statically declared, not user created
	CVAR_CHEAT				= BIT( 13 ),	// variable is considered a cheat
	CVAR_NOCHEAT			= BIT( 14 ),	// variable is not considered a cheat
	CVAR_INIT				= BIT( 15 ),	// can only be set from the command-line
	CVAR_ROM				= BIT( 16 ),	// display only, cannot be set by user at all
	CVAR_ARCHIVE			= BIT( 17 ),	// set to cause it to be saved to a config file
	CVAR_MODIFIED			= BIT( 18 ),	// set when the variable is modified
	CVAR_NEW				= BIT( 19 )		// added for RBDoom
} cvarFlags_t;
// ==== engine neo/framework/CVarSystem.h lines 393-393 (include guard end) ====
#endif /* !__CVARSYSTEM_H__ */
