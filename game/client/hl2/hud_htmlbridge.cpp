// Copyright Source Authors. All rights reserved.
//
// HTML HUD bridge for Half-Life 2. Every client frame it gathers the HUD state
// (health, suit, ammo, aux power, weapon selection, squad) and hands it to
// gameui.dll (IHtmlHud) as JSON, which renders htmlui/hud.html with Chromium.
// One-shot events (damage direction, pickups) are forwarded as they happen.
//
// The replaced native elements keep running (weapon selection input, timers)
// but stop painting, see CHud::Think and htmlhud.h.

#include "cbase.h"

#include <cmath>
#include <string>

#include "hud.h"
#include "hudelement.h"
#include "c_basehlplayer.h"
#include "weapon_selection.h"
#include "history_resource.h"
#include "ammodef.h"
#include "igamesystem.h"
#include "view.h"
#include "vgui/ILocalize.h"
#include "tier1/interface.h"
#include "GameUI/IHtmlHud.h"
#include "htmlhud.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar cl_htmlhud( "cl_htmlhud", "1", FCVAR_ARCHIVE, "Draw the HUD with the HTML (Chromium) renderer instead of vgui." );

namespace
{

// Native elements drawn by hud.html instead.
const char *const kReplacedElements[] = {
	"CHudHealth",
	"CHudBattery",
	"CHudAmmo",
	"CHudSecondaryAmmo",
	"CHudSuitPower",
	"CHudWeaponSelection",
	"CHudDamageIndicator",
	"CHudSquadStatus",
	"CHudHistoryResource",
	"CHudPoisonDamageIndicator",
	"CHudFlashlight",
};

class JsonOut
{
public:
	JsonOut &Raw( const char *s ) { m_s += s; return *this; }
	JsonOut &Str( const char *s )
	{
		m_s += '"';
		for ( const unsigned char *p = reinterpret_cast<const unsigned char *>( s ? s : "" ); *p; ++p )
		{
			switch ( *p )
			{
				case '"': m_s += "\\\""; break;
				case '\\': m_s += "\\\\"; break;
				case '\n': m_s += "\\n"; break;
				case '\r': break;
				case '\t': m_s += "\\t"; break;
				default:
					if ( *p < 0x20 ) break;
					m_s += static_cast<char>( *p );
			}
		}
		m_s += '"';
		return *this;
	}
	JsonOut &Localized( const char *token )
	{
		if ( token && token[0] == '#' && g_pVGuiLocalize )
		{
			if ( const wchar_t *w = g_pVGuiLocalize->Find( token ) )
			{
				char utf8[512];
				V_UnicodeToUTF8( w, utf8, sizeof( utf8 ) );
				return Str( utf8 );
			}
		}
		return Str( token );
	}
	JsonOut &Int( int v ) { m_s += std::to_string( v ); return *this; }
	JsonOut &Num( float v )
	{
		char buf[32];
		V_sprintf_safe( buf, "%.3f", std::isfinite( v ) ? v : 0.0f );
		m_s += buf;
		return *this;
	}
	JsonOut &Bool( bool b ) { m_s += b ? "true" : "false"; return *this; }
	JsonOut &Key( const char *k ) { Sep(); Str( k ); m_s += ':'; return *this; }
	JsonOut &Begin( char c ) { Sep(); m_s += c; return *this; }
	JsonOut &End( char c ) { m_s += c; return *this; }
	// Separator handling: call before a value inside arrays.
	JsonOut &Item() { Sep(); return *this; }
	JsonOut &Icon( const CHudTexture *icon )
	{
		if ( !icon )
			return Raw( "null" );
		Raw( "{\"name\":" ).Str( icon->szShortName );
		if ( icon->bRenderUsingFont && icon->cCharacterInFont )
		{
			char ch[2] = { icon->cCharacterInFont, 0 };
			Raw( ",\"char\":" ).Str( ch ).Raw( ",\"font\":" ).Str( icon->szTextureFile );
		}
		return Raw( "}" );
	}
	const std::string &str() const { return m_s; }

private:
	void Sep()
	{
		if ( !m_s.empty() )
		{
			const char last = m_s.back();
			if ( last != '{' && last != '[' && last != ':' )
				m_s += ',';
		}
	}

	std::string m_s;
};

}  // namespace

//-----------------------------------------------------------------------------
// The bridge
//-----------------------------------------------------------------------------
class CHtmlHudBridge : public CAutoGameSystemPerFrame
{
public:
	CHtmlHudBridge() : CAutoGameSystemPerFrame( "CHtmlHudBridge" ) {}

	bool Init() override
	{
		g_HtmlHudHooks.SuppressNative = &CHtmlHudBridge::SuppressNative;
		g_HtmlHudHooks.OnPickup = &CHtmlHudBridge::OnPickup;
		g_HtmlHudHooks.OnDamage = &CHtmlHudBridge::OnDamage;
		return true;
	}

	void Shutdown() override
	{
		g_HtmlHudHooks.SuppressNative = nullptr;
		g_HtmlHudHooks.OnPickup = nullptr;
		g_HtmlHudHooks.OnDamage = nullptr;
		if ( m_pHud && m_bEnabled )
			m_pHud->SetEnabled( false );
		m_pHud = nullptr;
		m_bEnabled = false;
	}

	void LevelShutdownPreEntity() override
	{
		SetEnabled( false );
	}

	void Update( float ) override
	{
		C_BaseHLPlayer *player = dynamic_cast<C_BaseHLPlayer *>( C_BasePlayer::GetLocalPlayer() );
		const bool want = cl_htmlhud.GetBool() && player && engine->IsInGame() && !engine->IsLevelMainMenuBackground();
		SetEnabled( want );
		if ( !m_bEnabled || !player )
			return;

		// Announce movement mode switches.
		static ConVarRef s_MovementMode( "sv_movement_mode" );
		const int mode = s_MovementMode.IsValid() ? s_MovementMode.GetInt() : 0;
		if ( mode != m_nLastMovementMode )
		{
			if ( m_nLastMovementMode >= 0 )
			{
				char json[32];
				V_sprintf_safe( json, "{\"mode\":%d}", mode );
				m_pHud->Event( "movement", json );
			}
			m_nLastMovementMode = mode;
		}

		const std::string state = BuildState( player );
		if ( state != m_LastState )
		{
			m_LastState = state;
			m_pHud->SetState( state.c_str() );
		}
	}

	static CHtmlHudBridge &Get();

private:
	IHtmlHud *Hud()
	{
		if ( !m_pHud && !m_bLookedUp )
		{
			m_bLookedUp = true;
			static CDllDemandLoader s_GameUI( "gameui" );
			if ( CreateInterfaceFn factory = s_GameUI.GetFactory() )
				m_pHud = static_cast<IHtmlHud *>( factory( HTMLHUD_INTERFACE_VERSION, nullptr ) );
			if ( !m_pHud )
				DevWarning( "HTML HUD: gameui does not export %s, using the vgui HUD.\n", HTMLHUD_INTERFACE_VERSION );
		}
		return m_pHud;
	}

	void SetEnabled( bool enable )
	{
		if ( enable == m_bEnabled )
			return;
		IHtmlHud *hud = Hud();
		if ( !hud )
		{
			m_bEnabled = false;
			return;
		}
		m_bEnabled = hud->SetEnabled( enable ) && enable;
		m_LastState.clear();
	}

	static bool SuppressNative( const char *name )
	{
		if ( !Get().m_bEnabled || !name )
			return false;
		for ( const char *e : kReplacedElements )
		{
			if ( V_strcmp( e, name ) == 0 )
				return true;
		}
		return false;
	}

	static void OnPickup( int type, int id, C_BaseCombatWeapon *weapon, int count, CHudTexture *icon )
	{
		CHtmlHudBridge &self = Get();
		if ( !self.m_bEnabled )
			return;

		JsonOut j;
		j.Begin( '{' );
		switch ( type )
		{
			case HISTSLOT_WEAP:
				j.Key( "type" ).Str( "weapon" );
				j.Key( "label" ).Localized( weapon ? weapon->GetPrintName() : "" );
				j.Key( "icon" ).Icon( weapon ? weapon->GetSpriteInactive() : nullptr );
				break;
			case HISTSLOT_AMMO:
			case HISTSLOT_AMMODENIED:
			{
				j.Key( "type" ).Str( type == HISTSLOT_AMMO ? "ammo" : "denied" );
				const Ammo_t *ammo = GetAmmoDef()->GetAmmoOfIndex( id );
				j.Key( "label" ).Str( ammo ? ammo->pName : "" );
				j.Key( "icon" ).Icon( gWR.GetAmmoIconFromWeapon( id ) );
				break;
			}
			default:
				j.Key( "type" ).Str( "item" );
				j.Key( "label" ).Str( icon ? icon->szShortName : "" );
				j.Key( "icon" ).Icon( icon );
				break;
		}
		j.Key( "count" ).Int( count );
		j.End( '}' );
		self.m_pHud->Event( "pickup", j.str().c_str() );
	}

	static void OnDamage( int armor, int damage, long bits, const Vector &from )
	{
		CHtmlHudBridge &self = Get();
		if ( !self.m_bEnabled )
			return;

		JsonOut j;
		j.Begin( '{' );
		j.Key( "damage" ).Int( damage );
		j.Key( "armor" ).Int( armor );
		j.Key( "drown" ).Bool( ( bits & DMG_DROWN ) != 0 );
		j.Key( "poison" ).Bool( ( bits & ( DMG_POISON | DMG_NERVEGAS ) ) != 0 );
		j.Key( "burn" ).Bool( ( bits & ( DMG_BURN | DMG_SLOWBURN ) ) != 0 );
		j.Key( "shock" ).Bool( ( bits & ( DMG_SHOCK | DMG_SONIC ) ) != 0 );

		j.Key( "angle" );
		if ( from == vec3_origin )
		{
			j.Raw( "null" );
		}
		else
		{
			// 0 = in front, +90 = right, 180 = behind.
			Vector delta = from - MainViewOrigin();
			delta.z = 0;
			VectorNormalize( delta );
			Vector forward, right;
			AngleVectors( MainViewAngles(), &forward, &right, nullptr );
			forward.z = right.z = 0;
			VectorNormalize( forward );
			VectorNormalize( right );
			j.Num( RAD2DEG( atan2f( DotProduct( delta, right ), DotProduct( delta, forward ) ) ) );
		}
		j.End( '}' );
		self.m_pHud->Event( "damage", j.str().c_str() );
	}

	static std::string BuildState( C_BaseHLPlayer *player )
	{
		JsonOut j;
		j.Begin( '{' );

		const bool alive = player->IsAlive();
		j.Key( "alive" ).Bool( alive );
		j.Key( "health" ).Int( MAX( player->GetHealth(), 0 ) );
		j.Key( "suit" ).Bool( player->IsSuitEquipped() );
		j.Key( "armor" ).Int( g_HtmlHudHooks.iArmor );

		j.Key( "hidden" ).Begin( '{' );
		j.Key( "all" ).Bool( gHUD.IsHidden( HIDEHUD_ALL ) );
		j.Key( "health" ).Bool( gHUD.IsHidden( HIDEHUD_HEALTH | HIDEHUD_PLAYERDEAD | HIDEHUD_NEEDSUIT ) );
		j.Key( "weapons" ).Bool( gHUD.IsHidden( HIDEHUD_WEAPONSELECTION | HIDEHUD_PLAYERDEAD ) );
		j.End( '}' );

		const C_HL2PlayerLocalData &local = player->m_HL2Local;
		j.Key( "aux" ).Begin( '{' );
		j.Key( "power" ).Num( local.m_flSuitPower );
		j.Key( "sprint" ).Bool( player->IsSprinting() );
		j.Key( "flashlight" ).Bool( player->IsFlashlightActive() );
		j.Key( "breather" ).Bool( player->IsBreatherActive() );
		j.End( '}' );

		j.Key( "squad" ).Begin( '{' );
		j.Key( "members" ).Int( local.m_iSquadMemberCount );
		j.Key( "medics" ).Int( local.m_iSquadMedicCount );
		j.Key( "following" ).Bool( local.m_fSquadInFollowMode );
		j.End( '}' );

		j.Key( "zoom" ).Bool( local.m_bZooming );
		j.Key( "vehicle" ).Bool( player->IsInAVehicle() );

		static ConVarRef s_MovementMode( "sv_movement_mode" );
		j.Key( "movement" ).Int( s_MovementMode.IsValid() ? s_MovementMode.GetInt() : 0 );
		static ConVarRef s_AutoBhop( "sv_autobunnyhopping" );
		j.Key( "autobhop" ).Bool( s_AutoBhop.IsValid() && s_AutoBhop.GetBool() );
		j.Key( "speed" ).Int( static_cast<int>( player->GetAbsVelocity().Length2D() + 0.5f ) );
		// Map time (stops while paused) for the HUD timer, in milliseconds.
		j.Key( "time" ).Int( static_cast<int>( gpGlobals->curtime * 1000.0f ) );
		// Current map (speedrun timer: game time across maps, auto split).
		char map[MAX_PATH];
		V_FileBase( engine->GetLevelName(), map, sizeof( map ) );
		j.Key( "map" ).Str( map );

		// Active weapon
		j.Key( "weapon" );
		C_BaseCombatWeapon *wpn = player->GetActiveWeapon();
		if ( wpn && !player->IsInAVehicle() )
		{
			j.Begin( '{' );
			j.Key( "class" ).Str( wpn->GetClassname() );
			j.Key( "label" ).Localized( wpn->GetPrintName() );
			j.Key( "icon" ).Icon( wpn->GetSpriteActive() );
			const bool usesPrimary = wpn->UsesPrimaryAmmo();
			j.Key( "usesAmmo" ).Bool( usesPrimary );
			if ( usesPrimary )
			{
				const bool clips = wpn->UsesClipsForAmmo1();
				j.Key( "clip" ).Int( clips ? wpn->Clip1() : -1 );
				j.Key( "maxClip" ).Int( clips ? wpn->GetMaxClip1() : -1 );
				j.Key( "reserve" ).Int( player->GetAmmoCount( wpn->GetPrimaryAmmoType() ) );
			}
			j.Key( "secondary" ).Int( wpn->UsesSecondaryAmmo() ? player->GetAmmoCount( wpn->GetSecondaryAmmoType() ) : -1 );
			j.End( '}' );
		}
		else
		{
			j.Raw( "null" );
		}

		// Weapon selection (only while open)
		CBaseHudWeaponSelection *sel = GetHudWeaponSelection();
		const bool selecting = sel && sel->IsInSelectionMode() && alive;
		j.Key( "selection" );
		if ( selecting )
		{
			C_BaseCombatWeapon *selected = sel->GetSelectedWeapon();
			j.Begin( '{' );
			j.Key( "slots" ).Begin( '[' );
			for ( int slot = 0; slot < MAX_WEAPON_SLOTS; ++slot )
			{
				j.Item().Begin( '[' );
				for ( int pos = 0; pos < MAX_WEAPON_POSITIONS; ++pos )
				{
					C_BaseCombatWeapon *w = nullptr;
					for ( int i = 0; i < MAX_WEAPONS; ++i )
					{
						C_BaseCombatWeapon *cand = player->GetWeapon( i );
						if ( cand && cand->GetSlot() == slot && cand->GetPosition() == pos && cand->VisibleInWeaponSelection() )
						{
							w = cand;
							break;
						}
					}
					if ( !w )
						continue;
					j.Item().Begin( '{' );
					j.Key( "label" ).Localized( w->GetPrintName() );
					j.Key( "icon" ).Icon( w->GetSpriteInactive() );
					j.Key( "selected" ).Bool( w == selected );
					j.Key( "empty" ).Bool( !w->CanBeSelected() );
					j.End( '}' );
				}
				j.End( ']' );
			}
			j.End( ']' );
			j.End( '}' );
		}
		else
		{
			j.Raw( "null" );
		}

		j.End( '}' );
		return j.str();
	}

	IHtmlHud *m_pHud = nullptr;
	bool m_bLookedUp = false;
	bool m_bEnabled = false;
	std::string m_LastState;
	int m_nLastMovementMode = -1;
};

static CHtmlHudBridge g_HtmlHudBridge;

CHtmlHudBridge &CHtmlHudBridge::Get()
{
	return g_HtmlHudBridge;
}
