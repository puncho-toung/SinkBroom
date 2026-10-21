// ==== engine neo/framework/DeclManager.cpp lines 1-31 (licence and include) ====
/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2015-2023 Robert Beckebans

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
// ==== engine neo/framework/DeclManager.cpp lines 75-284 (idDeclType, idDeclFolder, idDeclLocal, idDeclFile, idDeclManagerLocal) ====
class idDeclType
{
public:
	idStr						typeName;
	declType_t					type;
	idDecl* 					( *allocator )();
};

class idDeclFolder
{
public:
	idStr						folder;
	idStr						extension;
	declType_t					defaultType;
};

class idDeclFile;

class idDeclLocal : public idDeclBase
{
	friend class idDeclFile;
	friend class idDeclManagerLocal;

public:
	idDeclLocal();
	virtual 					~idDeclLocal() {};
	virtual const char* 		GetName() const;
	virtual declType_t			GetType() const;
	virtual declState_t			GetState() const;
	virtual bool				IsImplicit() const;
	virtual bool				IsValid() const;
	virtual void				Invalidate();
	virtual void				Reload();
	virtual void				EnsureNotPurged();
	virtual int					Index() const;
	virtual int					GetLineNum() const;
	virtual const char* 		GetFileName() const;
	virtual size_t				Size() const;
	virtual void				GetText( char* text ) const;
	virtual int					GetTextLength() const;
	virtual void				SetText( const char* text );
	virtual bool				ReplaceSourceFileText();
	virtual bool				SourceFileChanged() const;
	virtual void				MakeDefault();
	virtual bool				EverReferenced() const;
	virtual ID_TIME_T			GetSourceFileTimestamp() const; // RB

protected:
	virtual bool				SetDefaultText();
	virtual const char* 		DefaultDefinition() const;
	virtual bool				Parse( const char* text, const int textLength, bool allowBinaryVersion );
	virtual void				FreeData();
	virtual void				List() const;
	virtual void				Print() const;

protected:
	void						AllocateSelf();

	// Parses the decl definition.
	// After calling parse, a decl will be guaranteed usable.
	void						ParseLocal();

	// Does a MakeDefualt, but flags the decl so that it
	// will Parse() the next time the decl is found.
	void						Purge();

	// Set textSource possible with compression.
	void						SetTextLocal( const char* text, const int length );

private:
	idDecl* 					self;

	idStr						name;					// name of the decl
	char* 						textSource;				// decl text definition
	int							textLength;				// length of textSource
	int							compressedLength;		// compressed length
	idDeclFile* 				sourceFile;				// source file in which the decl was defined
	int							sourceTextOffset;		// offset in source file to decl text
	int							sourceTextLength;		// length of decl text in source file
	int							sourceLine;				// this is where the actual declaration token starts
	int							checksum;				// checksum of the decl text
	declType_t					type;					// decl type
	declState_t					declState;				// decl state
	int							index;					// index in the per-type list

	bool						parsedOutsideLevelLoad;	// these decls will never be purged
	bool						everReferenced;			// set to true if the decl was ever used
	bool						referencedThisLevel;	// set to true when the decl is used for the current level
	bool						redefinedInReload;		// used during file reloading to make sure a decl that has
	// its source removed will be defaulted
	idDeclLocal* 				nextInFile;				// next decl in the decl file
};

class idDeclFile
{
public:
	idDeclFile();
	idDeclFile( const char* fileName, declType_t defaultType );

	void						Reload( bool force );
	int							LoadAndParse();

public:
	idStr						fileName;
	declType_t					defaultType;

	ID_TIME_T						timestamp;
	int							checksum;
	int							fileSize;
	int							numLines;

	idDeclLocal* 				decls;
};

class idDeclManagerLocal : public idDeclManager
{
	friend class idDeclLocal;

public:
	virtual void				Init();
	virtual void				Init2();
	virtual void				InitTool();
	virtual void				Shutdown();
	virtual void				Reload( bool force );
	virtual void				BeginLevelLoad();
	virtual void				EndLevelLoad();
	virtual void				RegisterDeclType( const char* typeName, declType_t type, idDecl * ( *allocator )() );
	virtual void				RegisterDeclFolder( const char* folder, const char* extension, declType_t defaultType );
	virtual int					GetChecksum() const;
	virtual int					GetNumDeclTypes() const;
	virtual int					GetNumDecls( declType_t type );
	virtual const char* 		GetDeclNameFromType( declType_t type ) const;
	virtual declType_t			GetDeclTypeFromName( const char* typeName ) const;
	virtual const idDecl* 		FindType( declType_t type, const char* name, bool makeDefault = true );
	virtual const idDecl* 		DeclByIndex( declType_t type, int index, bool forceParse = true );

	virtual const idDecl*		FindDeclWithoutParsing( declType_t type, const char* name, bool makeDefault = true );
	virtual void				ReloadFile( const char* filename, bool force );

	virtual void				ListType( const idCmdArgs& args, declType_t type );
	virtual void				PrintType( const idCmdArgs& args, declType_t type );

	virtual idDecl* 			CreateNewDecl( declType_t type, const char* name, const char* fileName );

	//BSM Added for the material editors rename capabilities
	virtual bool				RenameDecl( declType_t type, const char* oldName, const char* newName );

	virtual void				MediaPrint( VERIFY_FORMAT_STRING const char* fmt, ... ) ID_INSTANCE_ATTRIBUTE_PRINTF( 1, 2 );
	virtual void				WritePrecacheCommands( idFile* f );

	virtual const idMaterial* 		FindMaterial( const char* name, bool makeDefault = true );
	virtual const idMaterial* 		MaterialByIndex( int index, bool forceParse = true );

	virtual const idDeclSkin* 		FindSkin( const char* name, bool makeDefault = true );
	virtual const idDeclSkin* 		SkinByIndex( int index, bool forceParse = true );

#if !defined( DMAP )
	virtual const idSoundShader* 	FindSound( const char* name, bool makeDefault = true );
	virtual const idSoundShader* 	SoundByIndex( int index, bool forceParse = true );
#endif

	virtual void					Touch( const idDecl* decl );

public:
	static void					MakeNameCanonical( const char* name, char* result, int maxLength );
	idDeclLocal* 				FindTypeWithoutParsing( declType_t type, const char* name, bool makeDefault = true );

	idDeclType* 				GetDeclType( int type ) const
	{
		return declTypes[type];
	}
	const idDeclFile* 			GetImplicitDeclFile() const
	{
		return &implicitDecls;
	}

	void						ConvertPDAsToStrings( const idCmdArgs& args );

private:
	idSysMutex					mutex;

	idList<idDeclType*, TAG_IDLIB_LIST_DECL>		declTypes;
	idList<idDeclFolder*, TAG_IDLIB_LIST_DECL>		declFolders;

	idList<idDeclFile*, TAG_IDLIB_LIST_DECL>		loadedFiles;
	idHashIndex					hashTables[DECL_MAX_TYPES];
	idList<idDeclLocal*, TAG_IDLIB_LIST_DECL>		linearLists[DECL_MAX_TYPES];
	idDeclFile					implicitDecls;	// this holds all the decls that were created because explicit
	// text definitions were not found. Decls that became default
	// because of a parse error are not in this list.
	int							checksum;		// checksum of all loaded decl text
	int							indent;			// for MediaPrint
	bool						insideLevelLoad;

	static idCVar				decl_show;

private:
	static void					ListDecls_f( const idCmdArgs& args );
	static void					ReloadDecls_f( const idCmdArgs& args );
	static void					TouchDecl_f( const idCmdArgs& args );
	// RB begin
	static void                 ExportEntityDefsToBlender_f( const idCmdArgs& args );
	static void                 ExportMaterialsToBlender_f( const idCmdArgs& args );
	static void                 ExportEntityDefsToTrenchBroom_f( const idCmdArgs& args );
	static void                 ExportModelsToTrenchBroom_f( const idCmdArgs& args );
	static void                 ExportImagesToTrenchBroom_f( const idCmdArgs& args );

	static void                 MakeZooMapForModels_f( const idCmdArgs& args );
	// RB end
};
// ==== engine neo/framework/DeclManager.cpp lines 286-286 (idDeclManagerLocal::decl_show) ====
idCVar idDeclManagerLocal::decl_show( "decl_show", "0", CVAR_SYSTEM, "set to 1 to print parses, 2 to also print references", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
// ==== engine neo/framework/DeclManager.cpp lines 622-887 (idDeclFile constructors, Reload, LoadAndParse) ====
/*
================
idDeclFile::idDeclFile
================
*/
idDeclFile::idDeclFile( const char* fileName, declType_t defaultType )
{
	this->fileName = fileName;
	this->defaultType = defaultType;
	this->timestamp = 0;
	this->checksum = 0;
	this->fileSize = 0;
	this->numLines = 0;
	this->decls = NULL;
}

/*
================
idDeclFile::idDeclFile
================
*/
idDeclFile::idDeclFile()
{
	this->fileName = "<implicit file>";
	this->defaultType = DECL_MAX_TYPES;
	this->timestamp = 0;
	this->checksum = 0;
	this->fileSize = 0;
	this->numLines = 0;
	this->decls = NULL;
}

/*
================
idDeclFile::Reload

ForceReload will cause it to reload even if the timestamp hasn't changed
================
*/
void idDeclFile::Reload( bool force )
{
	// check for an unchanged timestamp
	if( !force && timestamp != 0 )
	{
		ID_TIME_T	testTimeStamp;
		fileSystem->ReadFile( fileName, NULL, &testTimeStamp );

		if( testTimeStamp == timestamp )
		{
			return;
		}
	}

	// parse the text
	LoadAndParse();
}

/*
================
idDeclFile::LoadAndParse

This is used during both the initial load, and any reloads
================
*/
int c_savedMemory = 0;

int idDeclFile::LoadAndParse()
{
	int			i, numTypes;
	idLexer		src;
	idToken		token;
	int			startMarker;
	char* 		buffer;
	int			length, size;
	int			sourceLine;
	idStr		name;
	idDeclLocal* newDecl;
	bool		reparse;

	// load the text
	common->DPrintf( "...loading '%s'\n", fileName.c_str() );
	length = fileSystem->ReadFile( fileName, ( void** )&buffer, &timestamp );
	if( length == -1 )
	{
		common->FatalError( "couldn't load %s", fileName.c_str() );
		return 0;
	}

	if( !src.LoadMemory( buffer, length, fileName ) )
	{
		common->Error( "Couldn't parse %s", fileName.c_str() );
		Mem_Free( buffer );
		return 0;
	}

	// mark all the defs that were from the last reload of this file
	for( idDeclLocal* decl = decls; decl; decl = decl->nextInFile )
	{
		decl->redefinedInReload = false;
	}

	src.SetFlags( DECL_LEXER_FLAGS );

	checksum = MD5_BlockChecksum( buffer, length );

	fileSize = length;

	// scan through, identifying each individual declaration
	while( 1 )
	{

		startMarker = src.GetFileOffset();
		sourceLine = src.GetLineNum();

		// parse the decl type name
		if( !src.ReadToken( &token ) )
		{
			break;
		}

		declType_t identifiedType = DECL_MAX_TYPES;

		// get the decl type from the type name
		numTypes = declManagerLocal.GetNumDeclTypes();
		for( i = 0; i < numTypes; i++ )
		{
			idDeclType* typeInfo = declManagerLocal.GetDeclType( i );
			if( typeInfo != NULL && typeInfo->typeName.Icmp( token ) == 0 )
			{
				identifiedType = ( declType_t ) typeInfo->type;
				break;
			}
		}

		if( i >= numTypes )
		{

			if( token.Icmp( "{" ) == 0 )
			{

				// if we ever see an open brace, we somehow missed the [type] <name> prefix
				src.Warning( "Missing decl name" );
				src.SkipBracedSection( false );
				continue;

			}
			else
			{

				if( defaultType == DECL_MAX_TYPES )
				{
					src.Warning( "No type" );
					continue;
				}
				src.UnreadToken( &token );
				// use the default type
				identifiedType = defaultType;
			}
		}

		// now parse the name
		if( !src.ReadToken( &token ) )
		{
			src.Warning( "Type without definition at end of file" );
			break;
		}

		if( !token.Icmp( "{" ) )
		{
			// if we ever see an open brace, we somehow missed the [type] <name> prefix
			src.Warning( "Missing decl name" );
			src.SkipBracedSection( false );
			continue;
		}

		// FIXME: export decls are only used by the model exporter, they are skipped here for now
		if( identifiedType == DECL_MODELEXPORT )
		{
			src.SkipBracedSection();
			continue;
		}

		name = token;

		// make sure there's a '{'
		if( !src.ReadToken( &token ) )
		{
			src.Warning( "Type without definition at end of file" );
			break;
		}
		if( token != "{" )
		{
			src.Warning( "Expecting '{' but found '%s'", token.c_str() );
			continue;
		}
		src.UnreadToken( &token );

		// now take everything until a matched closing brace
		src.SkipBracedSection();
		size = src.GetFileOffset() - startMarker;

		// look it up, possibly getting a newly created default decl
		reparse = false;
		newDecl = declManagerLocal.FindTypeWithoutParsing( identifiedType, name, false );
		if( newDecl )
		{
			// update the existing copy
			if( newDecl->sourceFile != this || newDecl->redefinedInReload )
			{
				src.Warning( "%s '%s' previously defined at %s:%i", declManagerLocal.GetDeclNameFromType( identifiedType ),
							 name.c_str(), newDecl->sourceFile->fileName.c_str(), newDecl->sourceLine );
				continue;
			}
			if( newDecl->declState != DS_UNPARSED )
			{
				reparse = true;
			}
		}
		else
		{
			// allow it to be created as a default, then add it to the per-file list
			newDecl = declManagerLocal.FindTypeWithoutParsing( identifiedType, name, true );
			newDecl->nextInFile = this->decls;
			this->decls = newDecl;
		}

		newDecl->redefinedInReload = true;

		if( newDecl->textSource )
		{
			Mem_Free( newDecl->textSource );
			newDecl->textSource = NULL;
		}

		newDecl->SetTextLocal( buffer + startMarker, size );
		newDecl->sourceFile = this;
		newDecl->sourceTextOffset = startMarker;
		newDecl->sourceTextLength = size;
		newDecl->sourceLine = sourceLine;
		newDecl->declState = DS_UNPARSED;

		// if it is currently in use, reparse it immedaitely
		if( reparse )
		{
			newDecl->ParseLocal();
		}
	}

	numLines = src.GetLineNum();

	Mem_Free( buffer );

	// any defs that weren't redefinedInReload should now be defaulted
	for( idDeclLocal* decl = decls ; decl ; decl = decl->nextInFile )
	{
		if( decl->redefinedInReload == false )
		{
			decl->MakeDefault();
			decl->sourceTextOffset = decl->sourceFile->fileSize;
			decl->sourceTextLength = 0;
			decl->sourceLine = decl->sourceFile->numLines;
		}
	}

	return checksum;
}
// ==== engine neo/framework/DeclManager.cpp lines 1083-1221 (idDeclManagerLocal::Reload, BeginLevelLoad, EndLevelLoad, RegisterDeclType, RegisterDeclFolder) ====
/*
===================
idDeclManagerLocal::Reload
===================
*/
void idDeclManagerLocal::Reload( bool force )
{
	for( int i = 0; i < loadedFiles.Num(); i++ )
	{
		loadedFiles[i]->Reload( force );
	}
}

/*
===================
idDeclManagerLocal::BeginLevelLoad
===================
*/
void idDeclManagerLocal::BeginLevelLoad()
{
	insideLevelLoad = true;

	// clear all the referencedThisLevel flags and purge all the data
	// so the next reference will cause a reparse
	for( int i = 0; i < DECL_MAX_TYPES; i++ )
	{
		int	num = linearLists[i].Num();
		for( int j = 0 ; j < num ; j++ )
		{
			idDeclLocal* decl = linearLists[i][j];
			decl->Purge();
		}
	}
}

/*
===================
idDeclManagerLocal::EndLevelLoad
===================
*/
void idDeclManagerLocal::EndLevelLoad()
{
	insideLevelLoad = false;

	// we don't need to do anything here, but the image manager, model manager,
	// and sound sample manager will need to free media that was not referenced
}

/*
===================
idDeclManagerLocal::RegisterDeclType
===================
*/
void idDeclManagerLocal::RegisterDeclType( const char* typeName, declType_t type, idDecl * ( *allocator )() )
{
	idDeclType* declType;

	if( type < declTypes.Num() && declTypes[( int )type] )
	{
		common->Warning( "idDeclManager::RegisterDeclType: type '%s' already exists", typeName );
		return;
	}

	declType = new( TAG_DECL ) idDeclType;
	declType->typeName = typeName;
	declType->type = type;
	declType->allocator = allocator;

	if( ( int )type + 1 > declTypes.Num() )
	{
		declTypes.AssureSize( ( int )type + 1, NULL );
	}
	declTypes[type] = declType;
}

/*
===================
idDeclManagerLocal::RegisterDeclFolder
===================
*/
void idDeclManagerLocal::RegisterDeclFolder( const char* folder, const char* extension, declType_t defaultType )
{
	int i, j;
	idStr fileName;
	idDeclFolder* declFolder;
	idFileList* fileList;
	idDeclFile* df;

	// check whether this folder / extension combination already exists
	for( i = 0; i < declFolders.Num(); i++ )
	{
		if( declFolders[i]->folder.Icmp( folder ) == 0 && declFolders[i]->extension.Icmp( extension ) == 0 )
		{
			break;
		}
	}
	if( i < declFolders.Num() )
	{
		declFolder = declFolders[i];
	}
	else
	{
		declFolder = new( TAG_DECL ) idDeclFolder;
		declFolder->folder = folder;
		declFolder->extension = extension;
		declFolder->defaultType = defaultType;
		declFolders.Append( declFolder );
	}

	// scan for decl files
	fileList = fileSystem->ListFiles( declFolder->folder, declFolder->extension, true );

	// load and parse decl files
	for( i = 0; i < fileList->GetNumFiles(); i++ )
	{
		fileName = declFolder->folder + "/" + fileList->GetFile( i );

		// check whether this file has already been loaded
		for( j = 0; j < loadedFiles.Num(); j++ )
		{
			if( fileName.Icmp( loadedFiles[j]->fileName ) == 0 )
			{
				break;
			}
		}
		if( j < loadedFiles.Num() )
		{
			df = loadedFiles[j];
		}
		else
		{
			df = new( TAG_DECL ) idDeclFile( fileName, defaultType );
			loadedFiles.Append( df );
		}
		df->LoadAndParse();
	}

	fileSystem->FreeFileList( fileList );
}
// ==== engine neo/framework/DeclManager.cpp lines 1273-1450 (GetNumDeclTypes, GetDeclNameFromType, GetDeclTypeFromName, FindType, FindDeclWithoutParsing, ReloadFile, GetNumDecls, DeclByIndex) ====
/*
===================
idDeclManagerLocal::GetNumDeclTypes
===================
*/
int idDeclManagerLocal::GetNumDeclTypes() const
{
	return declTypes.Num();
}

/*
===================
idDeclManagerLocal::GetDeclNameFromType
===================
*/
const char* idDeclManagerLocal::GetDeclNameFromType( declType_t type ) const
{
	int typeIndex = ( int )type;

	if( typeIndex < 0 || typeIndex >= declTypes.Num() || declTypes[typeIndex] == NULL )
	{
		common->FatalError( "idDeclManager::GetDeclNameFromType: bad type: %i", typeIndex );
	}
	return declTypes[typeIndex]->typeName;
}

/*
===================
idDeclManagerLocal::GetDeclTypeFromName
===================
*/
declType_t idDeclManagerLocal::GetDeclTypeFromName( const char* typeName ) const
{
	int i;

	for( i = 0; i < declTypes.Num(); i++ )
	{
		if( declTypes[i] && declTypes[i]->typeName.Icmp( typeName ) == 0 )
		{
			return ( declType_t )declTypes[i]->type;
		}
	}
	return DECL_MAX_TYPES;
}

/*
=================
idDeclManagerLocal::FindType

External users will always cause the decl to be parsed before returning
=================
*/
const idDecl* idDeclManagerLocal::FindType( declType_t type, const char* name, bool makeDefault )
{
	idDeclLocal* decl;

	idScopedCriticalSection cs( mutex );

	if( !name || !name[0] )
	{
		name = "_emptyName";
		//common->Warning( "idDeclManager::FindType: empty %s name", GetDeclType( (int)type )->typeName.c_str() );
	}

	decl = FindTypeWithoutParsing( type, name, makeDefault );
	if( !decl )
	{
		return NULL;
	}

	decl->AllocateSelf();

	// if it hasn't been parsed yet, parse it now
	if( decl->declState == DS_UNPARSED )
	{
		// GK: Let's hope I didn't break anything important with that
		// RB answer: this should be no issue with Vulkan but could remain a problem with OpenGL
#if 0
		if( !idLib::IsMainThread() )
		{
			// we can't load images from a background thread on OpenGL,
			// the renderer on the main thread should parse it if needed
			idLib::Error( "Attempted to load %s decl '%s' from game thread!", GetDeclNameFromType( type ), name );
		}
#endif
		decl->ParseLocal();

		// SRS - set non-purgeable flag only after ParseLocal(), don't reset if declState is parsed or defaulted
		decl->parsedOutsideLevelLoad = !insideLevelLoad;
	}

	// mark it as referenced
	decl->referencedThisLevel = true;
	decl->everReferenced = true;

	return decl->self;
}

/*
===============
idDeclManagerLocal::FindDeclWithoutParsing
===============
*/
const idDecl* idDeclManagerLocal::FindDeclWithoutParsing( declType_t type, const char* name, bool makeDefault )
{
	idDeclLocal* decl;
	decl = FindTypeWithoutParsing( type, name, makeDefault );
	if( decl )
	{
		return decl->self;
	}
	return NULL;
}

/*
===============
idDeclManagerLocal::ReloadFile
===============
*/
void idDeclManagerLocal::ReloadFile( const char* filename, bool force )
{
	for( int i = 0; i < loadedFiles.Num(); i++ )
	{
		if( !loadedFiles[i]->fileName.Icmp( filename ) )
		{
			checksum ^= loadedFiles[i]->checksum;
			loadedFiles[i]->Reload( force );
			checksum ^= loadedFiles[i]->checksum;
		}
	}
}

/*
===================
idDeclManagerLocal::GetNumDecls
===================
*/
int idDeclManagerLocal::GetNumDecls( declType_t type )
{
	int typeIndex = ( int )type;

	if( typeIndex < 0 || typeIndex >= declTypes.Num() || declTypes[typeIndex] == NULL )
	{
		common->FatalError( "idDeclManager::GetNumDecls: bad type: %i", typeIndex );
		return 0;
	}
	return linearLists[ typeIndex ].Num();
}

/*
===================
idDeclManagerLocal::DeclByIndex
===================
*/
const idDecl* idDeclManagerLocal::DeclByIndex( declType_t type, int index, bool forceParse )
{
	int typeIndex = ( int )type;

	if( typeIndex < 0 || typeIndex >= declTypes.Num() || declTypes[typeIndex] == NULL )
	{
		common->FatalError( "idDeclManager::DeclByIndex: bad type: %i", typeIndex );
		return NULL;
	}
	if( index < 0 || index >= linearLists[ typeIndex ].Num() )
	{
		common->Error( "idDeclManager::DeclByIndex: out of range" );
	}
	idDeclLocal* decl = linearLists[ typeIndex ][ index ];

	decl->AllocateSelf();

	if( forceParse && decl->declState == DS_UNPARSED )
	{
		decl->ParseLocal();
	}

	return decl->self;
}
// ==== engine neo/framework/DeclManager.cpp lines 1761-1786 (idDeclManagerLocal::MediaPrint) ====
/*
===================
idDeclManagerLocal::MediaPrint

This is just used to nicely indent media caching prints
===================
*/
void idDeclManagerLocal::MediaPrint( const char* fmt, ... )
{
	if( !decl_show.GetInteger() )
	{
		return;
	}
	for( int i = 0 ; i < indent ; i++ )
	{
		common->Printf( "    " );
	}
	va_list		argptr;
	char		buffer[1024];
	va_start( argptr, fmt );
	idStr::vsnPrintf( buffer, sizeof( buffer ), fmt, argptr );
	va_end( argptr );
	buffer[sizeof( buffer ) - 1] = '\0';

	common->Printf( "%s", buffer );
}
// ==== engine neo/framework/DeclManager.cpp lines 1823-1913 (FindMaterial, MaterialByIndex, FindSkin, SkinByIndex, FindSound, SoundByIndex, Touch, MakeNameCanonical) ====
/********************************************************************/

const idMaterial* idDeclManagerLocal::FindMaterial( const char* name, bool makeDefault )
{
	return static_cast<const idMaterial*>( FindType( DECL_MATERIAL, name, makeDefault ) );
}

const idMaterial* idDeclManagerLocal::MaterialByIndex( int index, bool forceParse )
{
	return static_cast<const idMaterial*>( DeclByIndex( DECL_MATERIAL, index, forceParse ) );
}

/********************************************************************/

const idDeclSkin* idDeclManagerLocal::FindSkin( const char* name, bool makeDefault )
{
	return static_cast<const idDeclSkin*>( FindType( DECL_SKIN, name, makeDefault ) );
}

const idDeclSkin* idDeclManagerLocal::SkinByIndex( int index, bool forceParse )
{
	return static_cast<const idDeclSkin*>( DeclByIndex( DECL_SKIN, index, forceParse ) );
}

#if !defined( DMAP )

/********************************************************************/

const idSoundShader* idDeclManagerLocal::FindSound( const char* name, bool makeDefault )
{
	return static_cast<const idSoundShader*>( FindType( DECL_SOUND, name, makeDefault ) );
}

const idSoundShader* idDeclManagerLocal::SoundByIndex( int index, bool forceParse )
{
	return static_cast<const idSoundShader*>( DeclByIndex( DECL_SOUND, index, forceParse ) );
}

#endif

/*
===================
idDeclManagerLocal::Touch
===================
*/
void idDeclManagerLocal::Touch( const idDecl* decl )
{

	if( decl->base->GetState() ==  DS_UNPARSED )
	{
		// This should parse the decl as well.
		FindType( decl->GetType(), decl->GetName() );
	}
}

/*
===================
idDeclManagerLocal::MakeNameCanonical
===================
*/
void idDeclManagerLocal::MakeNameCanonical( const char* name, char* result, int maxLength )
{
	int i, lastDot;

	lastDot = -1;
	for( i = 0; i < maxLength && name[i] != '\0'; i++ )
	{
		int c = name[i];
		if( c == '\\' )
		{
			result[i] = '/';
		}
		else if( c == '.' )
		{
			lastDot = i;
			result[i] = c;
		}
		else
		{
			result[i] = idStr::ToLower( c );
		}
	}
	if( lastDot != -1 )
	{
		result[lastDot] = '\0';
	}
	else
	{
		result[i] = '\0';
	}
}
// ==== engine neo/framework/DeclManager.cpp lines 4675-4735 (idDeclManagerLocal::FindTypeWithoutParsing) ====
/*
===================
idDeclManagerLocal::FindTypeWithoutParsing

This finds or creats the decl, but does not cause a parse.  This is only used internally.
===================
*/
idDeclLocal* idDeclManagerLocal::FindTypeWithoutParsing( declType_t type, const char* name, bool makeDefault )
{
	int typeIndex = ( int )type;
	int i, hash;

	if( typeIndex < 0 || typeIndex >= declTypes.Num() || declTypes[typeIndex] == NULL || typeIndex >= DECL_MAX_TYPES )
	{
		common->FatalError( "idDeclManager::FindTypeWithoutParsing: bad type: %i", typeIndex );
		return NULL;
	}

	char canonicalName[MAX_STRING_CHARS];

	MakeNameCanonical( name, canonicalName, sizeof( canonicalName ) );

	// see if it already exists
	hash = hashTables[typeIndex].GenerateKey( canonicalName, false );
	for( i = hashTables[typeIndex].First( hash ); i >= 0; i = hashTables[typeIndex].Next( i ) )
	{
		if( linearLists[typeIndex][i]->name.Icmp( canonicalName ) == 0 )
		{
			// only print these when decl_show is set to 2, because it can be a lot of clutter
			if( decl_show.GetInteger() > 1 )
			{
				MediaPrint( "referencing %s %s\n", declTypes[ type ]->typeName.c_str(), name );
			}
			return linearLists[typeIndex][i];
		}
	}

	if( !makeDefault )
	{
		return NULL;
	}

	idDeclLocal* decl = new( TAG_DECL ) idDeclLocal;
	decl->self = NULL;
	decl->name = canonicalName;
	decl->type = type;
	decl->declState = DS_UNPARSED;
	decl->textSource = NULL;
	decl->textLength = 0;
	decl->sourceFile = &implicitDecls;
	decl->referencedThisLevel = false;
	decl->everReferenced = false;
	// SRS - initialize to false, otherwise all decls will be set to non-purgeable during Init()
	decl->parsedOutsideLevelLoad = false;	// !insideLevelLoad;

	// add it to the linear list and hash table
	decl->index = linearLists[typeIndex].Num();
	hashTables[typeIndex].Add( hash, linearLists[typeIndex].Append( decl ) );

	return decl;
}
// ==== engine neo/framework/DeclManager.cpp lines 4922-5135 (idDeclLocal constructor through SetTextLocal) ====
/*
====================================================================================

	idDeclLocal

====================================================================================
*/

/*
=================
idDeclLocal::idDeclLocal
=================
*/
idDeclLocal::idDeclLocal()
{
	name = "unnamed";
	textSource = NULL;
	textLength = 0;
	compressedLength = 0;
	sourceFile = NULL;
	sourceTextOffset = 0;
	sourceTextLength = 0;
	sourceLine = 0;
	checksum = 0;
	type = DECL_ENTITYDEF;
	index = 0;
	declState = DS_UNPARSED;
	parsedOutsideLevelLoad = false;
	referencedThisLevel = false;
	everReferenced = false;
	redefinedInReload = false;
	nextInFile = NULL;
}

/*
=================
idDeclLocal::GetName
=================
*/
const char* idDeclLocal::GetName() const
{
	return name.c_str();
}

/*
=================
idDeclLocal::GetType
=================
*/
declType_t idDeclLocal::GetType() const
{
	return type;
}

/*
=================
idDeclLocal::GetState
=================
*/
declState_t idDeclLocal::GetState() const
{
	return declState;
}

/*
=================
idDeclLocal::IsImplicit
=================
*/
bool idDeclLocal::IsImplicit() const
{
	return ( sourceFile == declManagerLocal.GetImplicitDeclFile() );
}

/*
=================
idDeclLocal::IsValid
=================
*/
bool idDeclLocal::IsValid() const
{
	return ( declState != DS_UNPARSED );
}

/*
=================
idDeclLocal::Invalidate
=================
*/
void idDeclLocal::Invalidate()
{
	declState = DS_UNPARSED;
}

/*
=================
idDeclLocal::EnsureNotPurged
=================
*/
void idDeclLocal::EnsureNotPurged()
{
	if( declState == DS_UNPARSED )
	{
		ParseLocal();
	}
}

/*
=================
idDeclLocal::Index
=================
*/
int idDeclLocal::Index() const
{
	return index;
}

/*
=================
idDeclLocal::GetLineNum
=================
*/
int idDeclLocal::GetLineNum() const
{
	return sourceLine;
}

/*
=================
idDeclLocal::GetFileName
=================
*/
const char* idDeclLocal::GetFileName() const
{
	return ( sourceFile ) ? sourceFile->fileName.c_str() : "*invalid*";
}

/*
=================
idDeclLocal::Size
=================
*/
size_t idDeclLocal::Size() const
{
	return sizeof( idDecl ) + name.Allocated();
}

/*
=================
idDeclLocal::GetText
=================
*/
void idDeclLocal::GetText( char* text ) const
{
#ifdef USE_COMPRESSED_DECLS
	HuffmanDecompressText( text, textLength, ( byte* )textSource, compressedLength );
#else
	memcpy( text, textSource, textLength + 1 );
#endif
}

/*
=================
idDeclLocal::GetTextLength
=================
*/
int idDeclLocal::GetTextLength() const
{
	return textLength;
}

/*
=================
idDeclLocal::SetText
=================
*/
void idDeclLocal::SetText( const char* text )
{
	SetTextLocal( text, idStr::Length( text ) );
}

/*
=================
idDeclLocal::SetTextLocal
=================
*/
void idDeclLocal::SetTextLocal( const char* text, const int length )
{

	Mem_Free( textSource );

	checksum = MD5_BlockChecksum( text, length );

#ifdef GET_HUFFMAN_FREQUENCIES
	for( int i = 0; i < length; i++ )
	{
		huffmanFrequencies[( ( const unsigned char* )text )[i]]++;
	}
#endif

#ifdef USE_COMPRESSED_DECLS
	int maxBytesPerCode = ( maxHuffmanBits + 7 ) >> 3;
	byte* compressed = ( byte* )_alloca( length * maxBytesPerCode );
	compressedLength = HuffmanCompressText( text, length, compressed, length * maxBytesPerCode );
	textSource = ( char* )Mem_Alloc( compressedLength, TAG_DECLTEXT );
	memcpy( textSource, compressed, compressedLength );
#else
	compressedLength = length;
	textSource = ( char* ) Mem_Alloc( length + 1, TAG_DECLTEXT );
	memcpy( textSource, text, length );
	textSource[length] = '\0';
#endif
	textLength = length;
}
// ==== engine neo/framework/DeclManager.cpp lines 5224-5471 (idDeclLocal::SourceFileChanged through EverReferenced) ====
/*
=================
idDeclLocal::SourceFileChanged
=================
*/
bool idDeclLocal::SourceFileChanged() const
{
	int newLength;
	ID_TIME_T newTimestamp;

	if( sourceFile->fileSize <= 0 )
	{
		return false;
	}

	newLength = fileSystem->ReadFile( GetFileName(), NULL, &newTimestamp );

	if( newLength != sourceFile->fileSize || newTimestamp != sourceFile->timestamp )
	{
		return true;
	}

	return false;
}

// RB begin
ID_TIME_T idDeclLocal::GetSourceFileTimestamp() const
{
	ID_TIME_T newTimestamp;
	int newLength = fileSystem->ReadFile( GetFileName(), NULL, &newTimestamp );

	if( newLength <= 0 )
	{
		if( sourceFile->fileSize <= 0 )
		{
			return FILE_NOT_FOUND_TIMESTAMP;
		}
	}

	return newTimestamp;
}
// RB end

/*
=================
idDeclLocal::MakeDefault
=================
*/
void idDeclLocal::MakeDefault()
{
	static int recursionLevel;
	const char* defaultText;

	declManagerLocal.MediaPrint( "DEFAULTED\n" );
	declState = DS_DEFAULTED;

	AllocateSelf();

	defaultText = self->DefaultDefinition();

	// a parse error inside a DefaultDefinition() string could
	// cause an infinite loop, but normal default definitions could
	// still reference other default definitions, so we can't
	// just dump out on the first recursion
	if( ++recursionLevel > 100 )
	{
		common->FatalError( "idDecl::MakeDefault: bad DefaultDefinition(): %s", defaultText );
	}

	// always free data before parsing
	self->FreeData();

	// parse
	self->Parse( defaultText, strlen( defaultText ), false );

	// we could still eventually hit the recursion if we have enough Error() calls inside Parse...
	--recursionLevel;
}

/*
=================
idDeclLocal::SetDefaultText
=================
*/
bool idDeclLocal::SetDefaultText()
{
	return false;
}

/*
=================
idDeclLocal::DefaultDefinition
=================
*/
const char* idDeclLocal::DefaultDefinition() const
{
	return "{ }";
}

/*
=================
idDeclLocal::Parse
=================
*/
bool idDeclLocal::Parse( const char* text, const int textLength, bool allowBinaryVersion )
{
	idLexer src;

	src.LoadMemory( text, textLength, GetFileName(), GetLineNum() );
	src.SetFlags( DECL_LEXER_FLAGS );
	src.SkipUntilString( "{" );
	src.SkipBracedSection( false );
	return true;
}

/*
=================
idDeclLocal::FreeData
=================
*/
void idDeclLocal::FreeData()
{
}

/*
=================
idDeclLocal::List
=================
*/
void idDeclLocal::List() const
{
	common->Printf( "%s\n", GetName() );
}

/*
=================
idDeclLocal::Print
=================
*/
void idDeclLocal::Print() const
{
}

/*
=================
idDeclLocal::Reload
=================
*/
void idDeclLocal::Reload()
{
	this->sourceFile->Reload( false );
}

/*
=================
idDeclLocal::AllocateSelf
=================
*/
void idDeclLocal::AllocateSelf()
{
	if( self == NULL )
	{
		self = declManagerLocal.GetDeclType( ( int )type )->allocator();
		self->base = this;
	}
}

/*
=================
idDeclLocal::ParseLocal
=================
*/
void idDeclLocal::ParseLocal()
{
	bool generatedDefaultText = false;

	AllocateSelf();

	// always free data before parsing
	self->FreeData();

	declManagerLocal.MediaPrint( "parsing %s %s\n", declManagerLocal.declTypes[type]->typeName.c_str(), name.c_str() );

	// if no text source try to generate default text
	if( textSource == NULL )
	{
		generatedDefaultText = self->SetDefaultText();
	}

	// indent for DEFAULTED or media file references
	declManagerLocal.indent++;

	// no text immediately causes a MakeDefault()
	if( textSource == NULL )
	{
		MakeDefault();
		declManagerLocal.indent--;
		return;
	}

	declState = DS_PARSED;

	// parse
	char* declText = ( char* ) _alloca( ( GetTextLength() + 1 ) * sizeof( char ) );
	GetText( declText );
	self->Parse( declText, GetTextLength(), true );

	// free generated text
	if( generatedDefaultText )
	{
		Mem_Free( textSource );
		textSource = NULL;
		textLength = 0;
	}

	declManagerLocal.indent--;
}

/*
=================
idDeclLocal::Purge
=================
*/
void idDeclLocal::Purge()
{
	// never purge things that were referenced outside level load,
	// like the console and menu graphics
	if( parsedOutsideLevelLoad )
	{
		return;
	}

	referencedThisLevel = false;
	MakeDefault();

	// the next Find() for this will re-parse the real data
	declState = DS_UNPARSED;
}

/*
=================
idDeclLocal::EverReferenced
=================
*/
bool idDeclLocal::EverReferenced() const
{
	return everReferenced;
}
