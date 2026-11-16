// Copyright Source Authors. All rights reserved.
//
// Engine API for the HTML GameUI. See UiApi.h and htmlui/js/source.js for the
// JavaScript side.

#include "UiApi.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "tier0/icommandline.h"
#include "tier1/convar.h"
#include "tier1/fmtstr.h"
#include "tier1/ilocalize.h"
#include "tier1/KeyValues.h"
#include "tier1/utlbuffer.h"
#include "tier2/tier2.h"
#include "tier3/tier3.h"
#include "filesystem.h"
#include "inputsystem/iinputsystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/materialsystem_config.h"
#include "modes.h"
#include "savegame_version.h"
#include "vgui/ILocalize.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "IGameUIFuncs.h"
#include "GameUI/IGameConsole.h"

#include "EngineInterface.h"
#include "winlite.h"
#include <shellapi.h>
#include "GameUI_Interface.h"
#include "GameConsole.h"
#include "HtmlUI.h"
#include "ModInfo.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{

using Handler = bool ( * )( const json::Value &args, json::Writer &out, std::string &error );

constexpr char kSaveDir[]
{
#ifdef PLATFORM_64BITS
	"save/x64"
#else
	"save"
#endif
};

//-----------------------------------------------------------------------------
// Helpers
//-----------------------------------------------------------------------------

// Removes characters that could break out of a console command argument.
std::string SanitizeArg( const std::string &in )
{
	std::string out;
	out.reserve( in.size() );
	for ( char c : in )
	{
		if ( c == '"' || c == ';' || c == '\n' || c == '\r' )
			continue;
		out += c;
	}
	return out;
}

bool IsSafeFileName( const std::string &name )
{
	if ( name.empty() || name.size() > 120 )
		return false;
	for ( char c : name )
	{
		if ( !( V_isalnum( c ) || c == '-' || c == '_' || c == '.' ) )
			return false;
	}
	return name.find( ".." ) == std::string::npos;
}

const char *ModDirectory()
{
	static char dir[MAX_PATH];
	const char *full = engine->GetGameDirectory();
	const char *sep = full ? max( strrchr( full, '/' ), strrchr( full, '\\' ) ) : nullptr;
	V_strcpy_safe( dir, sep ? sep + 1 : ( full ? full : "hl2" ) );
	return dir;
}

void WriteLocalized( json::Writer &w, const char *token )
{
	if ( token && token[0] == '#' && g_pVGuiLocalize )
	{
		if ( const wchar_t *text = g_pVGuiLocalize->Find( token ) )
		{
			w.WString( text );
			return;
		}
	}
	w.String( token ? token : "" );
}

// Tokenizer compatible with the engine's config/script files: quoted strings,
// bare words, // comments.
const char *ParseToken( const char *data, std::string &token )
{
	token.clear();
	if ( !data )
		return nullptr;

	for ( ;; )
	{
		while ( *data && static_cast<unsigned char>( *data ) <= ' ' )
			++data;
		if ( !*data )
			return nullptr;
		if ( data[0] == '/' && data[1] == '/' )
		{
			while ( *data && *data != '\n' )
				++data;
			continue;
		}
		break;
	}

	if ( *data == '"' )
	{
		++data;
		while ( *data && *data != '"' )
			token += *data++;
		if ( *data == '"' )
			++data;
		return data;
	}

	while ( *data && static_cast<unsigned char>( *data ) > ' ' )
		token += *data++;
	return data;
}

//-----------------------------------------------------------------------------
// ui.*
//-----------------------------------------------------------------------------
bool Api_UiReady( const json::Value &, json::Writer &out, std::string & )
{
	HtmlUI().OnPageReady();
	out.Raw( UiApi_StateJson().c_str() );
	return true;
}

bool Api_HudReady( const json::Value &, json::Writer &out, std::string & )
{
	HtmlUI().OnHudReady();
	out.Raw( UiApi_StateJson().c_str() );
	return true;
}

bool SetGlass( CHtmlPanel *panel, const json::Value &args, json::Writer &out )
{
	std::vector<CHtmlPanel::GlassRect> rects;
	for ( const auto &r : args["rects"].Items() )
	{
		// [x, y, w, h, radius?, alpha?] in page pixels.
		CHtmlPanel::GlassRect g{ r[static_cast<size_t>( 0 )].AsInt(), r[static_cast<size_t>( 1 )].AsInt(),
			r[static_cast<size_t>( 2 )].AsInt(), r[static_cast<size_t>( 3 )].AsInt(),
			r.Size() > 4 ? static_cast<float>( r[static_cast<size_t>( 4 )].AsNumber() ) : 0.0f,
			r.Size() > 5 ? static_cast<float>( r[static_cast<size_t>( 5 )].AsNumber( 1 ) ) : 1.0f };
		g.radius = std::clamp( g.radius, 0.0f, 0.5f * static_cast<float>( (std::min)( g.w, g.h ) ) );
		g.alpha = std::clamp( g.alpha, 0.0f, 1.0f );
		if ( g.w > 0 && g.h > 0 && g.alpha > 0.0f && rects.size() < 64 )
			rects.push_back( g );
	}
	if ( panel )
		panel->SetGlassRects( std::move( rects ) );
	out.Bool( panel != nullptr );
	return true;
}

bool Api_UiGlass( const json::Value &args, json::Writer &out, std::string & )
{
	return SetGlass( HtmlUI().Menu().Panel(), args, out );
}

bool Api_HudGlass( const json::Value &args, json::Writer &out, std::string & )
{
	return SetGlass( HtmlUI().Hud().Panel(), args, out );
}

// hud.flash { r, g, b, alpha, seconds }: screen-edge damage flash.
bool Api_HudFlash( const json::Value &args, json::Writer &out, std::string & )
{
	CHtmlPanel *panel = HtmlUI().Hud().Panel();
	if ( panel )
		panel->Flash( args["r"].AsInt( 200 ), args["g"].AsInt( 0 ), args["b"].AsInt( 0 ),
			static_cast<float>( args["alpha"].AsNumber( 0.5 ) ), static_cast<float>( args["seconds"].AsNumber( 0.8 ) ) );
	out.Bool( panel != nullptr );
	return true;
}

bool Api_UiStats( const json::Value &, json::Writer &out, std::string & )
{
	const CHtmlUI::Stats &s = HtmlUI().GetStats();
	out.BeginObject();
	out.Prop( "engineFps", static_cast<double>( s.engineFps ) );
	out.Prop( "beginFrames", static_cast<double>( s.beginFramesPerSec ) );
	out.Prop( "menuPaints", static_cast<double>( s.menuPaintsPerSec ) );
	out.Prop( "hudPaints", static_cast<double>( s.hudPaintsPerSec ) );
	out.EndObject();
	return true;
}

bool Api_UiEscape( const json::Value &args, json::Writer &out, std::string & )
{
	HtmlUI().SetPreventEscape( args["prevent"].AsBool() );
	out.Bool( HtmlUI().IsEscapePrevented() );
	return true;
}

bool Api_UiReload( const json::Value &, json::Writer &out, std::string & )
{
	HtmlUI().Reload();
	out.Bool( true );
	return true;
}

bool Api_UiDevTools( const json::Value &, json::Writer &out, std::string & )
{
	HtmlUI().ShowDevTools();
	out.Bool( true );
	return true;
}

bool Api_State( const json::Value &, json::Writer &out, std::string & )
{
	out.Raw( UiApi_StateJson().c_str() );
	return true;
}

//-----------------------------------------------------------------------------
// Commands / cvars
//-----------------------------------------------------------------------------
bool Api_Cmd( const json::Value &args, json::Writer &out, std::string &error )
{
	std::string command = args["command"].AsString();
	if ( command.empty() )
	{
		error = "missing 'command'";
		return false;
	}
	if ( command.back() != '\n' )
		command += '\n';
	engine->ClientCmd_Unrestricted( command.c_str() );
	out.Bool( true );
	return true;
}

bool Api_Quit( const json::Value &, json::Writer &out, std::string & )
{
	engine->ClientCmd_Unrestricted( "quit\n" );
	out.Bool( true );
	return true;
}

void WriteConVar( json::Writer &w, ConVar *var )
{
	w.BeginObject();
	w.Prop( "value", var->GetString() );
	w.Prop( "default", var->GetDefault() );
	w.Prop( "help", var->GetHelpText() ? var->GetHelpText() : "" );
	float v;
	if ( var->GetMin( v ) ) w.Prop( "min", static_cast<double>( v ) );
	if ( var->GetMax( v ) ) w.Prop( "max", static_cast<double>( v ) );
	w.Prop( "archive", var->IsFlagSet( FCVAR_ARCHIVE ) );
	w.Prop( "cheat", var->IsFlagSet( FCVAR_CHEAT ) );
	w.EndObject();
}

bool Api_CvarGet( const json::Value &args, json::Writer &out, std::string &error )
{
	const json::Value &names = args["names"];
	if ( !names.IsArray() )
	{
		error = "'names' must be an array";
		return false;
	}
	out.BeginObject();
	for ( const auto &n : names.Items() )
	{
		const std::string name = n.AsString();
		out.Key( name.c_str() );
		ConVar *var = g_pCVar->FindVar( name.c_str() );
		if ( var ) WriteConVar( out, var );
		else out.Null();
	}
	out.EndObject();
	return true;
}

bool Api_CvarSet( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string name = args["name"].AsString();
	const std::string value = args["value"].AsString();
	ConVar *var = name.empty() ? nullptr : g_pCVar->FindVar( name.c_str() );
	if ( !var )
	{
		error = "unknown cvar '" + name + "'";
		return false;
	}

	if ( var->IsFlagSet( FCVAR_CHEAT | FCVAR_REPLICATED | FCVAR_SPONLY | FCVAR_NOT_CONNECTED | FCVAR_DEVELOPMENTONLY ) )
	{
		// Let the engine enforce cheat / replication rules.
		CFmtStrN<512> cmd( "%s \"%s\"\n", var->GetName(), SanitizeArg( value ).c_str() );
		engine->ClientCmd_Unrestricted( cmd );
	}
	else
	{
		var->SetValue( value.c_str() );
	}

	WriteConVar( out, var );
	return true;
}

bool Api_ConfigSave( const json::Value &, json::Writer &out, std::string & )
{
	engine->ClientCmd_Unrestricted( "host_writeconfig\n" );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Localization / game info
//-----------------------------------------------------------------------------
bool Api_Localize( const json::Value &args, json::Writer &out, std::string & )
{
	out.BeginObject();
	for ( const auto &t : args["tokens"].Items() )
	{
		const std::string token = t.AsString();
		out.Key( token.c_str() );
		WriteLocalized( out, token.c_str() );
	}
	out.EndObject();
	return true;
}

bool Api_LocalizePrefix( const json::Value &args, json::Writer &out, std::string & )
{
	std::vector<std::string> prefixes;
	for ( const auto &p : args["prefixes"].Items() )
	{
		std::string s = p.AsString();
		if ( !s.empty() && s[0] == '#' ) s.erase( 0, 1 );
		if ( !s.empty() ) prefixes.push_back( s );
	}

	out.BeginObject();
	if ( g_pVGuiLocalize )
	{
		for ( auto i = g_pVGuiLocalize->GetFirstStringIndex(); i != INVALID_LOCALIZE_STRING_INDEX; i = g_pVGuiLocalize->GetNextStringIndex( i ) )
		{
			const char *name = g_pVGuiLocalize->GetNameByIndex( i );
			if ( !name )
				continue;
			const bool match = prefixes.empty() || std::any_of( prefixes.begin(), prefixes.end(),
				[name]( const std::string &p ) { return V_strnicmp( name, p.c_str(), static_cast<int>( p.size() ) ) == 0; } );
			if ( !match )
				continue;
			char key[256];
			V_sprintf_safe( key, "#%s", name );
			out.Key( key ).WString( g_pVGuiLocalize->GetValueByIndex( i ) );
		}
	}
	out.EndObject();
	return true;
}

bool CutscenelessDir( char *pOut, int nOut );

bool Api_GameInfo( const json::Value &, json::Writer &out, std::string & )
{
	out.BeginObject();
	out.Key( "title" ).WString( ModInfo().GetGameTitle() );
	out.Key( "title2" ).WString( ModInfo().GetGameTitle2() );
	out.Prop( "name", ModInfo().GetGameName() );
	out.Prop( "dir", ModDirectory() );
	out.Prop( "singleplayerOnly", ModInfo().IsSinglePlayerOnly() );
	out.Prop( "multiplayerOnly", ModInfo().IsMultiplayerOnly() );
	out.Prop( "noDifficulty", ModInfo().NoDifficulty() );
	out.Prop( "noModels", ModInfo().NoModels() );
	out.Prop( "noHiModel", ModInfo().NoHiModel() );
	out.Prop( "noCrosshair", ModInfo().NoCrosshair() );
	out.Prop( "advCrosshair", ModInfo().AdvCrosshair() );
	out.Prop( "hasPortals", ModInfo().HasPortals() );
	out.Prop( "hasHDContent", ModInfo().HasHDContent() );
	out.Prop( "useGameLogo", ModInfo().UseGameLogo() );
	out.Prop( "appId", engine->GetAppID() );
	char lang[64] = {};
	engine->GetUILanguage( lang, sizeof( lang ) );
	out.Prop( "language", lang[0] ? lang : "english" );
	out.Prop( "x64", static_cast<bool>( sizeof( void * ) == 8 ) );
	char cutsceneless[MAX_PATH];
	out.Prop( "cutsceneless", CutscenelessDir( cutsceneless, sizeof( cutsceneless ) ) );
	out.EndObject();
	return true;
}

bool Api_SoundPlay( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string name = args["name"].AsString();
	if ( name.empty() || name.find( ".." ) != std::string::npos )
	{
		error = "invalid sound";
		return false;
	}
	vgui::surface()->PlaySound( name.c_str() );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// New game
//-----------------------------------------------------------------------------
struct Chapter
{
	std::string file;  // chapter1.cfg
	std::string id;    // 1, 9a, ...
};

std::vector<Chapter> ScanChapters()
{
	std::vector<Chapter> chapters;

	FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
	for ( const char *name = g_pFullFileSystem->FindFirst( "cfg/chapter*.cfg", &handle ); name; name = g_pFullFileSystem->FindNext( handle ) )
	{
		char full[MAX_PATH];
		V_sprintf_safe( full, "cfg/%s", name );
		// Only chapters from the current mod; skip empty demo stubs.
		if ( !g_pFullFileSystem->FileExists( full, "MOD" ) || g_pFullFileSystem->Size( full, "MOD" ) == 0 )
			continue;

		Chapter c;
		c.file = name;
		c.id = name + 7;  // after "chapter"
		const size_t dot = c.id.find( '.' );
		if ( dot != std::string::npos ) c.id.erase( dot );
		if ( c.id.empty() || c.id.size() > 8 )
			continue;
		chapters.push_back( c );
	}
	g_pFullFileSystem->FindClose( handle );

	std::sort( chapters.begin(), chapters.end(), []( const Chapter &a, const Chapter &b ) {
		const int na = atoi( a.id.c_str() ), nb = atoi( b.id.c_str() );
		if ( na != nb ) return na < nb;
		if ( a.id.size() != b.id.size() ) return a.id.size() < b.id.size();
		return a.id < b.id;
	} );
	return chapters;
}

bool Api_Chapters( const json::Value &, json::Writer &out, std::string & )
{
	const std::vector<Chapter> chapters = ScanChapters();
	ConVarRef unlocked( "sv_unlockedchapters" );
	const int unlockedCount = unlocked.IsValid() ? max( unlocked.GetInt(), 1 ) : static_cast<int>( chapters.size() );
	const bool unlockAll = CommandLine()->HasParm( "-dev" ) != 0;

	out.BeginArray();
	for ( size_t i = 0; i < chapters.size(); ++i )
	{
		const Chapter &c = chapters[i];
		char token[128];
		V_sprintf_safe( token, "#%s_Chapter%s_Title", ModDirectory(), c.id.c_str() );

		char image[MAX_PATH];
		V_sprintf_safe( image, "/img/vtf/vgui/chapters/chapter%s", c.id.c_str() );

		out.BeginObject();
		out.Prop( "index", static_cast<int>( i ) );
		out.Prop( "id", c.id );
		out.Prop( "config", c.file );
		out.Key( "title" );
		WriteLocalized( out, token );
		out.Prop( "image", image );
		out.Prop( "locked", !unlockAll && static_cast<int>( i ) + 1 > unlockedCount );
		out.EndObject();
	}
	out.EndArray();
	return true;
}

bool Api_NewGame( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string config = args["config"].AsString();
	if ( !IsSafeFileName( config ) || V_strnicmp( config.c_str(), "chapter", 7 ) != 0 )
	{
		error = "invalid chapter config";
		return false;
	}

	if ( args.Has( "skill" ) )
	{
		const int skill = clamp( args["skill"].AsInt( 2 ), 1, 3 );
		ConVarRef skillVar( "skill" );
		if ( skillVar.IsValid() ) skillVar.SetValue( skill );
	}

	const bool commentary = args["commentary"].AsBool();
	ConVarRef commentaryVar( "commentary" );
	if ( commentaryVar.IsValid() ) commentaryVar.SetValue( commentary );
	ConVarRef cheats( "sv_cheats" );
	if ( cheats.IsValid() ) cheats.SetValue( commentary );

	CFmtStrN<512> cmd( "disconnect\ndeathmatch 0\nprogress_enable\nexec %s\n", config.c_str() );
	engine->ClientCmd_Unrestricted( cmd );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Saves
//-----------------------------------------------------------------------------
int ReadSaveHeader( FileHandle_t f, std::string &mapName, std::string &comment )
{
	int tag = 0, size = 0, tokenSize = 0, tokenCount = 0;
	if ( g_pFullFileSystem->Read( &tag, sizeof( tag ), f ) != sizeof( tag ) || tag != MAKEID( 'J', 'S', 'A', 'V' ) )
		return 0;
	if ( g_pFullFileSystem->Read( &tag, sizeof( tag ), f ) != sizeof( tag ) || tag != SAVEGAME_VERSION )
		return 0;
	g_pFullFileSystem->Read( &size, sizeof( size ), f );
	g_pFullFileSystem->Read( &tokenCount, sizeof( tokenCount ), f );
	g_pFullFileSystem->Read( &tokenSize, sizeof( tokenSize ), f );

	if ( size < 0 || tokenCount <= 0 || tokenCount > 1024 * 1024 || tokenSize <= 0 || tokenSize > 32 * 1024 * 1024 || size > 32 * 1024 * 1024 )
		return 0;

	const int total = size + tokenSize;
	std::vector<char> data( static_cast<size_t>( total ) + 1, '\0' );
	if ( g_pFullFileSystem->Read( data.data(), total, f ) != total )
		return 0;

	const char *p = data.data();
	const char *end = data.data() + total;

	std::vector<const char *> tokens( static_cast<size_t>( tokenCount ), nullptr );
	for ( int i = 0; i < tokenCount && p < end; ++i )
	{
		tokens[i] = *p ? p : nullptr;
		while ( p < end && *p ) ++p;
		++p;
	}

	auto readShort = [&]( short &v ) {
		if ( end - p < 2 ) return false;
		memcpy( &v, p, 2 );
		p += 2;
		return true;
	};
	auto tokenAt = [&]( short idx ) -> const char * {
		return ( idx >= 0 && idx < tokenCount ) ? tokens[idx] : nullptr;
	};

	short fieldSize = 0, fieldIndex = 0;
	if ( !readShort( fieldSize ) || !readShort( fieldIndex ) )
		return 0;
	const char *headerName = tokenAt( fieldIndex );
	if ( !headerName || V_stricmp( headerName, "GameHeader" ) != 0 || end - p < 4 )
		return 0;

	int fieldCount = 0;
	memcpy( &fieldCount, p, 4 );
	p += fieldSize;

	for ( int i = 0; i < fieldCount && p < end; ++i )
	{
		if ( !readShort( fieldSize ) || !readShort( fieldIndex ) || fieldSize < 0 || end - p < fieldSize )
			break;
		const char *name = tokenAt( fieldIndex );
		if ( name )
		{
			if ( V_stricmp( name, "comment" ) == 0 )
				comment.assign( p, strnlen( p, fieldSize ) );
			else if ( V_stricmp( name, "mapName" ) == 0 )
				mapName.assign( p, strnlen( p, fieldSize ) );
		}
		p += fieldSize;
	}

	return !mapName.empty() && !comment.empty();
}

//-----------------------------------------------------------------------------
// Splits (LiveSplit .lss files in <mod>/splits). The HUD timer reads and
// updates them; the files stay LiveSplit compatible.
//-----------------------------------------------------------------------------
constexpr char kSplitsDir[] = "splits";

// LiveSplit names are free text ("Half-Life 2 - Any%.lss"): allow anything but
// path separators, reserved characters and "..".
bool IsSafeSplitsName( const std::string &name )
{
	if ( name.size() < 5 || name.size() > 160 || name.find( ".." ) != std::string::npos )
		return false;
	for ( unsigned char c : name )
	{
		if ( c < 32 || strchr( "/\\:*?\"<>|", c ) )
			return false;
	}
	return V_stricmp( name.c_str() + name.size() - 4, ".lss" ) == 0;
}

bool Api_SplitsList( const json::Value &, json::Writer &out, std::string & )
{
	g_pFullFileSystem->CreateDirHierarchy( kSplitsDir, "MOD" );
	std::vector<std::string> files;
	char pattern[MAX_PATH];
	V_sprintf_safe( pattern, "%s/*.lss", kSplitsDir );
	FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
	for ( const char *name = g_pFullFileSystem->FindFirstEx( pattern, "MOD", &handle ); name; name = g_pFullFileSystem->FindNext( handle ) )
		files.emplace_back( name );
	if ( handle != FILESYSTEM_INVALID_FIND_HANDLE )
		g_pFullFileSystem->FindClose( handle );
	std::sort( files.begin(), files.end() );
	out.BeginArray();
	for ( const auto &f : files ) out.String( f );
	out.EndArray();
	return true;
}

bool Api_SplitsRead( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string file = args["file"].AsString();
	if ( !IsSafeSplitsName( file ) )
	{
		error = "invalid splits file name";
		return false;
	}
	char path[MAX_PATH];
	V_sprintf_safe( path, "%s/%s", kSplitsDir, file.c_str() );
	CUtlBuffer buf( static_cast<intp>( 0 ), 0, CUtlBuffer::TEXT_BUFFER );
	if ( !g_pFullFileSystem->ReadFile( path, "MOD", buf ) )
	{
		error = "cannot read splits file";
		return false;
	}
	std::string text( static_cast<const char *>( buf.Base() ), buf.TellPut() );
	// Strip a UTF-8 BOM (LiveSplit writes one).
	if ( text.size() >= 3 && static_cast<unsigned char>( text[0] ) == 0xEF && static_cast<unsigned char>( text[1] ) == 0xBB && static_cast<unsigned char>( text[2] ) == 0xBF )
		text.erase( 0, 3 );
	out.String( text );
	return true;
}

bool Api_SplitsWrite( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string file = args["file"].AsString();
	const std::string text = args["text"].AsString();
	if ( !IsSafeSplitsName( file ) || text.size() < 16 || text.size() > 32 * 1024 * 1024 )
	{
		error = "invalid splits file";
		return false;
	}
	char path[MAX_PATH];
	V_sprintf_safe( path, "%s/%s", kSplitsDir, file.c_str() );

	// Keep the user's original once, before the first time we rewrite it.
	char backup[MAX_PATH];
	V_sprintf_safe( backup, "%s.bak", path );
	if ( g_pFullFileSystem->FileExists( path, "MOD" ) && !g_pFullFileSystem->FileExists( backup, "MOD" ) )
	{
		CUtlBuffer orig;
		if ( g_pFullFileSystem->ReadFile( path, "MOD", orig ) )
			g_pFullFileSystem->WriteFile( backup, "MOD", orig );
	}

	CUtlBuffer buf;
	static const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
	buf.Put( bom, sizeof( bom ) );
	buf.Put( text.data(), static_cast<int>( text.size() ) );
	if ( !g_pFullFileSystem->WriteFile( path, "MOD", buf ) )
	{
		error = "cannot write splits file";
		return false;
	}
	out.Bool( true );
	return true;
}

bool Api_SplitsFolder( const json::Value &, json::Writer &out, std::string & )
{
	g_pFullFileSystem->CreateDirHierarchy( kSplitsDir, "MOD" );
	char full[MAX_PATH];
	if ( !g_pFullFileSystem->RelativePathToFullPath( kSplitsDir, "MOD", full, sizeof( full ) ) )
	{
		out.Bool( false );
		return true;
	}
	V_FixSlashes( full );
	ShellExecuteA( nullptr, "open", full, nullptr, nullptr, SW_SHOWNORMAL );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Replays (engine: ssm_replay_record / _play / _render). A replay is
// replays/<name>.dem plus <name>_2.dem, _3.dem ... and maybe <name>.mp4.
//-----------------------------------------------------------------------------
constexpr char kReplaysDir[] = "replays";

bool IsSafeReplayName( const std::string &name )
{
	if ( name.empty() || name.size() > 100 )
		return false;
	for ( char c : name )
	{
		if ( !( V_isalnum( c ) || c == '-' || c == '_' ) )
			return false;
	}
	return true;
}

bool FFmpegInstalled()
{
	char exe[MAX_PATH];
	if ( !GetModuleFileNameA( nullptr, exe, sizeof( exe ) ) )
		return false;
	V_StripFilename( exe );
	V_strcat_safe( exe, "\\bin\\ffmpeg.exe" );
	return GetFileAttributesA( exe ) != INVALID_FILE_ATTRIBUTES;
}

// "<name>_<n>" -> name, n when <name>.dem exists (map names end in _NN too);
// otherwise the stem is a replay of its own.
void SplitReplayPart( const std::string &stem, std::string &base, int &part )
{
	base = stem;
	part = 1;
	const size_t us = stem.rfind( '_' );
	if ( us == std::string::npos || us + 1 >= stem.size() )
		return;
	for ( size_t i = us + 1; i < stem.size(); ++i )
	{
		if ( !V_isdigit( stem[i] ) )
			return;
	}
	const int n = V_atoi( stem.c_str() + us + 1 );
	if ( n < 2 || n >= 10000 )
		return;
	char first[MAX_PATH];
	V_sprintf_safe( first, "%s/%s.dem", kReplaysDir, stem.substr( 0, us ).c_str() );
	if ( g_pFullFileSystem->FileExists( first, "MOD" ) )
	{
		base = stem.substr( 0, us );
		part = n;
	}
}

bool Api_ReplaysList( const json::Value &, json::Writer &out, std::string & )
{
	struct Replay
	{
		int parts = 0;
		int64 size = 0;
		time_t time = 0;
		bool mp4 = false;
	};
	std::map<std::string, Replay> replays;
	std::vector<std::string> mp4s;

	char pattern[MAX_PATH];
	V_sprintf_safe( pattern, "%s/*.*", kReplaysDir );
	FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
	for ( const char *name = g_pFullFileSystem->FindFirstEx( pattern, "MOD", &handle ); name; name = g_pFullFileSystem->FindNext( handle ) )
	{
		const std::string file = name;
		const size_t dot = file.rfind( '.' );
		if ( dot == std::string::npos )
			continue;
		const std::string stem = file.substr( 0, dot ), ext = file.substr( dot + 1 );
		if ( !V_stricmp( ext.c_str(), "mp4" ) )
		{
			mp4s.push_back( stem );
			continue;
		}
		if ( V_stricmp( ext.c_str(), "dem" ) )
			continue;
		std::string base;
		int part;
		SplitReplayPart( stem, base, part );
		char path[MAX_PATH];
		V_sprintf_safe( path, "%s/%s", kReplaysDir, name );
		Replay &r = replays[base];
		r.parts = max( r.parts, part );
		r.size += g_pFullFileSystem->Size( path, "MOD" );
		r.time = max( r.time, static_cast<time_t>( g_pFullFileSystem->GetFileTime( path, "MOD" ) ) );
	}
	if ( handle != FILESYSTEM_INVALID_FIND_HANDLE )
		g_pFullFileSystem->FindClose( handle );
	for ( const auto &m : mp4s )
	{
		auto it = replays.find( m );
		if ( it != replays.end() )
			it->second.mp4 = true;
	}

	std::vector<std::pair<std::string, Replay>> sorted( replays.begin(), replays.end() );
	std::sort( sorted.begin(), sorted.end(), []( const auto &a, const auto &b ) { return a.second.time > b.second.time; } );

	out.BeginObject();
	out.Key( "ffmpeg" ); out.Bool( FFmpegInstalled() );
	out.Key( "replays" );
	out.BeginArray();
	for ( const auto &[name, r] : sorted )
	{
		if ( !IsSafeReplayName( name ) )
			continue;
		out.BeginObject();
		out.Key( "name" ); out.String( name );
		out.Key( "parts" ); out.Int( r.parts );
		out.Key( "size" ); out.Number( static_cast<double>( r.size ) );
		out.Key( "time" ); out.Number( static_cast<double>( r.time ) );
		out.Key( "mp4" ); out.Bool( r.mp4 );
		out.EndObject();
	}
	out.EndArray();
	out.EndObject();
	return true;
}

// Demo playback controls: right mouse button during playback (engine:
// ssm_demo_ui_key) or any key bound to ssm_demo_ui. Opens the menu showing only
// the playback bar; again closes it.
ConVar ssm_demo_ui_open( "ssm_demo_ui_open", "0", FCVAR_HIDDEN | FCVAR_DONTRECORD, "" );

CON_COMMAND( ssm_demo_ui, "Show or hide the demo playback controls (while a demo plays)." )
{
	if ( GameUI().IsActive() )
	{
		engine->ClientCmd_Unrestricted( "gameui_hide\n" );
		return;
	}
	if ( !engine->IsPlayingDemo() )
		return;
	ssm_demo_ui_open.SetValue( 1 );
	engine->ClientCmd_Unrestricted( "gameui_activate\n" );
}

// Demo / replay playback position for the menu's playback bar.
bool Api_DemoState( const json::Value &, json::Writer &out, std::string & )
{
	out.BeginObject();
	const bool playing = engine->IsPlayingDemo();
	out.Prop( "playing", playing );
	out.Prop( "controls", ssm_demo_ui_open.GetBool() );
	if ( playing )
	{
		out.Prop( "loading", engine->IsLoadingDemo() );
		out.Prop( "tick", engine->GetDemoPlaybackTick() );
		out.Prop( "total", engine->GetDemoPlaybackTotalTicks() );
		out.Prop( "timescale", static_cast<double>( engine->GetDemoPlaybackTimeScale() ) );
		int part = 0, parts = 0, rendering = 0, paused = 0;
		static ConVarRef info( "ssm_demo_info" );
		if ( info.IsValid() )
			sscanf( info.GetString(), "%d %d %d %d", &part, &parts, &rendering, &paused );
		out.Prop( "part", part );
		out.Prop( "parts", parts );
		out.Prop( "rendering", rendering != 0 );
		out.Prop( "paused", paused != 0 );
	}
	out.EndObject();
	return true;
}

bool Api_ReplaysDelete( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string name = args["name"].AsString();
	if ( !IsSafeReplayName( name ) )
	{
		error = "invalid replay name";
		return false;
	}
	char path[MAX_PATH];
	V_sprintf_safe( path, "%s/%s.dem", kReplaysDir, name.c_str() );
	g_pFullFileSystem->RemoveFile( path, "MOD" );
	for ( int part = 2; part < 10000; ++part )
	{
		V_sprintf_safe( path, "%s/%s_%d.dem", kReplaysDir, name.c_str(), part );
		if ( !g_pFullFileSystem->FileExists( path, "MOD" ) )
			break;
		g_pFullFileSystem->RemoveFile( path, "MOD" );
	}
	V_sprintf_safe( path, "%s/%s.txt", kReplaysDir, name.c_str() );
	g_pFullFileSystem->RemoveFile( path, "MOD" );
	if ( args["video"].AsBool() )
	{
		V_sprintf_safe( path, "%s/%s.mp4", kReplaysDir, name.c_str() );
		g_pFullFileSystem->RemoveFile( path, "MOD" );
	}
	out.Bool( true );
	return true;
}

bool Api_ReplaysFolder( const json::Value &args, json::Writer &out, std::string & )
{
	g_pFullFileSystem->CreateDirHierarchy( kReplaysDir, "MOD" );
	char full[MAX_PATH];
	const std::string video = args["video"].AsString();
	char rel[MAX_PATH];
	if ( IsSafeReplayName( video ) )
		V_sprintf_safe( rel, "%s/%s.mp4", kReplaysDir, video.c_str() );
	else
		V_strcpy_safe( rel, kReplaysDir );
	if ( !g_pFullFileSystem->RelativePathToFullPath( rel, "MOD", full, sizeof( full ) ) )
	{
		out.Bool( false );
		return true;
	}
	V_FixSlashes( full );
	if ( IsSafeReplayName( video ) )
	{
		// Show the video selected in Explorer.
		char params[MAX_PATH + 16];
		V_sprintf_safe( params, "/select,\"%s\"", full );
		ShellExecuteA( nullptr, "open", "explorer.exe", params, nullptr, SW_SHOWNORMAL );
	}
	else
	{
		ShellExecuteA( nullptr, "open", full, nullptr, nullptr, SW_SHOWNORMAL );
	}
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Cutsceneless maps (optional download from the launcher into
// <game>\cutsceneless\maps). When on, that folder goes in front of every other
// GAME search path, so the next map loaded comes from it.
//-----------------------------------------------------------------------------
bool CutscenelessDir( char *pOut, int nOut )
{
	char exe[MAX_PATH];
	if ( !GetModuleFileNameA( nullptr, exe, sizeof( exe ) ) )
		return false;
	V_StripFilename( exe );
	V_snprintf( pOut, nOut, "%s\\cutsceneless", exe );
	char maps[MAX_PATH];
	V_sprintf_safe( maps, "%s\\maps", pOut );
	return GetFileAttributesA( maps ) != INVALID_FILE_ATTRIBUTES;
}

void CutscenelessChanged( IConVar *var, const char *, float )
{
	char dir[MAX_PATH];
	const bool bInstalled = CutscenelessDir( dir, sizeof( dir ) );
	g_pFullFileSystem->RemoveSearchPath( dir, "GAME" );
	if ( static_cast<ConVar *>( var )->GetBool() && bInstalled )
	{
		g_pFullFileSystem->AddSearchPath( dir, "GAME", PATH_ADD_TO_HEAD );
		ConMsg( "Cutsceneless maps on; they are used from the next map loaded.\n" );
	}
}

ConVar ssm_cutsceneless( "ssm_cutsceneless", "0", FCVAR_ARCHIVE,
	"Use the Cutsceneless maps (installed from the launcher). Start a new game after changing it.", CutscenelessChanged );

bool Api_SavesList( const json::Value &, json::Writer &out, std::string & )
{
	struct Save
	{
		std::string file, map, comment, elapsed, type;
		time_t time;
		unsigned size;
	};
	std::vector<Save> saves;

	char pattern[MAX_PATH];
	V_sprintf_safe( pattern, "%s/*.sav", kSaveDir );

	FileFindHandle_t handle = FILESYSTEM_INVALID_FIND_HANDLE;
	for ( const char *name = g_pFullFileSystem->FindFirstEx( pattern, "MOD", &handle ); name; name = g_pFullFileSystem->FindNext( handle ) )
	{
		if ( V_strnicmp( name, "HLSave", 6 ) == 0 )
			continue;

		char path[MAX_PATH];
		V_sprintf_safe( path, "%s/%s", kSaveDir, name );
		FileHandle_t f = g_pFullFileSystem->Open( path, "rb", "MOD" );
		if ( !f )
			continue;

		Save s;
		const int ok = ReadSaveHeader( f, s.map, s.comment );
		s.size = g_pFullFileSystem->Size( f );
		g_pFullFileSystem->Close( f );
		if ( !ok )
			continue;

		s.file = name;
		s.time = g_pFullFileSystem->GetFileTime( path, "MOD" );

		// The last 6 characters of the comment are the elapsed time (mmm:ss).
		if ( s.comment.size() >= 6 )
		{
			s.elapsed = s.comment.substr( s.comment.size() - 6 );
			s.comment.erase( s.comment.size() - 6 );
			while ( !s.comment.empty() && s.comment.back() == ' ' ) s.comment.pop_back();
			while ( !s.elapsed.empty() && s.elapsed.front() == ' ' ) s.elapsed.erase( 0, 1 );
		}

		if ( V_stristr( name, "quick" ) ) s.type = "quick";
		else if ( V_stristr( name, "autosave" ) ) s.type = "auto";
		else s.type = "manual";

		saves.push_back( std::move( s ) );
	}
	g_pFullFileSystem->FindClose( handle );

	std::sort( saves.begin(), saves.end(), []( const Save &a, const Save &b ) {
		if ( a.time != b.time ) return a.time > b.time;
		return a.file < b.file;
	} );

	out.BeginArray();
	for ( const auto &s : saves )
	{
		std::string base = s.file;
		const size_t dot = base.rfind( '.' );
		if ( dot != std::string::npos ) base.erase( dot );

		char thumb[MAX_PATH];
		V_sprintf_safe( thumb, "/img/tga/%s/%s.tga", kSaveDir, base.c_str() );

		out.BeginObject();
		out.Prop( "file", s.file );
		out.Prop( "name", base );
		out.Prop( "map", s.map );
		out.Key( "comment" );
		WriteLocalized( out, s.comment.c_str() );
		out.Prop( "elapsed", s.elapsed );
		out.Prop( "type", s.type );
		out.Prop( "time", static_cast<long long>( s.time ) );
		out.Prop( "size", static_cast<long long>( s.size ) );
		out.Prop( "thumb", thumb );
		out.EndObject();
	}
	out.EndArray();
	return true;
}

bool Api_SavesLoad( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string file = args["file"].AsString();
	if ( !IsSafeFileName( file ) )
	{
		error = "invalid save name";
		return false;
	}
	CFmtStrN<300> cmd( "progress_enable\nload %s\n", file.c_str() );
	engine->ClientCmd_Unrestricted( cmd );
	out.Bool( true );
	return true;
}

bool Api_SavesSave( const json::Value &args, json::Writer &out, std::string &error )
{
	if ( !GameUI().IsInLevel() || GameUI().IsInBackgroundLevel() || GameUI().IsInMultiplayer() )
	{
		error = "cannot save now";
		return false;
	}

	std::string name = args["file"].AsString();
	if ( !name.empty() )
	{
		if ( !IsSafeFileName( name ) )
		{
			error = "invalid save name";
			return false;
		}
		const size_t dot = name.rfind( ".sav" );
		if ( dot != std::string::npos ) name.erase( dot );
	}
	else
	{
		for ( int i = 0; i < 1000; ++i )
		{
			char path[MAX_PATH];
			V_sprintf_safe( path, "%s/half-life-%03i.sav", kSaveDir, i );
			if ( !g_pFullFileSystem->FileExists( path, "MOD" ) )
			{
				char buf[32];
				V_sprintf_safe( buf, "half-life-%03i", i );
				name = buf;
				break;
			}
		}
		if ( name.empty() )
		{
			error = "no free save slot";
			return false;
		}
	}

	CFmtStrN<300> cmd( "save %s\n", name.c_str() );
	engine->ClientCmd_Unrestricted( cmd );
	GameUI().SetSavedThisMenuSession( true );
	out.String( name );
	return true;
}

bool Api_SavesDelete( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string file = args["file"].AsString();
	if ( !IsSafeFileName( file ) || !V_stristr( file.c_str(), ".sav" ) )
	{
		error = "invalid save name";
		return false;
	}

	char path[MAX_PATH];
	V_sprintf_safe( path, "%s/%s", kSaveDir, file.c_str() );
	g_pFullFileSystem->RemoveFile( path, "MOD" );
	V_SetExtension( path, ".tga", sizeof( path ) );
	g_pFullFileSystem->RemoveFile( path, "MOD" );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Video
//-----------------------------------------------------------------------------
bool Api_VideoGet( const json::Value &, json::Writer &out, std::string & )
{
	vmode_t *modes = nullptr;
	int count = 0;
	gameuifuncs->GetVideoModes( &modes, &count );

	int desktopW = 0, desktopH = 0;
	gameuifuncs->GetDesktopResolution( desktopW, desktopH );

	const MaterialSystem_Config_t &config = materials->GetCurrentConfigForVideoCard();

	out.BeginObject();
	out.Prop( "width", config.m_VideoMode.m_Width );
	out.Prop( "height", config.m_VideoMode.m_Height );
	out.Prop( "windowed", config.Windowed() );
	out.Prop( "borderless", config.NoWindowBorder() );
	out.Prop( "desktopWidth", desktopW );
	out.Prop( "desktopHeight", desktopH );
	out.Key( "modes" ).BeginArray();
	std::vector<std::pair<int, int>> seen;
	for ( int i = 0; i < count; ++i )
	{
		const std::pair<int, int> wh( modes[i].width, modes[i].height );
		if ( std::find( seen.begin(), seen.end(), wh ) != seen.end() )
			continue;
		seen.push_back( wh );
		out.BeginObject().Prop( "width", wh.first ).Prop( "height", wh.second ).EndObject();
	}
	out.EndArray();
	out.EndObject();
	return true;
}

bool Api_VideoSet( const json::Value &args, json::Writer &out, std::string &error )
{
	const int width = args["width"].AsInt();
	const int height = args["height"].AsInt();
	const std::string mode = args["mode"].AsString( "fullscreen" );
	if ( width < 640 || height < 480 || width > 16384 || height > 16384 )
	{
		error = "invalid resolution";
		return false;
	}
	const bool borderless = mode == "borderless";
	const bool windowed = mode == "windowed" || borderless;
	CFmtStrN<128> cmd( "mat_setvideomode %d %d %d %d\n", width, height, windowed ? 1 : 0, borderless ? 1 : 0 );
	engine->ClientCmd_Unrestricted( cmd );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Key bindings
//-----------------------------------------------------------------------------
bool IsBindableButton( ButtonCode_t code )
{
	return ( code >= KEY_FIRST && code <= KEY_LAST ) || ( code >= MOUSE_FIRST && code <= MOUSE_LAST );
}

// binding command -> list of key names
std::map<std::string, std::vector<std::string>> CollectBindings()
{
	std::map<std::string, std::vector<std::string>> result;
	for ( int i = KEY_FIRST + 1; i <= MOUSE_LAST; ++i )
	{
		const ButtonCode_t code = static_cast<ButtonCode_t>( i );
		if ( !IsBindableButton( code ) )
			continue;
		const char *binding = gameuifuncs->GetBindingForButtonCode( code );
		if ( !binding || !*binding )
			continue;
		std::string key = binding;
		std::transform( key.begin(), key.end(), key.begin(), []( unsigned char c ) { return static_cast<char>( tolower( c ) ); } );
		result[key].push_back( g_pInputSystem->ButtonCodeToString( code ) );
	}
	return result;
}

bool Api_BindsList( const json::Value &, json::Writer &out, std::string & )
{
	CUtlBuffer buf( (intp)0, 0, CUtlBuffer::TEXT_BUFFER );
	if ( !g_pFullFileSystem->ReadFile( "scripts/kb_act.lst", nullptr, buf ) )
	{
		out.BeginArray().EndArray();
		return true;
	}
	buf.PutChar( '\0' );

	const auto bindings = CollectBindings();

	out.BeginArray();
	bool inSection = false;
	const char *data = buf.Base<const char>();
	std::string binding, description;
	while ( ( data = ParseToken( data, binding ) ) != nullptr )
	{
		data = ParseToken( data, description );
		if ( !data && description.empty() )
			break;
		if ( description.empty() || description[0] == '=' )
			continue;

		if ( V_stricmp( binding.c_str(), "blank" ) == 0 )
		{
			if ( inSection ) out.EndArray().EndObject();
			out.BeginObject();
			out.Key( "title" );
			WriteLocalized( out, description.c_str() );
			out.Key( "items" ).BeginArray();
			inSection = true;
			continue;
		}

		if ( !inSection )
		{
			out.BeginObject();
			out.Prop( "title", "" );
			out.Key( "items" ).BeginArray();
			inSection = true;
		}

		std::string lower = binding;
		std::transform( lower.begin(), lower.end(), lower.begin(), []( unsigned char c ) { return static_cast<char>( tolower( c ) ); } );

		out.BeginObject();
		out.Prop( "command", binding );
		out.Key( "label" );
		WriteLocalized( out, description.c_str() );
		out.Key( "keys" ).BeginArray();
		auto it = bindings.find( lower );
		if ( it != bindings.end() )
		{
			for ( const auto &k : it->second ) out.String( k );
		}
		out.EndArray();
		out.EndObject();
	}
	if ( inSection ) out.EndArray().EndObject();

	// SourceSpeedrunMod bindings (not part of kb_act.lst).
	struct BindItem { const char *command, *label; };
	static const BindItem kModBinds[] = {
		{ "movement_cycle", "Cycle movement mode" },
		{ "movement_hl2", "Movement: Half-Life 2 (ABH)" },
		{ "movement_oe", "Movement: Half-Life 2 old engine (AFH)" },
		{ "movement_hl1", "Movement: Half-Life 1" },
		{ "autobhop_toggle", "Toggle auto bunnyhop" },
	};
	// Speedrun tools (commands in HtmlUI.cpp; save/load/reload/kill are engine commands).
	static const BindItem kSpeedrunBinds[] = {
		{ "timer_split", "Timer: start / split" },
		{ "timer_reset", "Timer: reset" },
		{ "timer_undo", "Timer: undo split" },
		{ "timer_skip", "Timer: skip split" },
		{ "save quick", "Quick save" },
		{ "load quick", "Quick load" },
		{ "ssm_saveload", "Saveload (save + load)" },
		{ "ssm_savedeletion", "Save deletion (save \"\"; reload)" },
		{ "reload", "Reload (restart from the last save)" },
		{ "ssm_practice_save", "Practice save" },
		{ "ssm_practice_load", "Practice load" },
		{ "kill", "Kill (respawn)" },
		{ "+ssm_duckjump", "Duck + jump" },
		{ "ssm_showpos", "Toggle position display (cl_showpos)" },
		{ "ssm_demo_ui", "Demo playback controls (right mouse button while watching)" },
	};
	auto writeSection = [&]( const char *title, const BindItem *items, size_t count ) {
		out.BeginObject();
		out.Prop( "title", title );
		out.Key( "items" ).BeginArray();
		for ( size_t i = 0; i < count; ++i )
		{
			out.BeginObject();
			out.Prop( "command", items[i].command );
			out.Prop( "label", items[i].label );
			out.Key( "keys" ).BeginArray();
			auto it = bindings.find( items[i].command );
			if ( it != bindings.end() )
			{
				for ( const auto &k : it->second ) out.String( k );
			}
			out.EndArray();
			out.EndObject();
		}
		out.EndArray().EndObject();
	};
	writeSection( "SourceSpeedrunMod", kModBinds, std::size( kModBinds ) );
	writeSection( "Speedrun", kSpeedrunBinds, std::size( kSpeedrunBinds ) );

	out.EndArray();
	return true;
}

// Resolves {key:"name"} | {vk:n} | {mouse:n} | {wheel:+-1} to a key name.
bool ResolveKey( const json::Value &args, std::string &key, std::string &error )
{
	ButtonCode_t code = BUTTON_CODE_INVALID;
	if ( args.Has( "key" ) )
	{
		code = g_pInputSystem->StringToButtonCode( args["key"].AsString().c_str() );
	}
	else if ( args.Has( "vk" ) )
	{
		code = g_pInputSystem->VirtualKeyToButtonCode( args["vk"].AsInt() );
	}
	else if ( args.Has( "mouse" ) )
	{
		// DOM MouseEvent.button: 0 left, 1 middle, 2 right, 3 back, 4 forward
		static constexpr ButtonCode_t kMouse[] = { MOUSE_LEFT, MOUSE_MIDDLE, MOUSE_RIGHT, MOUSE_4, MOUSE_5 };
		const int b = args["mouse"].AsInt( -1 );
		if ( b >= 0 && b < static_cast<int>( std::size( kMouse ) ) ) code = kMouse[b];
	}
	else if ( args.Has( "wheel" ) )
	{
		code = args["wheel"].AsInt() > 0 ? MOUSE_WHEEL_UP : MOUSE_WHEEL_DOWN;
	}

	if ( code == BUTTON_CODE_INVALID || code == BUTTON_CODE_NONE || !IsBindableButton( code ) )
	{
		error = "unknown key";
		return false;
	}
	key = g_pInputSystem->ButtonCodeToString( code );
	return true;
}

bool Api_BindsKeyName( const json::Value &args, json::Writer &out, std::string &error )
{
	std::string key;
	if ( !ResolveKey( args, key, error ) )
		return false;
	out.String( key );
	return true;
}

bool Api_BindsSet( const json::Value &args, json::Writer &out, std::string &error )
{
	const std::string command = SanitizeArg( args["command"].AsString() );
	if ( command.empty() )
	{
		error = "missing 'command'";
		return false;
	}
	std::string key;
	if ( !ResolveKey( args, key, error ) )
		return false;

	// Escape and the console key are reserved.
	if ( V_stricmp( key.c_str(), "ESCAPE" ) == 0 )
	{
		error = "reserved key";
		return false;
	}

	CFmtStrN<512> cmd( "bind \"%s\" \"%s\"\n", key.c_str(), command.c_str() );
	engine->ClientCmd_Unrestricted( cmd );
	out.String( key );
	return true;
}

bool Api_BindsUnbind( const json::Value &args, json::Writer &out, std::string &error )
{
	std::string key;
	if ( !ResolveKey( args, key, error ) )
		return false;
	if ( V_stricmp( key.c_str(), "ESCAPE" ) == 0 )
	{
		error = "reserved key";
		return false;
	}
	CFmtStrN<128> cmd( "unbind \"%s\"\n", key.c_str() );
	engine->ClientCmd_Unrestricted( cmd );
	out.Bool( true );
	return true;
}

bool Api_BindsDefaults( const json::Value &, json::Writer &out, std::string &error )
{
	CUtlBuffer buf( (intp)0, 0, CUtlBuffer::TEXT_BUFFER );
	if ( !g_pFullFileSystem->ReadFile( "cfg/config_default.cfg", nullptr, buf ) )
	{
		error = "cfg/config_default.cfg not found";
		return false;
	}
	buf.PutChar( '\0' );

	std::string commands = "unbindall\n";
	const char *data = buf.Base<const char>();
	std::string token, key, binding;
	while ( ( data = ParseToken( data, token ) ) != nullptr )
	{
		if ( V_stricmp( token.c_str(), "bind" ) != 0 )
			continue;
		data = ParseToken( data, key );
		data = ParseToken( data, binding );
		if ( key.empty() || binding.empty() )
			break;
		commands += CFmtStrN<512>( "bind \"%s\" \"%s\"\n", SanitizeArg( key ).c_str(), SanitizeArg( binding ).c_str() ).Get();
		if ( !data ) break;
	}
	commands += "bind \"`\" \"toggleconsole\"\nbind \"ESCAPE\" \"cancelselect\"\n";
	engine->ClientCmd_Unrestricted( commands.c_str() );
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Console
//-----------------------------------------------------------------------------
bool Api_ConsoleSubmit( const json::Value &args, json::Writer &out, std::string & )
{
	std::string text = args["text"].AsString();
	while ( !text.empty() && ( text.back() == '\n' || text.back() == '\r' ) ) text.pop_back();

	HtmlUI().QueueConsoleText( ( "] " + text + "\n" ).c_str(), 160, 160, 160, 255, false );
	if ( !text.empty() )
	{
		text += '\n';
		engine->ClientCmd_Unrestricted( text.c_str() );
	}
	out.Bool( true );
	return true;
}

bool Api_ConsoleComplete( const json::Value &args, json::Writer &out, std::string & )
{
	const std::string text = args["text"].AsString();
	const int limit = clamp( args["limit"].AsInt( 24 ), 1, 200 );

	struct Match
	{
		std::string name, value, help;
		bool command;
	};
	std::vector<Match> matches;

	if ( !text.empty() && text.find( ' ' ) == std::string::npos )
	{
		ICvar::Iterator iter( g_pCVar );
		for ( iter.SetFirst(); iter.IsValid(); iter.Next() )
		{
			ConCommandBase *cmd = iter.Get();
			if ( cmd->IsFlagSet( FCVAR_DEVELOPMENTONLY ) || cmd->IsFlagSet( FCVAR_HIDDEN ) )
				continue;
			if ( V_strnicmp( cmd->GetName(), text.c_str(), static_cast<int>( text.size() ) ) != 0 )
				continue;

			Match m;
			m.name = cmd->GetName();
			m.command = cmd->IsCommand();
			m.help = cmd->GetHelpText() ? cmd->GetHelpText() : "";
			if ( !m.command ) m.value = static_cast<ConVar *>( cmd )->GetString();
			matches.push_back( std::move( m ) );
		}
	}

	std::sort( matches.begin(), matches.end(), []( const Match &a, const Match &b ) {
		if ( a.name.size() != b.name.size() ) return a.name.size() < b.name.size();
		return V_stricmp( a.name.c_str(), b.name.c_str() ) < 0;
	} );

	out.BeginArray();
	for ( size_t i = 0; i < matches.size() && static_cast<int>( i ) < limit; ++i )
	{
		const auto &m = matches[i];
		out.BeginObject();
		out.Prop( "name", m.name );
		out.Prop( "command", m.command );
		if ( !m.command ) out.Prop( "value", m.value );
		if ( !m.help.empty() ) out.Prop( "help", m.help );
		out.EndObject();
	}
	out.EndArray();
	return true;
}

bool Api_ConsoleClose( const json::Value &, json::Writer &out, std::string & )
{
	GameConsole().Hide();
	out.Bool( true );
	return true;
}

//-----------------------------------------------------------------------------
// Dispatch table
//-----------------------------------------------------------------------------
struct Method
{
	const char *name;
	Handler handler;
};

constexpr Method kMethods[] = {
	{ "ui.ready", Api_UiReady },
	{ "hud.ready", Api_HudReady },
	{ "ui.stats", Api_UiStats },
	{ "ui.glass", Api_UiGlass },
	{ "hud.glass", Api_HudGlass },
	{ "hud.flash", Api_HudFlash },
	{ "ui.escape", Api_UiEscape },
	{ "ui.reload", Api_UiReload },
	{ "ui.devtools", Api_UiDevTools },
	{ "state", Api_State },
	{ "cmd", Api_Cmd },
	{ "quit", Api_Quit },
	{ "cvar.get", Api_CvarGet },
	{ "cvar.set", Api_CvarSet },
	{ "config.save", Api_ConfigSave },
	{ "localize", Api_Localize },
	{ "localize.prefix", Api_LocalizePrefix },
	{ "game.info", Api_GameInfo },
	{ "sound.play", Api_SoundPlay },
	{ "chapters", Api_Chapters },
	{ "newgame", Api_NewGame },
	{ "saves.list", Api_SavesList },
	{ "splits.list", Api_SplitsList },
	{ "splits.read", Api_SplitsRead },
	{ "splits.write", Api_SplitsWrite },
	{ "splits.folder", Api_SplitsFolder },
	{ "saves.load", Api_SavesLoad },
	{ "saves.save", Api_SavesSave },
	{ "saves.delete", Api_SavesDelete },
	{ "replays.list", Api_ReplaysList },
	{ "replays.delete", Api_ReplaysDelete },
	{ "demo.state", Api_DemoState },
	{ "replays.folder", Api_ReplaysFolder },
	{ "video.get", Api_VideoGet },
	{ "video.set", Api_VideoSet },
	{ "binds.list", Api_BindsList },
	{ "binds.keyname", Api_BindsKeyName },
	{ "binds.set", Api_BindsSet },
	{ "binds.unbind", Api_BindsUnbind },
	{ "binds.defaults", Api_BindsDefaults },
	{ "console.submit", Api_ConsoleSubmit },
	{ "console.complete", Api_ConsoleComplete },
	{ "console.close", Api_ConsoleClose },
};

}  // namespace

std::string UiApi_StateJson()
{
	const char *level = engine->GetLevelName();
	const bool inLevel = GameUI().IsInLevel();
	const bool inBackground = GameUI().IsInBackgroundLevel();

	json::Writer w;
	w.BeginObject();
	w.Prop( "inLevel", inLevel );
	w.Prop( "inBackgroundLevel", inBackground );
	w.Prop( "inGame", inLevel && !inBackground );
	w.Prop( "multiplayer", GameUI().IsInMultiplayer() );
	w.Prop( "connected", engine->IsConnected() );
	w.Prop( "paused", engine->IsPaused() );
	w.Prop( "level", level ? level : "" );
	w.Prop( "maxClients", engine->GetMaxClients() );
	w.Prop( "consoleVisible", GameConsole().IsConsoleVisible() );
	w.Prop( "canSave", inLevel && !inBackground && !GameUI().IsInMultiplayer() );
	w.EndObject();
	return w.take();
}

bool UiApi_Dispatch( const char *method, const json::Value &args, json::Writer &result, std::string &error )
{
	for ( const auto &m : kMethods )
	{
		if ( V_strcmp( m.name, method ) == 0 )
			return m.handler( args, result, error );
	}
	error = "unknown method '";
	error += method;
	error += "'";
	return false;
}
