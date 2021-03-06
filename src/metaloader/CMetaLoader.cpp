#include "Common.h"
#include "FilePaths.h"

//Must be included before FileSystem2.h because conflicting steamtypes.h headers exist. - Solokiller
#include "steam_api.h"

#include "FileSystem2.h"
#include "Logging.h"
#include "IMetaTool.h"

#include "steam/SteamWrapper.h"

#include "CMetaLoader.h"

namespace fs = std::experimental::filesystem;

CMetaLoader g_MetaLoader;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CMetaLoader, IMetaLoader, IMETALOADER_NAME, g_MetaLoader );

namespace
{
static const char* const STRIP_COMMANDS[] = 
{
	"-game",	//Always strip -game so it can be reused.
	nullptr
};
}

const char* CMetaLoader::GetGameDirectory( char* pszDest, size_t uiSizeInCharacters ) const
{
	if( !pszDest || uiSizeInCharacters <= 0 )
		return nullptr;

	if( strlen( m_szGameDir ) >= uiSizeInCharacters )
		return nullptr;

	strncpy( pszDest, m_szGameDir, uiSizeInCharacters );
	pszDest[ uiSizeInCharacters - 1 ] = '\0';

	return pszDest;
}

const char* CMetaLoader::GetToolDirectory( char* pszDest, size_t uiSizeInCharacters ) const
{
	if( !pszDest || uiSizeInCharacters <= 0 )
		return nullptr;

	if( strlen( m_szToolDir ) >= uiSizeInCharacters )
		return nullptr;

	strncpy( pszDest, m_szToolDir, uiSizeInCharacters );
	pszDest[ uiSizeInCharacters - 1 ] = '\0';

	return pszDest;
}

SDL_Window* CMetaLoader::GetEngineWindow() const
{
	return m_pEngineWindow;
}

void CMetaLoader::SetGameDirectory( const char* pszGameDir )
{
	ASSERT( pszGameDir );

	strncpy( m_szGameDir, pszGameDir, sizeof( m_szGameDir ) );
	m_szGameDir[ sizeof( m_szGameDir ) - 1 ] = '\0';
}

void CMetaLoader::Run( const bool bIsListenServer )
{
	m_bIsListenServer = bIsListenServer;

	const bool bResult = RunLoader();

	Shutdown();

	exit( bResult ? EXIT_SUCCESS : EXIT_FAILURE );
}

bool CMetaLoader::RunLoader()
{
	//TODO: until we can draw the console onscreen, use a console window. - Solokiller
#ifdef WIN32
	AllocConsole();
	freopen( "CONOUT$", "w", stdout );
#endif

	if( !( *m_szGameDir ) )
	{
		UTIL_ShowMessageBox( "No game directory set", "Error", LogType::ERROR );
		return false;
	}

	//Must be done before setting the working directory to prevent library load failure. - Solokiller
	if( !Steam_InitWrappers() )
		return false;

	//Shut down the older API so tools can safely use the newer one.
	SteamAPI_Shutdown();

	//Set the working directory to the game directory that the engine is running in.
	//Needed so asset loading works. Note that any mods that rely on ./valve to exist will break. - Solokiller
	//TODO: Consider adding symlinks or requiring the copying of assets to game directories. Windows won't allow it when targeting XP.
	{

		m_OldCWD = std::experimental::filesystem::current_path();

		const std::string szPath = std::string( "./" ) + m_szGameDir;

		std::error_code error;

		std::experimental::filesystem::current_path( std::experimental::filesystem::path( szPath ), error );

		if( error )
		{
			UTIL_ShowMessageBox( ( std::string( "Error while setting current working directory \"" ) + szPath + "\": " + error.message() ).c_str(), "Error", LogType::ERROR );
			return false;
		}
	}

	{
		int iArgC;
		char** ppszArgV;

		if( !InitCommandLine( iArgC, &ppszArgV ) )
			return false;

		const bool bResult = GetCommandLine()->Initialize( iArgC, ppszArgV, STRIP_COMMANDS );

		for( int iArg = 0; iArg < iArgC; ++iArg )
		{
			delete[] ppszArgV[ iArg ];
		}

		delete[] ppszArgV;

		if( !bResult )
			return false;
	}

	if( GetCommandLine()->GetValue( "-dumpcmdline" ) )
	{
		Msg( "%s\n", GetCommandLine()->GetCommandLineString() );
	}

	if( !LoadFileSystem() )
		return false;

	if( m_bIsListenServer )
	{
		//Already done by GoldSource. - Solokiller
		/*
		if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
		{
		return false;
		}
		*/

		//Find the engine's window and make it invisible.
		m_pEngineWindow = FindEngineWindow();

		if( m_pEngineWindow )
		{
			SDL_HideWindow( m_pEngineWindow );
		}

		//Run the SDL message pump to purge any events that are still pending.
		//This will also process the hide event sent above.
		SDL_Event event;

		while( SDL_PollEvent( &event ) )
		{
		}
	}

	{
		const char* pszToolLib = GetCommandLine()->GetValue( "-metatool" );

		const char* pszToolName = GetCommandLine()->GetValue( "-metatoolname" );

		//Default to loading the engine.
		if( !pszToolLib )
			pszToolLib = "Engine";

		//Default to loading the default tool.
		if( !pszToolName )
			pszToolName = DEFAULT_IMETATOOL_NAME;

		auto toolPath = fs::path( filepaths::TOOLS_DIR ) / pszToolLib;

		strncpy( m_szToolDir, toolPath.u8string().c_str(), sizeof( m_szToolDir ) );
		m_szToolDir[ sizeof( m_szToolDir ) - 1 ] = '\0';

		toolPath /= filepaths::BIN_DIR;

		if( !m_ToolLib.Load( CLibArgs( pszToolLib ).DisablePrefixes( true ).Path( toolPath.u8string().c_str() ) ) )
		{
			Msg( "Couldn't load tool \"%s\" library \"%s\" from \"%s\"\n", pszToolName, pszToolLib, toolPath.u8string().c_str() );
			return false;
		}

		auto factory = reinterpret_cast<CreateInterfaceFn>( m_ToolLib.GetFunctionAddress( CREATEINTERFACE_PROCNAME ) );

		if( !factory )
		{
			Msg( "Couldn't get tool \"%s\" library \"%s\" factory\n", pszToolName, pszToolLib );
			return false;
		}

		m_pTool = static_cast<IMetaTool*>( factory( pszToolName, nullptr ) );

		if( !m_pTool )
		{
			Msg( "Couldn't create tool \"%s\" from library \"%s\"\n", pszToolName, pszToolLib );
			return false;
		}

		//We know this is available since we called it earlier. - Solokiller
		auto filesystemFactory = reinterpret_cast<CreateInterfaceFn>( m_FileSystemLib.GetFunctionAddress( CREATEINTERFACE_PROCNAME ) );

		if( !m_pTool->Startup( *this, &filesystemFactory, 1 ) )
		{
			Msg( "Error while starting up tool \"%s\" from library \"%s\"\n", pszToolName, pszToolLib );
			return false;
		}
	}

	return m_pTool->Run();
}

void CMetaLoader::Shutdown()
{
	if( m_pTool )
	{
		m_pTool->Shutdown();
		m_pTool = nullptr;
	}

	m_ToolLib.Free();

	if( m_bIsListenServer )
	{
		SDL_Quit();
	}

	m_FileSystemLib.Free();
}

bool CMetaLoader::LoadFileSystem()
{
	if( !m_FileSystemLib.Load( CLibArgs( "FileSystem" ).DisablePrefixes( true ).Path( filepaths::BIN_DIR ) ) )
	{
		Msg( "Couldn't load FileSystem: %s\n", CLibrary::GetLoadErrorDescription() );
		return false;
	}

	auto filesystemFactory = reinterpret_cast<CreateInterfaceFn>( m_FileSystemLib.GetFunctionAddress( CREATEINTERFACE_PROCNAME ) );

	if( !filesystemFactory )
	{
		Msg( "Couldn't find filesystem factory\n" );
		return false;
	}

	m_pFileSystem = static_cast<IFileSystem2*>( filesystemFactory( FILESYSTEM2_INTERFACE_VERSION, nullptr ) );

	if( !m_pFileSystem )
	{
		Msg( "Couldn't instantiate the filesystem\n" );
		return false;
	}

	return true;
}

SDL_Window* CMetaLoader::FindEngineWindow()
{
	for( Uint32 uiID = 0; uiID < std::numeric_limits<Uint32>::max(); ++uiID )
	{
		SDL_Window* pWindow = SDL_GetWindowFromID( uiID );

		if( pWindow )
			return pWindow;
	}

	return nullptr;
}
