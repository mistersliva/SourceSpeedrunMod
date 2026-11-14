// Copyright Source Authors. All rights reserved.
//
// Interface exported by gameui.dll that lets the client DLL drive the HTML
// HUD (htmlui/hud.html). The client gathers HUD data every frame and pushes
// JSON snapshots / events; gameui renders them with Chromium.

#ifndef SE_COMMON_GAMEUI_IHTMLHUD_H_
#define SE_COMMON_GAMEUI_IHTMLHUD_H_
#ifdef _WIN32
#pragma once
#endif

#include "tier1/interface.h"

abstract_class IHtmlHud : public IBaseInterface
{
public:
	// Creates / shows (or hides) the HUD page. Returns false when the HTML
	// HUD is unavailable (e.g. CEF failed to start).
	virtual bool SetEnabled( bool enabled ) = 0;
	virtual bool IsEnabled() = 0;

	// Replaces the HUD state. |json| is a JSON object; only send on change.
	virtual void SetState( const char *json ) = 0;

	// Fires a one-shot HUD event (e.g. "damage", "pickup"). |json| may be null.
	virtual void Event( const char *name, const char *json ) = 0;
};

#define HTMLHUD_INTERFACE_VERSION "HtmlHud001"

#endif  // SE_COMMON_GAMEUI_IHTMLHUD_H_
