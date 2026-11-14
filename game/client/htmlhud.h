// Copyright Source Authors. All rights reserved.
//
// Hooks between the native vgui HUD and the HTML HUD bridge
// (hl2/hud_htmlbridge.cpp). Shared HUD code calls through these pointers so
// games that do not compile the bridge are unaffected.

#ifndef SE_GAME_CLIENT_HTMLHUD_H_
#define SE_GAME_CLIENT_HTMLHUD_H_
#ifdef _WIN32
#pragma once
#endif

class C_BaseCombatWeapon;
class CHudTexture;
class Vector;

struct HtmlHudHooks_t
{
	// Returns true when the HTML HUD draws this element instead (the native
	// element keeps its logic but does not paint).
	bool ( *SuppressNative )( const char *pszElementName ) = nullptr;

	// Pickup history (CHudHistoryResource::AddIconToHistory).
	void ( *OnPickup )( int iType, int iId, C_BaseCombatWeapon *pWeapon, int iCount, CHudTexture *pIcon ) = nullptr;

	// Damage taken ("Damage" user message).
	void ( *OnDamage )( int iArmor, int iDamageTaken, long bitsDamage, const Vector &vecFrom ) = nullptr;

	// Suit armor as last reported by the "Battery" user message.
	int iArmor = 0;
};

extern HtmlHudHooks_t g_HtmlHudHooks;

#endif  // SE_GAME_CLIENT_HTMLHUD_H_
