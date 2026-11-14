// Copyright Source Authors. All rights reserved.
//
// IGameUI implementation for the HTML GameUI.

#include "GameUI_Interface.h"

#include "winlite.h"

#include <cmath>
#include <vector>

#include "tier0/icommandline.h"
#include "tier0/dbg.h"
#include "tier0/platform.h"
#include "tier1/convar.h"
#include "tier1/fmtstr.h"
#include "tier1/ilocalize.h"
#include "tier2/tier2.h"
#include "tier3/tier3.h"
#include "filesystem.h"
#include "ienginevgui.h"
#include "IGameUIFuncs.h"
#include "engine/IEngineSound.h"
#include "game/client/IGameClientExports.h"
#include "vgui/ILocalize.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "vgui_controls/Controls.h"

#include "EngineInterface.h"
#include "GameConsole.h"
#include "HtmlUI.h"
#include "Json.h"
#include "ModInfo.h"
#include "UiApi.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IGameUIFuncs *gameuifuncs = nullptr;
IEngineVGui *enginevguifuncs = nullptr;
vgui::ISurface *enginesurfacefuncs = nullptr;
IVEngineClient *engine = nullptr;
IEngineSound *enginesound = nullptr;

static IGameClientExports *g_pGameClientExports = nullptr;
IGameClientExports *GameClientExports()
{
	return g_pGameClientExports;
}

static CGameUI g_GameUI;

CGameUI &GameUI()
{
	return g_GameUI;
}

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CGameUI, IGameUI, GAMEUI_INTERFACE_VERSION, g_GameUI );

//-----------------------------------------------------------------------------
// gamemenucommand: lets configs and the client DLL drive the menu.
//-----------------------------------------------------------------------------
CON_COMMAND( gamemenucommand, "Issue game menu command." )
{
	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: gamemenucommand <commandname>\n" );
		return;
	}
	GameUI().SendMainMenuCommand( args[1] );
}

CGameUI::CGameUI()
	: m_GameFactory( nullptr ),
	m_bPlayGameStartupSound( false ),
	m_bActivatedUI( false ),
	m_bHasSavedThisMenuSession( false ),
	m_bLoading( false ),
	m_bShowProgressText( true ),
	m_flLastProgress( -1.0f ),
	m_flNextStatePoll( 0.0 )
{
}

CGameUI::~CGameUI() = default;

void CGameUI::Initialize( CreateInterfaceFn factory )
{
	ConnectTier1Libraries( &factory, 1 );
	ConnectTier2Libraries( &factory, 1 );
	ConVar_Register( FCVAR_CLIENTDLL );
	ConnectTier3Libraries( &factory, 1 );

	enginesound = (IEngineSound *)factory( IENGINESOUND_CLIENT_INTERFACE_VERSION, nullptr );
	engine = (IVEngineClient *)factory( VENGINE_CLIENT_INTERFACE_VERSION, nullptr );
	enginevguifuncs = (IEngineVGui *)factory( VENGINE_VGUI_VERSION, nullptr );
	enginesurfacefuncs = (vgui::ISurface *)factory( VGUI_SURFACE_INTERFACE_VERSION, nullptr );
	gameuifuncs = (IGameUIFuncs *)factory( VENGINE_GAMEUIFUNCS_VERSION, nullptr );

	if ( !engine || !enginesurfacefuncs || !gameuifuncs || !enginevguifuncs )
	{
		Error( "CGameUI::Initialize() failed to get necessary interfaces\n" );
	}

	vgui::VGui_InitInterfacesList( "gameui", &factory, 1 );

	g_pVGuiLocalize->AddFile( "Resource/gameui_%language%.txt", "GAME", true );
	g_pVGuiLocalize->AddFile( "Resource/valve_%language%.txt", "GAME", true );

	ModInfo().LoadCurrentGameInfo();

	if ( !HtmlUI().Init( enginevguifuncs->GetPanel( PANEL_GAMEUIDLL ) ) )
	{
		Error( "CGameUI::Initialize() failed to start the HTML game UI\n" );
	}
}

void CGameUI::PostInit()
{
}

void CGameUI::Connect( CreateInterfaceFn gameFactory )
{
	m_GameFactory = gameFactory;
	g_pGameClientExports = (IGameClientExports *)gameFactory( GAMECLIENTEXPORTS_INTERFACE_VERSION, nullptr );
	if ( !g_pGameClientExports )
	{
		Error( "CGameUI::Connect() failed to get necessary interfaces\n" );
	}
}

void CGameUI::Start()
{
	g_pFullFileSystem->AddSearchPath( "platform", "PLATFORM" );

	g_pVGuiLocalize->AddFile( "Resource/platform_%language%.txt" );
	g_pVGuiLocalize->AddFile( "Resource/vgui_%language%.txt" );

	m_bPlayGameStartupSound = true;
}

void CGameUI::Shutdown()
{
	HtmlUI().Shutdown();

	// Let vgui delete the panel we marked for deletion.
	vgui::ivgui()->RunFrame();

	ModInfo().FreeModInfo();

	vgui::VGui_ShutdownInterfacesList( "gameui" );

	g_pGameClientExports = nullptr;
	gameuifuncs = nullptr;
	enginesurfacefuncs = nullptr;
	enginevguifuncs = nullptr;
	enginesound = nullptr;
	engine = nullptr;

	DisconnectTier3Libraries();
	ConVar_Unregister();
	DisconnectTier2Libraries();
	DisconnectTier1Libraries();
}

void CGameUI::PlayGameStartupSound()
{
	if ( CommandLine()->HasParm( "-nostartupsound" ) )
		return;

	std::vector<std::string> files;
	FileFindHandle_t fh = FILESYSTEM_INVALID_FIND_HANDLE;
	for ( const char *fn = g_pFullFileSystem->FindFirstEx( "sound/ui/gamestartup*.mp3", "MOD", &fh ); fn; fn = g_pFullFileSystem->FindNext( fh ) )
	{
		files.push_back( std::string( "ui/" ) + fn );
	}
	g_pFullFileSystem->FindClose( fh );

	if ( files.empty() )
		return;

	const size_t index = static_cast<size_t>( Plat_MSTime() ) % files.size();
	// "*#" makes it stream and obey snd_musicvolume.
	CFmtStrN<512> cmd( "play *#%s\n", files[index].c_str() );
	engine->ClientCmd_Unrestricted( cmd );
}

void CGameUI::RunFrame()
{
	HtmlUI().RunFrame();

	if ( m_bHideAfterLoad )
	{
		const bool inGame = engine->IsInGame();
		if ( ( inGame && ++m_nHideAfterLoadFrames >= 2 ) || !engine->IsConnected() || Plat_FloatTime() > m_flHideAfterLoadDeadline )
		{
			m_bHideAfterLoad = false;
			HtmlUI().SetHoldingFrame( false );
			engine->ExecuteClientCmd( "gameui_hide" );
		}
	}

	if ( m_bPlayGameStartupSound )
	{
		m_bPlayGameStartupSound = false;
		PlayGameStartupSound();
	}

	// Cheap state polling so the page can react to pause / level changes.
	const double now = Plat_FloatTime();
	if ( now >= m_flNextStatePoll )
	{
		m_flNextStatePoll = now + 0.25;
		PushState();
	}
}

void CGameUI::PushState()
{
	std::string state = UiApi_StateJson();
	if ( state != m_LastStateJson )
	{
		m_LastStateJson = state;
		HtmlUI().Emit( "state", state.c_str() );
	}
}

bool CGameUI::IsInLevel()
{
	const char *levelName = engine->GetLevelName();
	return levelName && levelName[0] && !engine->IsLevelMainMenuBackground();
}

bool CGameUI::IsInBackgroundLevel()
{
	const char *levelName = engine->GetLevelName();
	return levelName && levelName[0] && engine->IsLevelMainMenuBackground();
}

bool CGameUI::IsInMultiplayer()
{
	return IsInLevel() && engine->GetMaxClients() > 1;
}

//-----------------------------------------------------------------------------
// Activation
//-----------------------------------------------------------------------------
void CGameUI::OnGameUIActivated()
{
	m_bActivatedUI = true;

	// Pause the server in single player.
	if ( engine->GetMaxClients() <= 1 )
	{
		engine->ClientCmd_Unrestricted( "setpause\n" );
	}

	SetSavedThisMenuSession( false );
	HtmlUI().SetVisible( true );

	m_LastStateJson = UiApi_StateJson();
	HtmlUI().Emit( "activated", m_LastStateJson.c_str() );
}

void CGameUI::OnGameUIHidden()
{
	m_bActivatedUI = false;

	if ( engine->GetMaxClients() <= 1 )
	{
		engine->ClientCmd_Unrestricted( "unpause\n" );
	}

	HtmlUI().SetPreventEscape( false );
	HtmlUI().SetVisible( false );
	HtmlUI().Emit( "hidden" );
}

bool CGameUI::IsMainMenuVisible()
{
	return m_bActivatedUI;
}

//-----------------------------------------------------------------------------
// Loading
//-----------------------------------------------------------------------------
void CGameUI::OnLevelLoadingStarted( bool bShowProgressDialog )
{
	m_bLoading = true;
	m_bPlayGameStartupSound = false;
	HtmlUI().SetLoading( true );
	HtmlUI().ForceAllowEscape();
	m_flLastProgress = -1.0f;
	m_LastStatusText.clear();

	json::Writer w;
	w.BeginObject().Prop( "dialog", bShowProgressDialog ).EndObject();
	HtmlUI().Emit( "loading.start", w.str().c_str() );
	HtmlUI().Pump();
}

void CGameUI::OnLevelLoadingFinished( bool bError, const char *failureReason, const char *extendedReason )
{
	m_bLoading = false;
	HtmlUI().SetLoading( false );

	json::Writer w;
	w.BeginObject();
	w.Prop( "error", bError );
	w.Key( "reason" );
	if ( failureReason && failureReason[0] == '#' && g_pVGuiLocalize && g_pVGuiLocalize->Find( failureReason ) )
		w.WString( g_pVGuiLocalize->Find( failureReason ) );
	else
		w.String( failureReason ? failureReason : "" );
	w.Prop( "extended", extendedReason ? extendedReason : "" );
	w.EndObject();
	HtmlUI().Emit( "loading.end", w.str().c_str() );
	HtmlUI().Pump();

	HtmlUI().ForceAllowEscape();
	if ( bError )
	{
		engine->ExecuteClientCmd( "gameui_hide" );
		return;
	}
	// The client still has to finish connecting before it draws the world;
	// hiding now would show black frames meanwhile (RunFrame hides it).
	m_bHideAfterLoad = true;
	m_nHideAfterLoadFrames = 0;
	m_flHideAfterLoadDeadline = Plat_FloatTime() + 5.0;
	HtmlUI().SetHoldingFrame( true );
}

bool CGameUI::UpdateProgressBar( float progress, const char *statusText )
{
	if ( !m_bLoading )
		return false;

	// Keep servicing the page while the main thread is busy loading.
	HtmlUI().Pump();

	bool changed = false;
	if ( std::fabs( progress - m_flLastProgress ) >= 0.01f )
	{
		m_flLastProgress = progress;
		changed = true;
	}
	const std::string text = ( m_bShowProgressText && statusText ) ? statusText : "";
	if ( text != m_LastStatusText )
	{
		m_LastStatusText = text;
		changed = true;
	}

	if ( changed )
	{
		json::Writer w;
		w.BeginObject();
		w.Prop( "progress", static_cast<double>( progress ) );
		w.Key( "text" );
		if ( !text.empty() && text[0] == '#' && g_pVGuiLocalize && g_pVGuiLocalize->Find( text.c_str() ) )
			w.WString( g_pVGuiLocalize->Find( text.c_str() ) );
		else
			w.String( text );
		w.EndObject();
		HtmlUI().Emit( "loading.progress", w.str().c_str() );
	}

	// Redraw whenever Chromium produced a new frame (the page animates
	// asynchronously on its own threads).
	return changed || HtmlUI().HasNewFrame();
}

bool CGameUI::SetShowProgressText( bool show )
{
	const bool previous = m_bShowProgressText;
	m_bShowProgressText = show;
	return previous;
}

void CGameUI::SetProgressOnStart()
{
	HtmlUI().Emit( "loading.pending" );
}

void CGameUI::SetLoadingBackgroundDialog( vgui::VPANEL )
{
	// Loading screens are drawn by the page.
}

//-----------------------------------------------------------------------------
// Connection
//-----------------------------------------------------------------------------
void CGameUI::OLD_OnConnectToServer( const char *game, int IP, int port )
{
	OnConnectToServer2( game, IP, port, port );
}

void CGameUI::OnConnectToServer2( const char *, int, int, int )
{
	PushState();
}

void CGameUI::OnDisconnectFromServer_OLD( uint8 eSteamLoginFailure, const char * )
{
	OnDisconnectFromServer( eSteamLoginFailure );
}

void CGameUI::OnDisconnectFromServer( uint8 eSteamLoginFailure )
{
	const char *reason = "";
	switch ( eSteamLoginFailure )
	{
		case STEAMLOGINFAILURE_BADTICKET: reason = "badticket"; break;
		case STEAMLOGINFAILURE_NOSTEAMLOGIN: reason = "nosteamlogin"; break;
		case STEAMLOGINFAILURE_VACBANNED: reason = "vacbanned"; break;
		case STEAMLOGINFAILURE_LOGGED_IN_ELSEWHERE: reason = "loggedinelsewhere"; break;
		default: break;
	}
	json::Writer w;
	w.BeginObject().Prop( "reason", reason ).EndObject();
	HtmlUI().Emit( "disconnected", w.str().c_str() );
	PushState();
}

//-----------------------------------------------------------------------------
// Menu commands
//-----------------------------------------------------------------------------
void CGameUI::ShowNewGameDialog( int chapter )
{
	json::Writer w;
	w.BeginObject().Prop( "chapter", chapter ).EndObject();
	HtmlUI().Emit( "newgame.show", w.str().c_str() );
}

void CGameUI::OnConfirmQuit()
{
	HtmlUI().Emit( "quit.confirm" );
}

void CGameUI::SetMainMenuOverride( vgui::VPANEL )
{
	// The HTML main menu cannot be replaced by a vgui panel.
}

void CGameUI::SendMainMenuCommand( const char *pszCommand )
{
	if ( !pszCommand || !*pszCommand )
		return;

	// Commands that need no UI are executed directly; the rest is routed to
	// the page (e.g. OpenOptionsDialog).
	if ( V_stricmp( pszCommand, "ResumeGame" ) == 0 )
	{
		engine->ClientCmd_Unrestricted( "gameui_hide\n" );
		return;
	}
	if ( V_stricmp( pszCommand, "QuitNoConfirm" ) == 0 )
	{
		engine->ClientCmd_Unrestricted( "quit\n" );
		return;
	}
	if ( V_stricmp( pszCommand, "DisconnectNoConfirm" ) == 0 )
	{
		engine->ClientCmd_Unrestricted( "disconnect\n" );
		return;
	}

	json::Writer w;
	w.BeginObject().Prop( "command", pszCommand ).EndObject();
	HtmlUI().Emit( "menu.command", w.str().c_str() );
}

void CGameUI::OnCreditsFinished()
{
	HtmlUI().Emit( "credits.finished" );
}

//-----------------------------------------------------------------------------
// Console-only (X360) notifications: not used on PC.
//-----------------------------------------------------------------------------
void CGameUI::SessionNotification( const int, const int ) {}
void CGameUI::SystemNotification( const int ) {}
void CGameUI::ShowMessageDialog( const uint, vgui::Panel * ) {}
void CGameUI::UpdatePlayerInfo( uint64, const char *, int, byte, int, bool ) {}
void CGameUI::SessionSearchResult( int, void *, XSESSION_SEARCHRESULT *, int ) {}

bool CGameUI::ValidateStorageDevice( int *pStorageDeviceValidated )
{
	if ( pStorageDeviceValidated )
		*pStorageDeviceValidated = 1;
	return true;
}

//-----------------------------------------------------------------------------
// Bonus maps: not part of Half-Life 2. Report "nothing" so callers behave.
//-----------------------------------------------------------------------------
void CGameUI::BonusMapUnlock( const char *, const char * ) {}
void CGameUI::BonusMapComplete( const char *, const char * ) {}
void CGameUI::BonusMapChallengeUpdate( const char *, const char *, const char *, int ) {}

void CGameUI::BonusMapChallengeNames( char *pchFileName, char *pchMapName, char *pchChallengeName )
{
	if ( pchFileName ) pchFileName[0] = '\0';
	if ( pchMapName ) pchMapName[0] = '\0';
	if ( pchChallengeName ) pchChallengeName[0] = '\0';
}

void CGameUI::BonusMapChallengeNames( char *pchFileName, intp fileSize, char *pchMapName, intp mapSize, char *pchChallengeName, intp challengeSize )
{
	if ( pchFileName && fileSize > 0 ) pchFileName[0] = '\0';
	if ( pchMapName && mapSize > 0 ) pchMapName[0] = '\0';
	if ( pchChallengeName && challengeSize > 0 ) pchChallengeName[0] = '\0';
}

void CGameUI::BonusMapChallengeObjectives( int &iBronze, int &iSilver, int &iGold )
{
	iBronze = iSilver = iGold = 0;
}

void CGameUI::BonusMapDatabaseSave() {}

int CGameUI::BonusMapNumAdvancedCompleted()
{
	return 0;
}

void CGameUI::BonusMapNumMedals( int piNumMedals[ 3 ] )
{
	piNumMedals[0] = piNumMedals[1] = piNumMedals[2] = 0;
}
