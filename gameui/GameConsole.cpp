// Copyright Source Authors. All rights reserved.
//
// IGameConsole implementation backed by the HTML GameUI.

#include "GameConsole.h"

#include "tier1/convar.h"
#include "tier1/fmtstr.h"
#include "filesystem.h"
#include "Color.h"

#include "HtmlUI.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static CGameConsole g_GameConsole;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CGameConsole, IGameConsole, GAMECONSOLE_INTERFACE_VERSION, g_GameConsole );

CGameConsole &GameConsole()
{
	return g_GameConsole;
}

namespace
{
constexpr size_t kMaxHistory = 512 * 1024;
}

CGameConsole::CGameConsole()
	: m_bInitialized( false ),
	m_bVisible( false )
{
}

void CGameConsole::Initialize()
{
	if ( m_bInitialized )
		return;
	m_bInitialized = true;
	g_pCVar->InstallConsoleDisplayFunc( this );
}

void CGameConsole::Initialize( vgui::VPANEL, const char * )
{
	Initialize();
}

void CGameConsole::Shutdown()
{
	if ( !m_bInitialized )
		return;
	m_bInitialized = false;
	g_pCVar->RemoveConsoleDisplayFunc( this );
}

void CGameConsole::SetParent( vgui::VPANEL )
{
	// The console lives inside the HTML page.
}

void CGameConsole::Activate()
{
	if ( !m_bInitialized )
		return;
	m_bVisible = true;
	HtmlUI().Emit( "console.show" );
}

void CGameConsole::Hide()
{
	if ( !m_bInitialized )
		return;
	m_bVisible = false;
	HtmlUI().Emit( "console.hide" );
}

void CGameConsole::Clear()
{
	{
		std::lock_guard lock( m_HistoryMutex );
		m_History.clear();
	}
	HtmlUI().Emit( "console.clear" );
}

bool CGameConsole::IsConsoleVisible()
{
	return m_bInitialized && m_bVisible;
}

void CGameConsole::Append( const char *text, int r, int g, int b, int a, bool developer )
{
	if ( !text || !*text )
		return;

	{
		std::lock_guard lock( m_HistoryMutex );
		m_History += text;
		if ( m_History.size() > kMaxHistory )
			m_History.erase( 0, m_History.size() - kMaxHistory );
	}

	HtmlUI().QueueConsoleText( text, r, g, b, a, developer );
}

void CGameConsole::ColorPrint( const Color &clr, const char *pMessage )
{
	Append( pMessage, clr.r(), clr.g(), clr.b(), clr.a(), false );
}

void CGameConsole::Print( const char *pMessage )
{
	Append( pMessage, 216, 222, 211, 255, false );
}

void CGameConsole::DPrint( const char *pMessage )
{
	Append( pMessage, 196, 181, 80, 255, true );
}

void CGameConsole::Dump()
{
	std::string text;
	{
		std::lock_guard lock( m_HistoryMutex );
		text = m_History;
	}

	for ( int i = 0; i < 1000; ++i )
	{
		CFmtStrN<64> name( "condump%03d.txt", i );
		if ( g_pFullFileSystem->FileExists( name, "MOD" ) )
			continue;

		FileHandle_t f = g_pFullFileSystem->Open( name, "wb", "MOD" );
		if ( !f )
		{
			Warning( "condump: unable to open %s for writing.\n", name.Get() );
			return;
		}
		g_pFullFileSystem->Write( text.data(), static_cast<int>( text.size() ), f );
		g_pFullFileSystem->Close( f );
		Msg( "console dumped to %s.\n", name.Get() );
		return;
	}
	Warning( "condump: too many existing condump files.\n" );
}

CON_COMMAND( condump, "dump the text currently in the console to condumpXX.txt" )
{
	GameConsole().Dump();
}
