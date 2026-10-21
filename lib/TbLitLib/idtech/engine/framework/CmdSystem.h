// ==== engine neo/framework/CmdSystem.h lines 1-71 (licence, include guard, cmdFlags_t, cmdExecution_t, cmdFunction_t, argCompletion_t) ====
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

#ifndef __CMDSYSTEM_H__
#define __CMDSYSTEM_H__

/*
===============================================================================

	Console command execution and command text buffering.

	Any number of commands can be added in a frame from several different
	sources. Most commands come from either key bindings or console line input,
	but entire text files can be execed.

	Command execution takes a null terminated string, breaks it into tokens,
	then searches for a command or variable that matches the first token.

===============================================================================
*/

// command flags
typedef enum
{
	CMD_FL_ALL				= -1,
	CMD_FL_CHEAT			= BIT( 0 ),	// command is considered a cheat
	CMD_FL_SYSTEM			= BIT( 1 ),	// system command
	CMD_FL_RENDERER			= BIT( 2 ),	// renderer command
	CMD_FL_SOUND			= BIT( 3 ),	// sound command
	CMD_FL_GAME				= BIT( 4 ),	// game command
	CMD_FL_TOOL				= BIT( 5 )	// tool command
} cmdFlags_t;

// parameters for command buffer stuffing
typedef enum
{
	CMD_EXEC_NOW,						// don't return until completed
	CMD_EXEC_INSERT,					// insert at current position, but don't run yet
	CMD_EXEC_APPEND						// add to end of the command buffer (normal case)
} cmdExecution_t;

// command function
typedef void ( *cmdFunction_t )( const idCmdArgs& args );

// argument completion function
typedef void ( *argCompletion_t )( const idCmdArgs& args, void( *callback )( const char* s ) );
// ==== engine neo/framework/CmdSystem.h lines 96-141 (CommandLinks, CONSOLE_COMMAND macros) ====
// The command system will create commands for all the static definitions
// when it initializes.
idCommandLink* CommandLinks( idCommandLink* cl = NULL );

/*
================================================
The CONSOLE_COMMAND macro is an even easier way to create a console command by
automatically generating the idCommandLink variable, and it also allows all the
command code to be stripped from a build with a single define.  For example:

CONSOLE_COMMAND( Sys_DumpMemory, "Walks the heap and reports stats" ) {
	// do stuff
}

NOTE: All CONSOLE_COMMANDs will be stripped with the shipping build unless it's
created using the CONSOLE_COMMAND_SHIP macro.
================================================
*/

#if defined( ID_RETAIL ) && !defined( ID_RETAIL_INTERNAL )
	#define CONSOLE_COMMAND_SHIP			CONSOLE_COMMAND_COMPILE
	#define CONSOLE_COMMAND					CONSOLE_COMMAND_NO_COMPILE
	// We need to disable this warning to get commands that were made friends
	// of classes to compile as inline.
	// warning C4211: nonstandard extension used : redefined extern to static
	#pragma warning( disable : 4211 )
	// warning C4505: 'xxx' : unreferenced local function has been removed
	#pragma warning( disable : 4505 )
#else
	#define CONSOLE_COMMAND_SHIP			CONSOLE_COMMAND_COMPILE
	#define CONSOLE_COMMAND					CONSOLE_COMMAND_COMPILE
#endif

// Turn console commands into static inline code, which will cause them to be
// removed from the build.
#define CONSOLE_COMMAND_NO_COMPILE( name, comment, completion ) \
	static inline void name ## _f( const idCmdArgs &args )

// lint incorrectly gives this for all console commands: Issue 1568: (Warning -- Variable 'TestAtomicString_v' accesses variable 'atomicStringManager' before the latter is initialized through calls: 'TestAtomicString_f() => idAtomicString::FreeDynamic()')
// I can't figure out how to disable this just around CONSOLE_COMMAND, so it must stay disabled everywhere,
// which is a shame.
//lint -e1568
#define CONSOLE_COMMAND_COMPILE( name, comment, completion ) \
	void name ## _f( const idCmdArgs &args ); \
	idCommandLink name ## _v( #name, name ## _f, comment, completion  ); \
	void name ## _f( const idCmdArgs &args )
// ==== engine neo/framework/CmdSystem.h lines 289-289 (include guard end) ====
#endif /* !__CMDSYSTEM_H__ */
