// Copyright Source Authors. All rights reserved.
//
// HTML GameUI host: owns the off-screen Chromium views (menu + HUD), the vgui
// panels that present them, and the JS <-> engine bridge.
//
// JS -> engine:  fetch("https://gameui.local/api/<method>", {method:"POST", body:JSON})
//                Handled on the main thread in RunFrame() (see UiApi.cpp).
// engine -> JS:  window.__sourceEvent(name, payload) via Emit() / EmitHud().
// Assets:        https://gameui.local/ui/...   -> htmlui/... (GAME, then PLATFORM)
//                https://gameui.local/game/... -> raw game files (GAME)
//                https://gameui.local/img/...  -> VTF/TGA converted to PNG
//
// Frame pacing: views use external begin-frame scheduling. The engine asks
// Chromium for a frame once per game frame (optionally capped by
// htmlui_maxfps), so the UI runs at the game's frame rate. Only the region
// that changed is converted and uploaded to the GPU.

#ifndef SE_GAMEUI_HTMLUI_H_
#define SE_GAMEUI_HTMLUI_H_

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "vgui_controls/Panel.h"
#include "cefbridge/htmlhost.h"

//-----------------------------------------------------------------------------
// Fullscreen vgui panel that paints a browser frame and (optionally)
// forwards input to it.
//-----------------------------------------------------------------------------
class CHtmlPanel : public vgui::Panel
{
	DECLARE_CLASS_SIMPLE_OVERRIDE( CHtmlPanel, vgui::Panel );

public:
	CHtmlPanel( vgui::VPANEL parent, const char *name, bool interactive );
	~CHtmlPanel() override;

	void SetView( htmlhost::View *view ) { m_pView = view; }
	htmlhost::View *View() const { return m_pView; }

	void Paint() override;
	void OnThink() override;
	void OnSizeChanged( int wide, int tall ) override;

	void OnCursorMoved( int x, int y ) override;
	void OnCursorExited() override;
	void OnMousePressed( vgui::MouseCode code ) override;
	void OnMouseDoublePressed( vgui::MouseCode code ) override;
	void OnMouseReleased( vgui::MouseCode code ) override;
	void OnMouseWheeled( int delta ) override;
	void OnKeyCodePressed( vgui::KeyCode code ) override;
	void OnKeyCodeTyped( vgui::KeyCode code ) override;
	void OnKeyCodeReleased( vgui::KeyCode code ) override;
	void OnKeyTyped( wchar_t unichar ) override;
	void OnSetFocus() override;
	void OnKillFocus() override;

	void SetBrowserCursor( htmlhost::IViewListener::Cursor cursor );

	// True when the panel is actually on screen (all parents visible).
	bool IsOnScreen() const;
	// Called when the panel becomes visible again: do not present the stale
	// frame from before it was hidden until Chromium repainted.
	void OnShown();

	uint64_t UploadedSerial() const { return m_nUploadedSerial; }

	// Screen rectangles (x, y, w, h) behind which the game is drawn blurred
	// ("glass" panels). Reported by the page.
	struct GlassRect { int x, y, w, h; float radius; float alpha; };
	void SetGlassRects( std::vector<GlassRect> rects ) { m_GlassRects = std::move( rects ); }

	// Screen-edge colour flash (damage), drawn by the engine: a full-screen
	// animated overlay in the page would re-upload the whole frame every frame.
	void Flash( int r, int g, int b, float alpha, float seconds );

private:
	uint32_t Modifiers() const;
	void ReleaseTexture();

	htmlhost::View *m_pView;
	bool m_bInteractive;
	int m_nTextureId;
	int m_nTextureWide;
	int m_nTextureTall;
	uint64_t m_nUploadedSerial;
	uint64_t m_nSerialAtShow;
	double m_flShownTime;
	int m_nLastX, m_nLastY;
	bool m_bMouseOutside = false;
	std::atomic<int> m_nPendingCursor;
	std::vector<GlassRect> m_GlassRects;
	struct FlashState { int r = 0, g = 0, b = 0; float alpha = 0; double start = 0; float duration = 0; } m_Flash;
};

//-----------------------------------------------------------------------------
// One browser page (view + panel + event queue).
//-----------------------------------------------------------------------------
class CHtmlSurface : public htmlhost::IViewListener
{
public:
	CHtmlSurface( const char *name, bool interactive );

	bool Create( vgui::VPANEL parent, const char *url );
	void Destroy();

	// Main thread.
	void Emit( const char *event, const char *jsonPayload );
	void Flush();
	void SetReady() { m_bReady = true; Flush(); }
	bool IsReady() const { return m_bReady; }
	void RequestReload() { m_bNeedsReload = true; }
	// Shows / hides the page; hidden pages do not render.
	void UpdateVisibility( bool forceVisible = false );
	void BeginFrame();
	bool HasNewFrame() const;

	htmlhost::View *View() const { return m_pView; }
	CHtmlPanel *Panel() const { return m_pPanel; }
	const char *Name() const { return m_szName; }

	// IViewListener (CEF threads)
	void OnCursorChanged( Cursor cursor ) override;
	void OnConsoleMessage( int level, const char *message, const char *source, int line ) override;
	void OnLoadEnd( int httpStatus ) override;
	void OnLoadError( int errorCode, const char *errorText, const char *url ) override;
	void OnRendererCrashed() override;

private:
	char m_szName[32];
	bool m_bInteractive;
	CHtmlPanel *m_pPanel;
	htmlhost::View *m_pView;
	std::vector<std::string> m_PendingEvents;
	std::atomic<bool> m_bReady;
	std::atomic<bool> m_bNeedsReload;
	bool m_bWasOnScreen;
};

//-----------------------------------------------------------------------------
// The host.
//-----------------------------------------------------------------------------
class CHtmlUI : public htmlhost::IRequestHandler
{
public:
	CHtmlUI();

	// Main thread.
	bool Init( vgui::VPANEL menuParent );
	void Shutdown();
	void RunFrame();
	// Lightweight pump usable while the engine is blocked loading a level.
	void Pump();

	// Menu page (menus, dialogs, console, loading screen).
	CHtmlSurface &Menu() { return m_Menu; }
	// HUD page (created lazily when the client enables the HTML HUD).
	CHtmlSurface &Hud() { return m_Hud; }

	bool IsReady() const { return m_Menu.IsReady(); }
	htmlhost::View *View() const { return m_Menu.View(); }
	CHtmlPanel *Panel() const { return m_Menu.Panel(); }
	bool HasNewFrame() const { return m_Menu.HasNewFrame(); }

	// Sends an event to the menu page. |jsonPayload| must be valid JSON (or nullptr).
	void Emit( const char *event, const char *jsonPayload = nullptr ) { m_Menu.Emit( event, jsonPayload ); }

	// HUD control (from the client DLL through IHtmlHud).
	bool EnableHud( bool enable );
	bool IsHudEnabled() const { return m_bHudEnabled; }
	void SetHudState( const char *json );
	void EmitHud( const char *event, const char *json );

	// Console output (thread-safe), batched per frame.
	void QueueConsoleText( const char *text, int r, int g, int b, int a, bool developer );

	void SetVisible( bool visible );
	void SetLoading( bool loading ) { m_bLoading = loading; }
	bool IsLoading() const { return m_bLoading; }
	void SetHoldingFrame( bool hold ) { m_bHoldingFrame = hold; }
	bool IsHoldingFrame() const { return m_bHoldingFrame; }
	void SetPreventEscape( bool prevent );
	bool IsEscapePrevented() const { return m_bPreventEscape; }
	void ForceAllowEscape();

	void OnPageReady() { m_Menu.SetReady(); }
	void OnHudReady();
	void Reload();
	void ShowDevTools( bool hud = false );

	// IRequestHandler (CEF thread)
	void OnRequest( const htmlhost::Request &req ) override;

private:
	struct PendingRequest
	{
		htmlhost::RequestId id;
		std::string method;
		std::string path;
		std::string query;
		std::string body;
	};

	struct ConsoleLine
	{
		std::string text;
		int r, g, b, a;
		bool developer;
	};

	void ProcessRequests();
	void FlushConsole();
	void CheckEscape();
	void ScheduleFrames( bool force );

	void UpdateStats();

public:
	struct Stats
	{
		float engineFps = 0;
		float beginFramesPerSec = 0;
		float menuPaintsPerSec = 0;
		float hudPaintsPerSec = 0;
	};
	const Stats &GetStats() const { return m_Stats; }

private:
	Stats m_Stats;
	double m_flStatsStart = 0;
	uint64_t m_nStatFrames = 0;
	uint64_t m_nStatBeginFrames = 0;
	uint64_t m_nStatMenuPaints = 0;
	uint64_t m_nStatHudPaints = 0;
	void ServeAsset( const htmlhost::Request &req );

	CHtmlSurface m_Menu;
	CHtmlSurface m_Hud;

	std::mutex m_RequestMutex;
	std::deque<PendingRequest> m_Requests;

	std::mutex m_ConsoleMutex;
	std::vector<ConsoleLine> m_ConsoleLines;

	std::string m_HudState;
	bool m_bHudStateDirty;

	bool m_bInitialized;
	bool m_bHudEnabled;
	bool m_bLoading;
	bool m_bHoldingFrame = false;
	bool m_bPreventEscape;
	bool m_bEscapeWasDown;
	double m_flLastBeginFrame;
};

CHtmlUI &HtmlUI();

#endif  // SE_GAMEUI_HTMLUI_H_
