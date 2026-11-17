//-----------------------------------------------------------------------------
// SourceSpeedrunMod ghost race: other players' positions arrive through the
// HUD page (js/race.js, WebSocket to the lobby server) as console commands,
//   ssm_ghost <id> <map> <x> <y> <z> <yaw> <speed> <duck> [name...]
//   ssm_ghost_remove <id>      ssm_ghost_clear
// and are drawn as translucent client-side models, interpolated between
// updates. Only ghosts on the map the local player is on are shown.
// (Included by hud_htmlbridge.cpp.)
//-----------------------------------------------------------------------------
#include "c_baseanimating.h"
#include "debugoverlay_shared.h"
#include <map>

static ConVar ssm_ghosts( "ssm_ghosts", "1", FCVAR_ARCHIVE, "Show other racers as ghosts." );
static ConVar ssm_ghost_alpha( "ssm_ghost_alpha", "110", FCVAR_ARCHIVE, "Ghost opacity (0-255).", true, 0, true, 255 );
static ConVar ssm_ghost_names( "ssm_ghost_names", "1", FCVAR_ARCHIVE, "Show names above ghosts." );
static ConVar ssm_ghost_model( "ssm_ghost_model", "models/humans/group03/male_07.mdl", FCVAR_ARCHIVE, "Model used for ghosts." );

namespace
{
	struct Ghost_t
	{
		std::string name;
		std::string map;
		Vector prevPos = vec3_origin, nextPos = vec3_origin;
		float prevYaw = 0, nextYaw = 0;
		float recvTime = 0;	// gpGlobals->realtime of the last update
		float interval = 0.1f;
		int speed = 0;
		bool duck = false;
		CHandle<C_BaseAnimating> model;
	};

	std::map<std::string, Ghost_t> g_Ghosts;

	void DestroyModel( Ghost_t &g )
	{
		if ( C_BaseAnimating *p = g.model.Get() )
			p->Release();
		g.model = nullptr;
	}

	C_BaseAnimating *EnsureModel( Ghost_t &g )
	{
		if ( C_BaseAnimating *p = g.model.Get() )
			return p;
		const char *mdl = ssm_ghost_model.GetString();
		if ( !mdl[0] )
			return nullptr;
		// The server never precaches this model: register it client-side.
		if ( modelinfo->GetModelIndex( mdl ) == -1 && modelinfo->RegisterDynamicModel( mdl, true ) == -1 )
			return nullptr;
		C_BaseAnimating *p = new C_BaseAnimating;
		if ( !p->InitializeAsClientEntity( mdl, RENDER_GROUP_TRANSLUCENT_ENTITY ) )
		{
			p->Release();
			return nullptr;
		}
		p->SetRenderMode( kRenderTransAlpha );
		p->SetRenderColor( 160, 210, 255 );
		p->SetRenderColorA( static_cast<byte>( ssm_ghost_alpha.GetInt() ) );
		p->SetSolid( SOLID_NONE );
		p->SetCollisionGroup( COLLISION_GROUP_DEBRIS );
		p->AddEffects( EF_NOSHADOW | EF_NORECEIVESHADOW );
		p->SetAbsOrigin( g.nextPos );
		p->UpdatePartitionListEntry();
		p->UpdateVisibility();
		g.model = p;
		return p;
	}

	float LerpYaw( float a, float b, float t )
	{
		float d = AngleNormalize( b - a );
		return a + d * t;
	}
}

CON_COMMAND_F( ssm_ghost, "Ghost race: position of another player (sent by the HUD page).", FCVAR_HIDDEN | FCVAR_DONTRECORD )
{
	if ( args.ArgC() < 9 )
		return;
	Ghost_t &g = g_Ghosts[args[1]];
	const Vector pos( Q_atof( args[3] ), Q_atof( args[4] ), Q_atof( args[5] ) );
	const float yaw = Q_atof( args[6] );
	const bool mapChanged = g.map != args[2];
	const float now = gpGlobals->realtime;
	if ( mapChanged || g.recvTime <= 0 || ( pos - g.nextPos ).LengthSqr() > 512.0f * 512.0f )
	{
		g.prevPos = g.nextPos = pos;	// teleport (load, level change)
		g.prevYaw = g.nextYaw = yaw;
	}
	else
	{
		g.prevPos = g.nextPos;
		g.prevYaw = g.nextYaw;
		g.nextPos = pos;
		g.nextYaw = yaw;
		g.interval = clamp( now - g.recvTime, 0.03f, 0.5f );
	}
	g.recvTime = now;
	g.map = args[2];
	g.speed = Q_atoi( args[7] );
	g.duck = Q_atoi( args[8] ) != 0;
	if ( args.ArgC() > 9 )
	{
		const char *name = args.ArgS();
		// Skip the first eight arguments to keep spaces in the name.
		for ( int i = 0; i < 8 && name; ++i )
		{
			name = strchr( name, ' ' );
			if ( name )
				++name;
		}
		if ( name )
			g.name = name;
	}
}

CON_COMMAND_F( ssm_ghost_remove, "Ghost race: a player left.", FCVAR_HIDDEN | FCVAR_DONTRECORD )
{
	if ( args.ArgC() < 2 )
		return;
	auto it = g_Ghosts.find( args[1] );
	if ( it == g_Ghosts.end() )
		return;
	DestroyModel( it->second );
	g_Ghosts.erase( it );
}

CON_COMMAND( ssm_ghost_list, "Ghost race: list the ghosts." )
{
	for ( auto &[id, g] : g_Ghosts )
	{
		C_BaseAnimating *p = g.model.Get();
		Msg( "%s '%s' map=%s pos=%.0f %.0f %.0f age=%.1fs model=%s drawn=%d\n", id.c_str(), g.name.c_str(), g.map.c_str(),
			g.nextPos.x, g.nextPos.y, g.nextPos.z, gpGlobals->realtime - g.recvTime, p ? "yes" : "no", p ? (int)!p->IsDormant() : 0 );
	}
}

CON_COMMAND_F( ssm_ghost_clear, "Ghost race: remove all ghosts.", FCVAR_HIDDEN | FCVAR_DONTRECORD )
{
	for ( auto &[id, g] : g_Ghosts )
		DestroyModel( g );
	g_Ghosts.clear();
}

class CSSMGhosts : public CAutoGameSystemPerFrame
{
public:
	CSSMGhosts() : CAutoGameSystemPerFrame( "CSSMGhosts" ) {}

	void LevelInitPreEntity() override
	{
		PrecacheModelForGhosts();
	}

	void LevelShutdownPreEntity() override
	{
		// Client entities die with the level; recreate them on the next one.
		for ( auto &[id, g] : g_Ghosts )
			DestroyModel( g );
	}

	void PrecacheModelForGhosts()
	{
		const char *mdl = ssm_ghost_model.GetString();
		if ( mdl[0] )
			modelinfo->FindOrLoadModel( mdl );
	}

	void Update( float ) override
	{
		if ( !engine->IsInGame() || engine->IsPlayingDemo() )
			return;
		char map[MAX_PATH];
		V_FileBase( engine->GetLevelName(), map, sizeof( map ) );
		const float now = gpGlobals->realtime;
		const byte alpha = static_cast<byte>( ssm_ghost_alpha.GetInt() );

		for ( auto it = g_Ghosts.begin(); it != g_Ghosts.end(); )
		{
			Ghost_t &g = it->second;
			// No news for 10 s: the player is gone.
			if ( now - g.recvTime > 10.0f || now < g.recvTime - 1.0f )
			{
				DestroyModel( g );
				it = g_Ghosts.erase( it );
				continue;
			}
			const bool visible = ssm_ghosts.GetBool() && V_stricmp( g.map.c_str(), map ) == 0;
			if ( !visible )
			{
				DestroyModel( g );
				++it;
				continue;
			}
			C_BaseAnimating *p = EnsureModel( g );
			if ( !p )
			{
				++it;
				continue;
			}
			const float t = clamp( ( now - g.recvTime ) / g.interval, 0.0f, 1.0f );
			Vector pos;
			VectorLerp( g.prevPos, g.nextPos, t, pos );
			p->SetAbsOrigin( pos );
			p->SetAbsAngles( QAngle( 0, LerpYaw( g.prevYaw, g.nextYaw, t ), 0 ) );
			p->UpdatePartitionListEntry();
			p->SetRenderColorA( alpha );

			// Run animation while moving, idle when standing.
			const char *seq = g.duck ? ( g.speed > 20 ? "crouchRUNALL1" : "crouchidle" ) : ( g.speed > 150 ? "run_all" : g.speed > 20 ? "walk_all" : "idle_subtle" );
			int s = p->LookupSequence( seq );
			if ( s < 0 )
				s = p->LookupSequence( "idle_subtle" );
			if ( s >= 0 && p->GetSequence() != s )
			{
				p->SetSequence( s );
				p->SetCycle( 0 );
			}
			p->SetPlaybackRate( 1.0f );
			p->SetCycle( fmodf( p->GetCycle() + gpGlobals->frametime * p->GetSequenceCycleRate( p->GetModelPtr(), p->GetSequence() ), 1.0f ) );

			if ( ssm_ghost_names.GetBool() && !g.name.empty() && debugoverlay )
				debugoverlay->AddTextOverlayRGB( pos + Vector( 0, 0, g.duck ? 50 : 80 ), 0, 0.0f, 0.78f, 0.9f, 1.0f, 1.0f, "%s", g.name.c_str() );
			++it;
		}
	}

	void Shutdown() override
	{
		for ( auto &[id, g] : g_Ghosts )
			DestroyModel( g );
		g_Ghosts.clear();
	}
};

static CSSMGhosts g_SSMGhosts;
