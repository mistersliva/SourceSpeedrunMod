// Copyright Source Authors. All rights reserved.
//
// HTML GameUI host. See HtmlUI.h.

#include "HtmlUI.h"

#include "winlite.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "tier0/icommandline.h"
#include "tier0/platform.h"
#include "tier1/convar.h"
#include "tier2/tier2.h"
#include "filesystem.h"
#include "ienginevgui.h"
#include "inputsystem/iinputsystem.h"
#include "vgui/IInput.h"
#include "vgui/IPanel.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "vgui/Cursor.h"
#include "bitmap/imageformat.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/itexture.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "tier1/KeyValues.h"
#include "tier3/tier3.h"
#include "GameUI/IHtmlHud.h"

#include "EngineInterface.h"
#include "Json.h"
#include "UiApi.h"
#include "UiAssets.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

extern IEngineVGui *enginevguifuncs;

ConVar htmlui_maxfps( "htmlui_maxfps", "0", FCVAR_ARCHIVE,
	"Maximum frame rate of the HTML UI (menus and HUD). 0 = match the game frame rate.", true, 0, true, 1000 );
ConVar htmlui_log_console( "htmlui_log_console", "1", FCVAR_ARCHIVE,
	"Echo HTML UI console messages (1 = warnings and errors, 2 = everything) to the game console." );

static CHtmlUI g_HtmlUI;

CHtmlUI &HtmlUI()
{
	return g_HtmlUI;
}

// Speedrun timer (HUD page, js/splits.js). LiveSplit-style hotkeys.
CON_COMMAND( timer_split, "Speedrun timer: start, or split if running." ) { HtmlUI().EmitHud( "timer", "{\"action\":\"split\"}" ); }
CON_COMMAND( timer_reset, "Speedrun timer: reset (saves best segments / PB)." ) { HtmlUI().EmitHud( "timer", "{\"action\":\"reset\"}" ); }
CON_COMMAND( timer_undo, "Speedrun timer: undo the last split." ) { HtmlUI().EmitHud( "timer", "{\"action\":\"undo\"}" ); }
CON_COMMAND( timer_skip, "Speedrun timer: skip the current split." ) { HtmlUI().EmitHud( "timer", "{\"action\":\"skip\"}" ); }

CON_COMMAND( htmlui_reload, "Reloads the HTML game UI (menu and HUD)." )
{
	HtmlUI().Reload();
}

CON_COMMAND( htmlui_devtools, "Opens Chromium DevTools for the HTML game UI. Pass 'hud' for the HUD page." )
{
	HtmlUI().ShowDevTools( args.ArgC() > 1 && V_stricmp( args[1], "hud" ) == 0 );
}

CON_COMMAND( htmlui_stats, "Prints HTML UI frame statistics (last second)." )
{
	const CHtmlUI::Stats &s = HtmlUI().GetStats();
	Msg( "engine %.0f fps, begin-frames %.0f/s, menu paints %.0f/s, hud paints %.0f/s\n",
		s.engineFps, s.beginFramesPerSec, s.menuPaintsPerSec, s.hudPaintsPerSec );
}

CON_COMMAND( htmlui_pixel, "Prints the BGRA value of a pixel of the HTML UI frame: htmlui_pixel <x> <y>" )
{
	htmlhost::View *view = HtmlUI().View();
	htmlhost::Frame frame;
	if ( !view || !view->AcquireFrame( frame ) )
	{
		Msg( "No frame.\n" );
		return;
	}
	const int x = clamp( atoi( args.Arg( 1 ) ), 0, frame.width - 1 );
	const int y = clamp( atoi( args.Arg( 2 ) ), 0, frame.height - 1 );
	const uint8_t *p = frame.bgra + ( static_cast<size_t>( y ) * frame.width + x ) * 4;
	Msg( "frame %dx%d serial %llu pixel(%d,%d) = B%d G%d R%d A%d\n", frame.width, frame.height,
		static_cast<unsigned long long>( frame.serial ), x, y, p[0], p[1], p[2], p[3] );
	view->ReleaseFrame();
}

namespace
{

// Directory containing gameui.dll (e.g. <game>/bin/x64), no trailing slash.
void GetModuleDir( char *out, size_t outSize )
{
	wchar_t path[MAX_PATH];
	DWORD n = ::GetModuleFileNameW( reinterpret_cast<HMODULE>( &__ImageBase ), path, static_cast<DWORD>( std::size( path ) ) );
	if ( n == 0 || n >= std::size( path ) )
	{
		out[0] = '\0';
		return;
	}
	wchar_t *slash = wcsrchr( path, L'\\' );
	if ( slash ) *slash = L'\0';
	::WideCharToMultiByte( CP_UTF8, 0, path, -1, out, static_cast<int>( outSize ), nullptr, nullptr );
}

// A dirty region larger than this fraction of the frame is uploaded whole.
constexpr float kFullUploadFraction = 0.6f;

}  // namespace

ConVar htmlui_glass( "htmlui_glass", "1", FCVAR_ARCHIVE, "Blur the game behind HTML UI glass panels.", true, 0, true, 1 );
ConVar htmlui_glass_debug( "htmlui_glass_debug", "0", 0, "Glass diagnostics: 1 = downscale only, no blur passes." );
ConVar htmlui_glass_brightness( "htmlui_glass_brightness", "1", FCVAR_ARCHIVE, "Brightness of the blurred glass background (1 = unchanged).", true, 0.1f, true, 8.0f );
ConVar htmlui_glass_passes( "htmlui_glass_passes", "2", FCVAR_ARCHIVE, "Number of Gaussian blur passes for HTML UI glass panels.", true, 0, true, 4 );
ConVar htmlui_glass_levels( "htmlui_glass_levels", "3", FCVAR_ARCHIVE, "Glass blur resolution: the frame is halved this many times before blurring (2 = 1/4, 3 = 1/8).", true, 1, true, 4 );

//-----------------------------------------------------------------------------
// Frosted glass. The back buffer is halved step by step (each 2:1 bilinear
// stretch is an exact 2x2 box filter, so no detail is skipped and the result
// does not shimmer when the camera moves), then blurred with the engine's
// separable 25-tap Gaussian at the smallest level and drawn, bilinearly
// upscaled, behind the page's glass panels. Computed at most once per frame.
// The targets are ours (not the bloom chain's _rt_SmallFB*), sized from the
// back buffer and recreated when it changes.
//-----------------------------------------------------------------------------
class CGlassBlur
{
public:
	static constexpr int kMaxLevels = 4;

	// Returns the vgui texture id holding the blurred frame, or -1.
	int Render( uint64_t frame )
	{
		if ( !Init() )
			return -1;
		if ( frame == m_nLastFrame )
			return m_nLastResult;
		m_nLastFrame = frame;
		m_nLastResult = -1;

		vgui::surface()->DrawFlushText();

		CMatRenderContextPtr ctx( materials );
		int w, h;
		ctx->GetRenderTargetDimensions( w, h );
		const int levels = htmlui_glass_levels.GetInt();
		if ( !EnsureTargets( w, h, levels ) )
			return -1;

		// Back buffer -> 1/2 -> 1/4 -> ... (each level copied from the one above).
		Rect_t src{ 0, 0, w, h };
		for ( int i = 0; i < levels; ++i )
		{
			ITexture *dstTex = m_pLevel[i];
			Rect_t dst{ 0, 0, dstTex->GetActualWidth(), dstTex->GetActualHeight() };
			if ( i > 0 )
				ctx->PushRenderTargetAndViewport( m_pLevel[i - 1] );
			ctx->CopyRenderTargetToTextureEx( dstTex, 0, &src, &dst );
			if ( i > 0 )
				ctx->PopRenderTargetAndViewport();
			src = dst;
		}

		// Separable Gaussian at the smallest level, ping-ponging with m_pTemp.
		ITexture *small = m_pLevel[levels - 1];
		const int sw = small->GetActualWidth(), sh = small->GetActualHeight();
		m_pBlurXBase->SetTextureValue( small );
		m_pBlurYBase->SetTextureValue( m_pTemp );
		const int passes = htmlui_glass_debug.GetInt() == 1 ? 0 : htmlui_glass_passes.GetInt();
		for ( int i = 0; i < passes; ++i )
		{
			if ( m_pBloomAmount )
				m_pBloomAmount->SetFloatValue( i == passes - 1 ? htmlui_glass_brightness.GetFloat() : 1.0f );
			ctx->PushRenderTargetAndViewport( m_pTemp );
			ctx->DrawScreenSpaceRectangle( m_pBlurX, 0, 0, sw, sh, 0, 0, sw - 1, sh - 1, sw, sh );
			ctx->PopRenderTargetAndViewport();
			ctx->PushRenderTargetAndViewport( small );
			ctx->DrawScreenSpaceRectangle( m_pBlurY, 0, 0, sw, sh, 0, 0, sw - 1, sh - 1, sw, sh );
			ctx->PopRenderTargetAndViewport();
		}

		m_pDrawBase->SetTextureValue( small );
		m_nLastResult = m_nTextureId;
		return m_nTextureId;
	}

private:
	bool Init()
	{
		if ( m_bInitialized )
			return m_bValid;
		m_bInitialized = true;

		// Created with a placeholder texture; the real targets are bound per frame.
		const char *placeholder = "_rt_SmallFB0";
		auto make = []( const char *name, const char *shader, const char *base ) {
			KeyValues *kv = new KeyValues( shader );
			kv->SetString( "$basetexture", base );
			if ( !V_stricmp( shader, "UnlitGeneric" ) )
			{
				kv->SetInt( "$ignorez", 1 );
				kv->SetInt( "$vertexcolor", 1 );
				kv->SetInt( "$vertexalpha", 1 );
				kv->SetInt( "$nofog", 1 );
				kv->SetInt( "$no_fullbright", 1 );
			}
			IMaterial *m = materials->CreateMaterial( name, kv );
			if ( m )
				m->Refresh();
			return m;
		};
		m_pBlurX = make( "__htmlui_glass_blurx", "BlurFilterX", placeholder );
		m_pBlurY = make( "__htmlui_glass_blury", "BlurFilterY", placeholder );
		m_pDraw = make( "__htmlui_glass", "UnlitGeneric", placeholder );

		auto bad = []( IMaterial *m ) { return !m || m->IsErrorMaterial(); };
		if ( bad( m_pBlurX ) || bad( m_pBlurY ) || bad( m_pDraw ) )
		{
			Warning( "HTML GameUI: glass blur unavailable (blur shaders missing).\n" );
			return m_bValid = false;
		}
		bool found = false;
		m_pBlurXBase = m_pBlurX->FindVar( "$basetexture", &found );
		m_pBlurYBase = m_pBlurY->FindVar( "$basetexture", &found );
		m_pDrawBase = m_pDraw->FindVar( "$basetexture", &found );
		m_pBloomAmount = m_pBlurY->FindVar( "$bloomamount", nullptr );
		if ( !m_pBlurXBase || !m_pBlurYBase || !m_pDrawBase )
			return m_bValid = false;

		m_nTextureId = vgui::surface()->CreateNewTextureID();
		g_pMatSystemSurface->DrawSetTextureMaterial( m_nTextureId, m_pDraw );
		return m_bValid = true;
	}

	bool EnsureTargets( int w, int h, int levels )
	{
		if ( w == m_nTargetW && h == m_nTargetH && levels == m_nTargetLevels )
			return m_pTemp != nullptr;
		ReleaseTargets();
		m_nTargetW = w;
		m_nTargetH = h;
		m_nTargetLevels = levels;

		// Unique names per size: the material system keys targets by name.
		++m_nGeneration;
		const unsigned flags = TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD;
		const ImageFormat fmt = IMAGE_FORMAT_RGBA8888;
		int lw = w, lh = h;
		char name[64];
		materials->BeginRenderTargetAllocation();
		for ( int i = 0; i < levels; ++i )
		{
			lw = MAX( 1, lw / 2 );
			lh = MAX( 1, lh / 2 );
			V_sprintf_safe( name, "_rt_htmlui_glass%d_%u", i, m_nGeneration );
			m_pLevel[i] = materials->CreateNamedRenderTargetTextureEx( name, lw, lh, RT_SIZE_LITERAL, fmt,
				MATERIAL_RT_DEPTH_NONE, flags, 0 );
		}
		V_sprintf_safe( name, "_rt_htmlui_glasstmp_%u", m_nGeneration );
		m_pTemp = materials->CreateNamedRenderTargetTextureEx( name, lw, lh, RT_SIZE_LITERAL, fmt,
			MATERIAL_RT_DEPTH_NONE, flags, 0 );
		materials->EndRenderTargetAllocation();

		bool ok = m_pTemp && !m_pTemp->IsError();
		for ( int i = 0; i < levels; ++i )
			ok = ok && m_pLevel[i] && !m_pLevel[i]->IsError();
		if ( !ok )
		{
			Warning( "HTML GameUI: could not create glass render targets.\n" );
			ReleaseTargets();
			m_nTargetW = w;  // don't retry every frame at this size
			m_nTargetH = h;
			m_nTargetLevels = levels;
			return false;
		}
		DevMsg( "HTML GameUI: glass targets %dx%d (%d levels)\n", lw, lh, levels );
		return true;
	}

	void ReleaseTargets()
	{
		// Unbind before deleting so no material keeps a dangling pointer.
		if ( m_bValid )
		{
			ITexture *fallback = materials->FindTexture( "_rt_SmallFB0", TEXTURE_GROUP_RENDER_TARGET, false );
			m_pBlurXBase->SetTextureValue( fallback );
			m_pBlurYBase->SetTextureValue( fallback );
			m_pDrawBase->SetTextureValue( fallback );
		}
		auto rel = []( ITexture *&t ) {
			if ( t )
			{
				t->DecrementReferenceCount();
				t->DeleteIfUnreferenced();
				t = nullptr;
			}
		};
		for ( ITexture *&t : m_pLevel )
			rel( t );
		rel( m_pTemp );
		m_nTargetW = m_nTargetH = m_nTargetLevels = 0;
	}

	bool m_bInitialized = false;
	bool m_bValid = false;
	uint64_t m_nLastFrame = ~0ull;
	int m_nLastResult = -1;
	int m_nTextureId = -1;
	int m_nTargetW = 0, m_nTargetH = 0, m_nTargetLevels = 0;
	unsigned m_nGeneration = 0;
	ITexture *m_pLevel[kMaxLevels] = {};
	ITexture *m_pTemp = nullptr;
	IMaterialVar *m_pBloomAmount = nullptr;
	IMaterialVar *m_pBlurXBase = nullptr, *m_pBlurYBase = nullptr, *m_pDrawBase = nullptr;
	IMaterial *m_pBlurX = nullptr, *m_pBlurY = nullptr, *m_pDraw = nullptr;
};

static CGlassBlur g_GlassBlur;

//-----------------------------------------------------------------------------
// Last game frame. Copied from the back buffer once per frame while a level
// is running, and drawn under the menu page while a level loads: the engine
// does not redraw the world during a load, and without this the screen would
// be black for every save/load (saveloads in particular), unlike the original
// game which keeps the last frame up.
//-----------------------------------------------------------------------------
class CFrozenFrame
{
public:
	void Capture( uint64_t frame )
	{
		if ( frame == m_nLastFrame )
			return;
		m_nLastFrame = frame;

		CMatRenderContextPtr ctx( materials );
		int w, h;
		ctx->GetRenderTargetDimensions( w, h );
		if ( !Ensure( w, h ) )
			return;
		vgui::surface()->DrawFlushText();
		Rect_t rc{ 0, 0, w, h };
		ctx->CopyRenderTargetToTextureEx( m_pTexture, 0, &rc, &rc );
		m_bValid = true;
	}

	// vgui texture id holding the last frame, or -1.
	int TextureId() const { return m_bValid ? m_nTextureId : -1; }

private:
	bool Ensure( int w, int h )
	{
		if ( m_pTexture && w == m_nW && h == m_nH )
			return true;
		if ( m_bFailed && w == m_nW && h == m_nH )
			return false;
		m_nW = w;
		m_nH = h;
		m_bValid = false;

		char name[64];
		V_sprintf_safe( name, "_rt_htmlui_frozen_%u", ++m_nGeneration );
		materials->BeginRenderTargetAllocation();
		ITexture *tex = materials->CreateNamedRenderTargetTextureEx( name, w, h, RT_SIZE_LITERAL, IMAGE_FORMAT_RGBA8888,
			MATERIAL_RT_DEPTH_NONE, TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD, 0 );
		materials->EndRenderTargetAllocation();
		if ( !tex || tex->IsError() )
		{
			m_bFailed = true;
			return false;
		}

		if ( !m_pMaterial )
		{
			KeyValues *kv = new KeyValues( "UnlitGeneric" );
			kv->SetString( "$basetexture", name );
			kv->SetInt( "$ignorez", 1 );
			kv->SetInt( "$vertexcolor", 1 );
			kv->SetInt( "$nofog", 1 );
			kv->SetInt( "$no_fullbright", 1 );
			m_pMaterial = materials->CreateMaterial( "__htmlui_frozen", kv );
			m_pMaterial->Refresh();
			m_pBase = m_pMaterial->FindVar( "$basetexture", nullptr );
			m_nTextureId = vgui::surface()->CreateNewTextureID();
			g_pMatSystemSurface->DrawSetTextureMaterial( m_nTextureId, m_pMaterial );
		}
		if ( m_pBase )
			m_pBase->SetTextureValue( tex );
		if ( m_pTexture )
		{
			m_pTexture->DecrementReferenceCount();
			m_pTexture->DeleteIfUnreferenced();
		}
		m_pTexture = tex;
		m_bFailed = false;
		return true;
	}

	ITexture *m_pTexture = nullptr;
	IMaterial *m_pMaterial = nullptr;
	IMaterialVar *m_pBase = nullptr;
	int m_nTextureId = -1;
	int m_nW = 0, m_nH = 0;
	unsigned m_nGeneration = 0;
	uint64_t m_nLastFrame = ~0ull;
	bool m_bValid = false;
	bool m_bFailed = false;
};

static CFrozenFrame g_FrozenFrame;

//-----------------------------------------------------------------------------
// In-game loads (save/load, saveloads, level transitions) don't show the game
// UI, and the engine draws empty frames while it loads: with the flip-model
// swap chain those are black (the old D3D9 swap kept the previous frame, which
// is what players expect). This root-level panel draws the last game frame
// and a small "LOADING" plaque over them until the client is back in game.
//-----------------------------------------------------------------------------
class CLoadFreezePanel : public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CLoadFreezePanel, vgui::Panel );

public:
	explicit CLoadFreezePanel( vgui::VPANEL parent ) : BaseClass( nullptr, "HtmlUILoadFreeze" )
	{
		SetParent( parent );
		SetPaintBackgroundEnabled( false );
		SetMouseInputEnabled( false );
		SetKeyBoardInputEnabled( false );
		SetProportional( false );
		SetZPos( 50 );
		SetVisible( true );
	}

	void Paint() override
	{
		int sw, sh;
		vgui::surface()->GetScreenSize( sw, sh );
		if ( GetWide() != sw || GetTall() != sh )
			SetBounds( 0, 0, sw, sh );

		if ( !HtmlUI().IsLoading() && !HtmlUI().IsHoldingFrame() )
			return;
		// The game UI (loading screen / menu) draws the frame itself.
		CHtmlPanel *menu = HtmlUI().Menu().Panel();
		if ( menu && menu->IsOnScreen() )
			return;
		const int tex = g_FrozenFrame.TextureId();
		if ( tex == -1 )
			return;

		vgui::ISurface *s = vgui::surface();
		s->DrawSetColor( 255, 255, 255, 255 );
		s->DrawSetTexture( tex );
		s->DrawTexturedRect( 0, 0, sw, sh );

		if ( m_hFont == vgui::INVALID_FONT )
		{
			m_hFont = s->CreateFont();
			const int tall = std::max( 14, sh / 50 );
			if ( !s->SetFontGlyphSet( m_hFont, "Bahnschrift", tall, 400, 0, 0, vgui::ISurface::FONTFLAG_ANTIALIAS ) )
				s->SetFontGlyphSet( m_hFont, "Segoe UI", tall, 400, 0, 0, vgui::ISurface::FONTFLAG_ANTIALIAS );
		}
		static const wchar_t kText[] = L"L O A D I N G";
		int tw = 0, th = 0;
		s->GetTextSize( m_hFont, kText, tw, th );
		const int padX = th, padY = th / 3;
		const int x1 = sw - sw * 24 / 1000, y1 = sh - sh * 32 / 1000;
		const int x0 = x1 - tw - padX * 2, y0 = y1 - th - padY * 2;
		s->DrawSetColor( 20, 22, 26, 200 );
		s->DrawFilledRect( x0, y0, x1, y1 );
		s->DrawSetTextFont( m_hFont );
		s->DrawSetTextColor( 232, 232, 232, 255 );
		s->DrawSetTextPos( x0 + padX, y0 + padY );
		s->DrawPrintText( kText, static_cast<int>( std::size( kText ) ) - 1 );
	}

private:
	vgui::HFont m_hFont = vgui::INVALID_FONT;
};

static CLoadFreezePanel *g_pLoadFreezePanel = nullptr;
static uint64_t g_nPaintFrame = 0;  // incremented once per engine frame

//-----------------------------------------------------------------------------
// CHtmlPanel
//-----------------------------------------------------------------------------
CHtmlPanel::CHtmlPanel( vgui::VPANEL parent, const char *name, bool interactive )
	: BaseClass( nullptr, name ),
	m_pView( nullptr ),
	m_bInteractive( interactive ),
	m_nTextureId( -1 ),
	m_nTextureWide( 0 ),
	m_nTextureTall( 0 ),
	m_nUploadedSerial( 0 ),
	m_nSerialAtShow( 0 ),
	m_flShownTime( 0 ),
	m_nLastX( 0 ),
	m_nLastY( 0 ),
	m_nPendingCursor( -1 )
{
	SetParent( parent );
	if ( interactive )
	{
		MakePopup( false, false );
	}

	int wide, tall;
	vgui::surface()->GetScreenSize( wide, tall );
	SetBounds( 0, 0, wide, tall );

	SetPaintBackgroundEnabled( false );
	SetPaintBorderEnabled( false );
	SetPaintEnabled( true );
	SetMouseInputEnabled( interactive );
	SetKeyBoardInputEnabled( interactive );
	if ( interactive )
		SetCursor( vgui::dc_arrow );
	SetVisible( true );

	vgui::ivgui()->AddTickSignal( GetVPanel() );
}

CHtmlPanel::~CHtmlPanel()
{
	ReleaseTexture();
}

void CHtmlPanel::ReleaseTexture()
{
	if ( m_nTextureId != -1 && vgui::surface() )
	{
		vgui::surface()->DeleteTextureByID( m_nTextureId );
	}
	m_nTextureId = -1;
	m_nTextureWide = m_nTextureTall = 0;
	m_nUploadedSerial = 0;
}

bool CHtmlPanel::IsOnScreen() const
{
	return vgui::ipanel()->IsFullyVisible( const_cast<CHtmlPanel *>( this )->GetVPanel() );
}

void CHtmlPanel::OnShown()
{
	m_nSerialAtShow = m_nUploadedSerial;
	m_flShownTime = Plat_FloatTime();
}

void CHtmlPanel::OnThink()
{
	// Keep fullscreen.
	int wide, tall;
	vgui::surface()->GetScreenSize( wide, tall );
	int myWide, myTall;
	GetSize( myWide, myTall );
	if ( myWide != wide || myTall != tall )
	{
		SetBounds( 0, 0, wide, tall );
	}

	const int cursor = m_nPendingCursor.exchange( -1 );
	if ( cursor >= 0 && m_bInteractive )
	{
		using C = htmlhost::IViewListener::Cursor;
		vgui::HCursor hc = vgui::dc_arrow;
		switch ( static_cast<C>( cursor ) )
		{
			case C::Hand: hc = vgui::dc_hand; break;
			case C::IBeam: hc = vgui::dc_ibeam; break;
			case C::Wait: hc = vgui::dc_waitarrow; break;
			case C::Crosshair: hc = vgui::dc_crosshair; break;
			case C::SizeWE: hc = vgui::dc_sizewe; break;
			case C::SizeNS: hc = vgui::dc_sizens; break;
			case C::SizeAll: hc = vgui::dc_sizeall; break;
			case C::No: hc = vgui::dc_no; break;
			case C::None: hc = vgui::dc_blank; break;
			default: hc = vgui::dc_arrow; break;
		}
		SetCursor( hc );
	}
}

void CHtmlPanel::OnSizeChanged( int wide, int tall )
{
	BaseClass::OnSizeChanged( wide, tall );
	if ( m_pView )
	{
		m_pView->Resize( wide, tall );
	}
}

void CHtmlPanel::Paint()
{
	if ( !m_pView )
		return;

	// Keep a copy of the game frame (before any HTML is drawn on top) for loads.
	if ( !HtmlUI().IsLoading() && !HtmlUI().IsHoldingFrame() && engine->IsInGame() )
	{
		const char *level = engine->GetLevelName();
		if ( level && level[0] && !engine->IsLevelMainMenuBackground() )
			g_FrozenFrame.Capture( g_nPaintFrame );
	}

	// Without a map the engine does not redraw (or clear) the frame, so the
	// translucent page would smear over previous frames. While a level loads
	// show the last game frame (like the original game); otherwise black.
	// (Drawn before the page frame is checked: the page may not have a fresh
	// frame yet right after being shown, and the screen must not go black.)
	if ( m_bInteractive )
	{
		int pw, ph;
		GetSize( pw, ph );
		const char *level = engine->GetLevelName();
		const bool noLevel = !level || !level[0];
		const bool loading = HtmlUI().IsLoading() || HtmlUI().IsHoldingFrame();
		if ( noLevel || loading )
		{
			const int frozen = loading ? g_FrozenFrame.TextureId() : -1;
			if ( frozen != -1 )
			{
				vgui::surface()->DrawSetColor( 255, 255, 255, 255 );
				vgui::surface()->DrawSetTexture( frozen );
				vgui::surface()->DrawTexturedRect( 0, 0, pw, ph );
			}
			else if ( noLevel )
			{
				vgui::surface()->DrawSetColor( 0, 0, 0, 255 );
				vgui::surface()->DrawFilledRect( 0, 0, pw, ph );
			}
		}
	}


	htmlhost::Frame frame;
	if ( !m_pView->AcquireFrame( frame ) )
		return;

	// Right after being shown, the buffer may still hold what was on screen
	// before the page was hidden; wait (briefly) for a fresh frame.
	if ( frame.serial == m_nSerialAtShow && m_nSerialAtShow != 0 && Plat_FloatTime() - m_flShownTime < 0.25 )
	{
		m_pView->ReleaseFrame();
		return;
	}
	m_nSerialAtShow = 0;

	bool consumed = false;
	if ( m_nTextureId != -1 && ( frame.width != m_nTextureWide || frame.height != m_nTextureTall ) )
	{
		// The engine sizes the backing texture on first upload; recreate on resize.
		ReleaseTexture();
	}

	if ( m_nTextureId == -1 )
	{
		m_nTextureId = vgui::surface()->CreateNewTextureID( true );
		m_nTextureWide = frame.width;
		m_nTextureTall = frame.height;
		vgui::surface()->DrawSetTextureRGBAEx( m_nTextureId, frame.bgra, frame.width, frame.height, IMAGE_FORMAT_BGRA8888 );
		m_nUploadedSerial = frame.serial;
		consumed = true;
	}
	else if ( frame.serial != m_nUploadedSerial )
	{
		if ( frame.dirty_w > 0 && frame.dirty_h > 0 )
		{
			const float fraction = static_cast<float>( frame.dirty_w ) * frame.dirty_h / ( static_cast<float>( frame.width ) * frame.height );
			if ( fraction >= kFullUploadFraction )
			{
				vgui::surface()->DrawSetTextureRGBAEx( m_nTextureId, frame.bgra, frame.width, frame.height, IMAGE_FORMAT_BGRA8888 );
			}
			else
			{
				// Uploads only the changed rows/columns; |bgra| is the whole frame.
				vgui::surface()->DrawUpdateRegionTextureRGBA( m_nTextureId, frame.dirty_x, frame.dirty_y, frame.bgra,
					frame.dirty_w, frame.dirty_h, IMAGE_FORMAT_BGRA8888 );
			}
		}
		m_nUploadedSerial = frame.serial;
		consumed = true;
	}

	const int w = frame.width, h = frame.height;
	m_pView->ReleaseFrame( consumed );

	// Frosted glass behind the page's glass panels.
	if ( !m_GlassRects.empty() && htmlui_glass.GetBool() )
	{
		const int glassTex = g_GlassBlur.Render( g_nPaintFrame );
		if ( glassTex != -1 )
		{
			const float su = 1.0f / w, sv = 1.0f / h;
			vgui::surface()->DrawSetTexture( glassTex );
			auto span = [&]( int x0, int y0, int x1, int y1 ) {
				if ( x1 > x0 && y1 > y0 )
					vgui::surface()->DrawTexturedSubRect( x0, y0, x1, y1, x0 * su, y0 * sv, x1 * su, y1 * sv );
			};
			for ( const GlassRect &r : m_GlassRects )
			{
				vgui::surface()->DrawSetColor( 255, 255, 255, static_cast<int>( r.alpha * 255.0f + 0.5f ) );
				// Rounded corners (matching CSS border-radius) as one row per
				// pixel of the radius, the middle as a single quad.
				const int rad = static_cast<int>( ceilf( r.radius ) );
				for ( int i = 0; i < rad; ++i )
				{
					const float dy = r.radius - ( i + 0.5f );
					const int inset = static_cast<int>( r.radius - sqrtf( MAX( 0.0f, r.radius * r.radius - dy * dy ) ) + 0.5f );
					span( r.x + inset, r.y + i, r.x + r.w - inset, r.y + i + 1 );
					span( r.x + inset, r.y + r.h - i - 1, r.x + r.w - inset, r.y + r.h - i );
				}
				span( r.x, r.y + rad, r.x + r.w, r.y + r.h - rad );
			}
		}
	}

	// Damage flash: fades in quickly, out slowly, from the screen edges.
	if ( m_Flash.duration > 0 )
	{
		const double t = ( Plat_FloatTime() - m_Flash.start ) / m_Flash.duration;
		if ( t >= 1.0 || t < 0.0 )
		{
			m_Flash.duration = 0;
		}
		else
		{
			const double k = t < 0.1 ? t / 0.1 : 1.0 - ( t - 0.1 ) / 0.9;
			const unsigned a = static_cast<unsigned>( m_Flash.alpha * 255.0 * k + 0.5 );
			const int ew = w * 24 / 100, eh = h * 28 / 100;
			vgui::ISurface *s = vgui::surface();
			s->DrawSetColor( m_Flash.r, m_Flash.g, m_Flash.b, 255 );
			s->DrawFilledRectFade( 0, 0, ew, h, a, 0, true );
			s->DrawFilledRectFade( w - ew, 0, w, h, 0, a, true );
			s->DrawFilledRectFade( 0, 0, w, eh, a, 0, false );
			s->DrawFilledRectFade( 0, h - eh, w, h, 0, a, false );
		}
	}

	vgui::surface()->DrawSetColor( 255, 255, 255, 255 );
	vgui::surface()->DrawSetTexture( m_nTextureId );
	vgui::surface()->DrawTexturedRect( 0, 0, w, h );
}

void CHtmlPanel::Flash( int r, int g, int b, float alpha, float seconds )
{
	m_Flash.r = clamp( r, 0, 255 );
	m_Flash.g = clamp( g, 0, 255 );
	m_Flash.b = clamp( b, 0, 255 );
	m_Flash.alpha = clamp( alpha, 0.0f, 1.0f );
	m_Flash.duration = clamp( seconds, 0.0f, 5.0f );
	m_Flash.start = Plat_FloatTime();
}

uint32_t CHtmlPanel::Modifiers() const
{
	uint32_t m = 0;
	vgui::IInput *in = vgui::input();
	if ( in->IsKeyDown( KEY_LSHIFT ) || in->IsKeyDown( KEY_RSHIFT ) ) m |= htmlhost::kModShift;
	if ( in->IsKeyDown( KEY_LCONTROL ) || in->IsKeyDown( KEY_RCONTROL ) ) m |= htmlhost::kModCtrl;
	if ( in->IsKeyDown( KEY_LALT ) || in->IsKeyDown( KEY_RALT ) ) m |= htmlhost::kModAlt;
	if ( in->IsMouseDown( MOUSE_LEFT ) ) m |= htmlhost::kModLeftButton;
	if ( in->IsMouseDown( MOUSE_MIDDLE ) ) m |= htmlhost::kModMiddleButton;
	if ( in->IsMouseDown( MOUSE_RIGHT ) ) m |= htmlhost::kModRightButton;
	return m;
}

// The engine keeps delivering cursor moves and button presses to vgui while
// the game window is minimized or behind another application. Only pass them
// to the page when the game window is the active one and the cursor is
// actually over it; otherwise hover sounds play and clicks land "through"
// other windows.
static ConVar htmlui_mouse_focus_check( "htmlui_mouse_focus_check", "1", 0, "Ignore mouse input to the HTML UI while the game window is inactive or covered." );

static bool GameWindowHasMouse()
{
	if ( !htmlui_mouse_focus_check.GetBool() )
		return true;
	HWND fg = ::GetForegroundWindow();
	if ( !fg || ::IsIconic( fg ) )
		return false;
	DWORD pid = 0;
	::GetWindowThreadProcessId( fg, &pid );
	if ( pid != ::GetCurrentProcessId() )
		return false;
	POINT pt;
	if ( !::GetCursorPos( &pt ) )
		return false;
	HWND under = ::WindowFromPoint( pt );
	if ( !under )
		return false;
	::GetWindowThreadProcessId( under, &pid );
	return pid == ::GetCurrentProcessId();
}

static bool ToBrowserButton( vgui::MouseCode code, htmlhost::MouseButton &out )
{
	switch ( code )
	{
		case MOUSE_LEFT: out = htmlhost::MouseButton::Left; return true;
		case MOUSE_RIGHT: out = htmlhost::MouseButton::Right; return true;
		case MOUSE_MIDDLE: out = htmlhost::MouseButton::Middle; return true;
		default: return false;
	}
}

void CHtmlPanel::OnCursorMoved( int x, int y )
{
	m_nLastX = x;
	m_nLastY = y;
	if ( !m_pView || !m_bInteractive )
		return;
	if ( !GameWindowHasMouse() )
	{
		// Tell the page the mouse left, once, so hover states clear.
		if ( !m_bMouseOutside )
			m_pView->MouseMove( x, y, 0, true );
		m_bMouseOutside = true;
		return;
	}
	m_bMouseOutside = false;
	m_pView->MouseMove( x, y, Modifiers(), false );
}

void CHtmlPanel::OnCursorExited()
{
	if ( m_pView && m_bInteractive )
		m_pView->MouseMove( m_nLastX, m_nLastY, Modifiers(), true );
}

void CHtmlPanel::OnMousePressed( vgui::MouseCode code )
{
	if ( !m_bInteractive || !GameWindowHasMouse() )
		return;
	RequestFocus();
	htmlhost::MouseButton b;
	if ( m_pView && ToBrowserButton( code, b ) )
		m_pView->MouseButton( m_nLastX, m_nLastY, b, false, 1, Modifiers() );
}

void CHtmlPanel::OnMouseDoublePressed( vgui::MouseCode code )
{
	htmlhost::MouseButton b;
	if ( m_pView && m_bInteractive && GameWindowHasMouse() && ToBrowserButton( code, b ) )
		m_pView->MouseButton( m_nLastX, m_nLastY, b, false, 2, Modifiers() );
}

void CHtmlPanel::OnMouseReleased( vgui::MouseCode code )
{
	htmlhost::MouseButton b;
	if ( m_pView && m_bInteractive && ToBrowserButton( code, b ) )
		m_pView->MouseButton( m_nLastX, m_nLastY, b, true, 1, Modifiers() );
}

void CHtmlPanel::OnMouseWheeled( int delta )
{
	if ( m_pView && m_bInteractive && GameWindowHasMouse() )
		m_pView->MouseWheel( m_nLastX, m_nLastY, 0, delta * 120, Modifiers() );
}

void CHtmlPanel::OnKeyCodePressed( vgui::KeyCode )
{
	// Handled in OnKeyCodeTyped (which also fires on auto-repeat).
}

void CHtmlPanel::OnKeyCodeTyped( vgui::KeyCode code )
{
	if ( !m_pView || !m_bInteractive )
		return;
	const int vk = g_pInputSystem ? g_pInputSystem->ButtonCodeToVirtualKey( code ) : 0;
	if ( vk )
		m_pView->Key( htmlhost::KeyType::RawKeyDown, vk, 0, Modifiers() );
}

void CHtmlPanel::OnKeyCodeReleased( vgui::KeyCode code )
{
	if ( !m_pView || !m_bInteractive )
		return;
	const int vk = g_pInputSystem ? g_pInputSystem->ButtonCodeToVirtualKey( code ) : 0;
	if ( vk )
		m_pView->Key( htmlhost::KeyType::KeyUp, vk, 0, Modifiers() );
}

void CHtmlPanel::OnKeyTyped( wchar_t unichar )
{
	if ( !m_pView || !m_bInteractive || unichar == 0 )
		return;
	// Ctrl+<letter> etc. arrive as control characters; Chromium handles those
	// through the raw key events already.
	if ( unichar < 0x20 && unichar != L'\r' && unichar != L'\t' && unichar != L'\b' )
		return;
	m_pView->Key( htmlhost::KeyType::Char, 0, static_cast<char16_t>( unichar ), Modifiers() );
}

void CHtmlPanel::OnSetFocus()
{
	BaseClass::OnSetFocus();
	if ( m_pView && m_bInteractive )
		m_pView->SetFocus( true );
}

void CHtmlPanel::OnKillFocus()
{
	BaseClass::OnKillFocus();
	if ( m_pView && m_bInteractive )
		m_pView->SetFocus( false );
}

void CHtmlPanel::SetBrowserCursor( htmlhost::IViewListener::Cursor cursor )
{
	m_nPendingCursor = static_cast<int>( cursor );
}

//-----------------------------------------------------------------------------
// CHtmlSurface
//-----------------------------------------------------------------------------
CHtmlSurface::CHtmlSurface( const char *name, bool interactive )
	: m_bInteractive( interactive ),
	m_pPanel( nullptr ),
	m_pView( nullptr ),
	m_bReady( false ),
	m_bNeedsReload( false ),
	m_bWasOnScreen( true )
{
	V_strcpy_safe( m_szName, name );
}

bool CHtmlSurface::Create( vgui::VPANEL parent, const char *url )
{
	if ( m_pView )
		return true;

	char panelName[64];
	V_sprintf_safe( panelName, "Html_%s", m_szName );
	m_pPanel = new CHtmlPanel( parent, panelName, m_bInteractive );

	int wide, tall;
	m_pPanel->GetSize( wide, tall );

	// The frame rate is driven by the engine (external begin frames); this is
	// only an upper bound for Chromium's own scheduling.
	m_pView = htmlhost::CreateView( url, wide, tall, this, 1000 );
	if ( !m_pView )
	{
		m_pPanel->MarkForDeletion();
		m_pPanel = nullptr;
		return false;
	}
	m_pPanel->SetView( m_pView );
	return true;
}

void CHtmlSurface::Destroy()
{
	m_bReady = false;
	if ( m_pPanel )
		m_pPanel->SetView( nullptr );
	if ( m_pView )
	{
		m_pView->Destroy();
		m_pView = nullptr;
	}
	if ( m_pPanel )
	{
		m_pPanel->SetParent( (vgui::VPANEL)0 );
		m_pPanel->MarkForDeletion();
		m_pPanel = nullptr;
	}
	m_PendingEvents.clear();
}

void CHtmlSurface::Emit( const char *event, const char *jsonPayload )
{
	std::string script = "window.__sourceEvent&&window.__sourceEvent(";
	script += json::Quote( event );
	script += ",";
	script += jsonPayload && *jsonPayload ? jsonPayload : "null";
	script += ");";
	m_PendingEvents.push_back( std::move( script ) );

	if ( m_bReady )
		Flush();
}

void CHtmlSurface::Flush()
{
	if ( !m_bReady || !m_pView || m_PendingEvents.empty() )
		return;

	std::string all;
	for ( auto &e : m_PendingEvents )
	{
		all += e;
		all += '\n';
	}
	m_PendingEvents.clear();
	m_pView->ExecuteScript( all.c_str() );
}

void CHtmlSurface::UpdateVisibility( bool forceVisible )
{
	if ( !m_pView || !m_pPanel )
		return;

	if ( m_bNeedsReload.exchange( false ) )
	{
		m_bReady = false;
		m_pView->Reload( true );
	}

	const bool onScreen = forceVisible || m_pPanel->IsOnScreen();
	if ( onScreen && !m_bWasOnScreen )
		m_pPanel->OnShown();
	m_bWasOnScreen = onScreen;
	m_pView->SetVisible( onScreen );
}

void CHtmlSurface::BeginFrame()
{
	if ( m_pView )
		m_pView->BeginFrame();
}

bool CHtmlSurface::HasNewFrame() const
{
	if ( !m_pView || !m_pPanel )
		return false;
	htmlhost::Frame frame;
	if ( !m_pView->AcquireFrame( frame ) )
		return false;
	const bool fresh = frame.serial != m_pPanel->UploadedSerial();
	m_pView->ReleaseFrame();
	return fresh;
}

void CHtmlSurface::OnCursorChanged( Cursor cursor )
{
	if ( m_pPanel )
		m_pPanel->SetBrowserCursor( cursor );
}

void CHtmlSurface::OnConsoleMessage( int level, const char *message, const char *source, int line )
{
	// cef_log_severity_t: 1 verbose, 2 info, 3 warning, 4 error
	if ( level >= 3 && htmlui_log_console.GetBool() )
	{
		const char *file = source ? V_UnqualifiedFileName( source ) : "";
		Warning( "[htmlui:%s] %s (%s:%d)\n", m_szName, message, file, line );
	}
	else if ( level >= 1 && htmlui_log_console.GetInt() > 1 )
	{
		Msg( "[htmlui:%s] %s\n", m_szName, message );
	}
}

void CHtmlSurface::OnLoadEnd( int httpStatus )
{
	if ( httpStatus != 200 )
		Warning( "HTML GameUI (%s): page loaded with HTTP status %d\n", m_szName, httpStatus );
}

void CHtmlSurface::OnLoadError( int errorCode, const char *errorText, const char *url )
{
	Warning( "HTML GameUI (%s): failed to load %s: %s (%d)\n", m_szName, url, errorText, errorCode );
}

void CHtmlSurface::OnRendererCrashed()
{
	Warning( "HTML GameUI (%s): renderer process terminated, reloading.\n", m_szName );
	m_bReady = false;
	m_bNeedsReload = true;
}

//-----------------------------------------------------------------------------
// IHtmlHud implementation (used by the client DLL)
//-----------------------------------------------------------------------------
class CHtmlHudInterface : public IHtmlHud
{
public:
	bool SetEnabled( bool enabled ) override { return HtmlUI().EnableHud( enabled ); }
	bool IsEnabled() override { return HtmlUI().IsHudEnabled(); }
	void SetState( const char *json ) override { HtmlUI().SetHudState( json ); }
	void Event( const char *name, const char *json ) override { HtmlUI().EmitHud( name, json ); }
};

static CHtmlHudInterface g_HtmlHudInterface;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CHtmlHudInterface, IHtmlHud, HTMLHUD_INTERFACE_VERSION, g_HtmlHudInterface );

//-----------------------------------------------------------------------------
// CHtmlUI
//-----------------------------------------------------------------------------
CHtmlUI::CHtmlUI()
	: m_Menu( "menu", true ),
	m_Hud( "hud", false ),
	m_bHudStateDirty( false ),
	m_bInitialized( false ),
	m_bHudEnabled( false ),
	m_bLoading( false ),
	m_bPreventEscape( false ),
	m_bEscapeWasDown( false ),
	m_flLastBeginFrame( 0 )
{
}

bool CHtmlUI::Init( vgui::VPANEL menuParent )
{
	char binDir[MAX_PATH];
	GetModuleDir( binDir, sizeof( binDir ) );

	char cefDir[MAX_PATH], subprocess[MAX_PATH], cacheDir[MAX_PATH], logFile[MAX_PATH];
	V_sprintf_safe( cefDir, "%s\\cef", binDir );
	V_sprintf_safe( subprocess, "%s\\cef\\gameui_cefsub.exe", binDir );
	V_sprintf_safe( cacheDir, "%s\\cef\\cache", binDir );
	V_sprintf_safe( logFile, "%s\\cef\\cef.log", binDir );

	int debugPort = 0;
	if ( CommandLine()->HasParm( "-htmlui_debug" ) )
	{
		debugPort = CommandLine()->ParmValue( "-htmlui_debug", 9222 );
		if ( debugPort <= 0 ) debugPort = 9222;
	}

	htmlhost::InitParams params{};
	params.cef_dir = cefDir;
	params.subprocess_path = subprocess;
	params.cache_dir = cacheDir;
	params.log_file = logFile;
	params.locale = "en-US";
	params.remote_debugging_port = debugPort;
	params.request_handler = this;

	const char *error = nullptr;
	if ( !htmlhost::Init( params, &error ) )
	{
		Error( "HTML GameUI: failed to initialize Chromium Embedded Framework (%s).\n"
			"Make sure %s contains the CEF runtime.\n", error ? error : "unknown error", cefDir );
	}

	char url[256];
	V_sprintf_safe( url, "%s/ui/index.html", htmlhost::kOrigin );
	if ( !m_Menu.Create( menuParent, url ) )
	{
		Error( "HTML GameUI: unable to create browser view.\n" );
	}

	g_pLoadFreezePanel = new CLoadFreezePanel( enginevguifuncs->GetPanel( PANEL_ROOT ) );

	if ( debugPort )
	{
		Msg( "HTML GameUI: DevTools available at http://127.0.0.1:%d\n", debugPort );
	}

	m_bInitialized = true;
	return true;
}

bool CHtmlUI::EnableHud( bool enable )
{
	if ( !m_bInitialized )
		return false;

	if ( enable && !m_Hud.View() )
	{
		char url[256];
		V_sprintf_safe( url, "%s/ui/hud.html", htmlhost::kOrigin );
		if ( !m_Hud.Create( enginevguifuncs->GetPanel( PANEL_CLIENTDLL ), url ) )
		{
			Warning( "HTML GameUI: unable to create the HUD view.\n" );
			return false;
		}
		// Below the client viewport so native panels (crosshair, captions,
		// hints) keep drawing on top.
		m_Hud.Panel()->SetZPos( -10 );
	}

	m_bHudEnabled = enable;
	if ( m_Hud.Panel() )
		m_Hud.Panel()->SetVisible( enable );
	return true;
}

void CHtmlUI::SetHudState( const char *json )
{
	if ( !json )
		return;
	if ( m_HudState == json )
		return;
	m_HudState = json;
	m_bHudStateDirty = true;
}

void CHtmlUI::EmitHud( const char *event, const char *json )
{
	if ( m_Hud.View() )
		m_Hud.Emit( event, json );
}

void CHtmlUI::OnHudReady()
{
	m_Hud.SetReady();
	m_bHudStateDirty = !m_HudState.empty();
}

void CHtmlUI::Shutdown()
{
	if ( !m_bInitialized )
		return;
	m_bInitialized = false;

	m_Hud.Destroy();
	m_Menu.Destroy();
	if ( g_pLoadFreezePanel )
	{
		g_pLoadFreezePanel->MarkForDeletion();
		g_pLoadFreezePanel = nullptr;
	}

	// Answer anything still queued so CEF does not wait on us.
	{
		std::lock_guard lock( m_RequestMutex );
		for ( auto &r : m_Requests )
			htmlhost::CompleteRequest( r.id, 503, "text/plain", "", 0 );
		m_Requests.clear();
	}

	htmlhost::Shutdown();
}

void CHtmlUI::ScheduleFrames( bool force )
{
	const double now = Plat_FloatTime();
	const float maxFps = htmlui_maxfps.GetFloat();
	if ( !force && maxFps > 0 && now - m_flLastBeginFrame < 1.0 / maxFps )
		return;
	m_flLastBeginFrame = now;
	++m_nStatBeginFrames;

	m_Menu.BeginFrame();
	if ( m_bHudEnabled )
		m_Hud.BeginFrame();
}

void CHtmlUI::RunFrame()
{
	if ( !m_bInitialized )
		return;

	++g_nPaintFrame;

	m_Menu.UpdateVisibility( m_bLoading );
	if ( m_Hud.View() )
		m_Hud.UpdateVisibility();

	CheckEscape();
	Pump();
	ScheduleFrames( false );
	UpdateStats();
}

void CHtmlUI::UpdateStats()
{
	++m_nStatFrames;
	const double now = Plat_FloatTime();
	if ( m_flStatsStart == 0 )
	{
		m_flStatsStart = now;
		m_nStatMenuPaints = m_Menu.View() ? m_Menu.View()->PaintCount() : 0;
		m_nStatHudPaints = m_Hud.View() ? m_Hud.View()->PaintCount() : 0;
		return;
	}
	const double dt = now - m_flStatsStart;
	if ( dt < 1.0 )
		return;
	const uint64_t menuPaints = m_Menu.View() ? m_Menu.View()->PaintCount() : 0;
	const uint64_t hudPaints = m_Hud.View() ? m_Hud.View()->PaintCount() : 0;
	m_Stats.engineFps = static_cast<float>( m_nStatFrames / dt );
	m_Stats.beginFramesPerSec = static_cast<float>( m_nStatBeginFrames / dt );
	m_Stats.menuPaintsPerSec = static_cast<float>( ( menuPaints - m_nStatMenuPaints ) / dt );
	m_Stats.hudPaintsPerSec = static_cast<float>( ( hudPaints - m_nStatHudPaints ) / dt );
	m_flStatsStart = now;
	m_nStatFrames = m_nStatBeginFrames = 0;
	m_nStatMenuPaints = menuPaints;
	m_nStatHudPaints = hudPaints;
}

void CHtmlUI::Pump()
{
	if ( !m_bInitialized )
		return;

	ProcessRequests();

	if ( m_bHudStateDirty && m_Hud.IsReady() )
	{
		m_bHudStateDirty = false;
		m_Hud.Emit( "hud.state", m_HudState.c_str() );
	}

	m_Menu.Flush();
	m_Hud.Flush();
	FlushConsole();

	// While loading, RunFrame is not called: keep the loading screen alive.
	if ( m_bLoading )
	{
		++g_nPaintFrame;
		m_Menu.UpdateVisibility( true );
		ScheduleFrames( false );
	}
}

void CHtmlUI::CheckEscape()
{
	CHtmlPanel *panel = m_Menu.Panel();
	if ( !g_pInputSystem || !panel || !panel->IsOnScreen() )
	{
		m_bEscapeWasDown = false;
		return;
	}

	const bool down = g_pInputSystem->IsButtonDown( KEY_ESCAPE );
	if ( down && !m_bEscapeWasDown )
	{
		Emit( "escape" );
	}
	m_bEscapeWasDown = down;
}

void CHtmlUI::SetVisible( bool visible )
{
	CHtmlPanel *panel = m_Menu.Panel();
	if ( panel && visible )
	{
		panel->MoveToFront();
		panel->RequestFocus();
	}
}

void CHtmlUI::SetPreventEscape( bool prevent )
{
	if ( m_bPreventEscape == prevent )
		return;
	m_bPreventEscape = prevent;
	engine->ClientCmd_Unrestricted( prevent ? "gameui_preventescape\n" : "gameui_allowescape\n" );
}

void CHtmlUI::ForceAllowEscape()
{
	// Executed immediately (not buffered) so it takes effect even while the
	// main thread is busy loading a level.
	m_bPreventEscape = false;
	engine->ExecuteClientCmd( "gameui_allowescape" );
}

void CHtmlUI::Reload()
{
	m_Menu.RequestReload();
	m_Hud.RequestReload();
}

void CHtmlUI::ShowDevTools( bool hud )
{
	htmlhost::View *view = hud ? m_Hud.View() : m_Menu.View();
	if ( view )
		view->ShowDevTools();
}

void CHtmlUI::QueueConsoleText( const char *text, int r, int g, int b, int a, bool developer )
{
	if ( !text || !*text )
		return;
	std::lock_guard lock( m_ConsoleMutex );
	// Bound the backlog in case the page is not loaded yet.
	if ( m_ConsoleLines.size() > 4000 )
		m_ConsoleLines.erase( m_ConsoleLines.begin(), m_ConsoleLines.begin() + 1000 );
	m_ConsoleLines.push_back( { text, r, g, b, a, developer } );
}

void CHtmlUI::FlushConsole()
{
	if ( !m_Menu.IsReady() )
		return;

	std::vector<ConsoleLine> lines;
	{
		std::lock_guard lock( m_ConsoleMutex );
		lines.swap( m_ConsoleLines );
	}
	if ( lines.empty() )
		return;

	json::Writer w;
	w.BeginArray();
	for ( auto &l : lines )
	{
		w.BeginObject();
		w.Prop( "text", l.text );
		w.Key( "color" ).BeginArray().Int( l.r ).Int( l.g ).Int( l.b ).Int( l.a ).EndArray();
		if ( l.developer ) w.Prop( "dev", true );
		w.EndObject();
	}
	w.EndArray();
	Emit( "console.print", w.str().c_str() );
}

//-----------------------------------------------------------------------------
// Requests
//-----------------------------------------------------------------------------
void CHtmlUI::OnRequest( const htmlhost::Request &req )
{
	// API calls need the engine and run on the main thread; everything else is
	// static content which the (thread-safe) filesystem can serve right here so
	// that the page can load even while the main thread is busy.
	if ( V_strncmp( req.path, "/api/", 5 ) == 0 )
	{
		PendingRequest p;
		p.id = req.id;
		p.method = req.method;
		p.path = req.path + 5;
		p.query = req.query;
		if ( req.body && req.body_size )
			p.body.assign( static_cast<const char *>( req.body ), req.body_size );

		std::lock_guard lock( m_RequestMutex );
		m_Requests.push_back( std::move( p ) );
		return;
	}

	ServeAsset( req );
}

void CHtmlUI::ServeAsset( const htmlhost::Request &req )
{
	CUtlBuffer data;
	const char *mime = "application/octet-stream";
	const int status = UiAssets_Serve( req.path, req.query, data, mime );
	htmlhost::CompleteRequest( req.id, status, mime, data.Base(), static_cast<size_t>( data.TellPut() ) );
}

void CHtmlUI::ProcessRequests()
{
	std::deque<PendingRequest> requests;
	{
		std::lock_guard lock( m_RequestMutex );
		requests.swap( m_Requests );
	}

	for ( auto &r : requests )
	{
		json::Value args;
		if ( !r.body.empty() && !json::Parse( r.body.data(), r.body.size(), args ) )
		{
			const char err[] = "{\"ok\":false,\"error\":\"invalid JSON body\"}";
			htmlhost::CompleteRequest( r.id, 400, "application/json", err, sizeof( err ) - 1 );
			continue;
		}

		json::Writer result;
		std::string error;
		const bool ok = UiApi_Dispatch( r.path.c_str(), args, result, error );

		std::string body;
		if ( ok )
		{
			body = "{\"ok\":true,\"result\":";
			body += result.str().empty() ? "null" : result.str();
			body += "}";
		}
		else
		{
			body = "{\"ok\":false,\"error\":";
			body += json::Quote( error.c_str() );
			body += "}";
		}
		htmlhost::CompleteRequest( r.id, ok ? 200 : 400, "application/json", body.data(), body.size() );
	}
}
