/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*
*	This product contains software technology licensed from Id
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc.
*	All Rights Reserved.
*
****/
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <memory>

#include "common/ByteSwap.h"
#include "Platform.h"

#include "Engine.h"
#include "FileSystem2.h"
#include "CFile.h"

#include "WadIO.h"

wadinfo_t* LoadWadFile( const char* const pszFileName )
{
	assert( pszFileName );

	CFile file( pszFileName, "rb" );

	if( !file.IsOpen() )
	{
		printf( "LoadWadFile: Couldn't open WAD \"%s\"\n", pszFileName );
		return nullptr;
	}

	const auto size = file.Size();

	wadinfo_t* pWad = reinterpret_cast<wadinfo_t*>( new uint8_t[ size ] );

	const auto read = file.Read( pWad, size );

	file.Close();

	if( read != size )
	{
		printf( "LoadWadFile: Failed to read WAD file \"%s\" (expected %u, got %u)\n", pszFileName, size, read );
		delete[] pWad;

		return nullptr;
	}

	if( strncmp( WAD2_ID, pWad->identification, 4 ) && strncmp( WAD3_ID, pWad->identification, 4 ) )
	{
		printf( "LoadWadFile: File \"%s\" is not a WAD2 or WAD3 file\n", pszFileName );
		delete[] pWad;
		return nullptr;
	}

	pWad->infotableofs = LittleValue( pWad->infotableofs );
	pWad->numlumps = LittleValue( pWad->numlumps );

	lumpinfo_t* pLump = reinterpret_cast<lumpinfo_t*>( reinterpret_cast<uint8_t*>( pWad ) + pWad->infotableofs );

	//Swap all variables, clean up names.
	for( int iLump = 0; iLump < pWad->numlumps; ++iLump, ++pLump )
	{
		CleanupWadLumpName( pLump->name, pLump->name, sizeof( pLump->name ) );

		pLump->filepos		= LittleValue( pLump->filepos );
		pLump->disksize		= LittleValue( pLump->disksize );
		pLump->size			= LittleValue( pLump->size );
	}

	return pWad;
}

void CleanupWadLumpName( const char* in, char* out, const size_t uiBufferSize )
{
	//Must be large enough to fit at least an entire name.
	assert( uiBufferSize >= WAD_MAX_LUMP_NAME_SIZE );

	size_t i;

	for( i = 0; i< WAD_MAX_LUMP_NAME_SIZE; i++ )
	{
		if( !in[ i ] )
			break;

		out[ i ] = toupper( in[ i ] );
	}

	//Fill the rest of the buffer with 0.
	memset( out + i, 0, uiBufferSize - i );
}

const miptex_t* Wad_FindTexture( const wadinfo_t* pWad, const char* const pszName )
{
	assert( pWad );
	assert( pszName );

	if( !pWad || !pszName )
		return nullptr;

	const lumpinfo_t* pLump = reinterpret_cast<const lumpinfo_t*>( reinterpret_cast<const uint8_t*>( pWad ) + pWad->infotableofs );

	for( int iLump = 0; iLump < pWad->numlumps; ++iLump, ++pLump )
	{
		//Found it.
		if( stricmp( pszName, pLump->name ) == 0 )
		{
			//Must be a miptex lump.
			if( pLump->type != ( TYP_LUMPY + TYP_LUMPY_MIPTEX ) )
			{
				return nullptr;
			}

			//Compression isn't used, so just return it.
			return reinterpret_cast<const miptex_t*>( reinterpret_cast<const uint8_t*>( pWad ) + pLump->filepos );
		}
	}

	return nullptr;
}