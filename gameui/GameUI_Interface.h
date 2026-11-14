// Copyright Source Authors. All rights reserved.
//
// IGameUI implementation for the HTML GameUI. All menus, dialogs, the loading
// screen and the console are HTML/CSS/JS pages rendered by CEF; this class only
// forwards engine notifications to the page and services its requests.

#ifndef GAMEUI_INTERFACE_H
#define GAMEUI_INTERFACE_H
#pragma once

#include <string>

#include "GameUI/IGameUI.h"

class IGameClientExports;

class CGameUI : public IGameUI
{
public:
	CGameUI();
	~CGameUI();

	// IGameUI
	void Initialize( CreateInterfaceFn appFactory ) override;
	void PostInit() override;
	void Connect( CreateInterfaceFn gameFactory ) override;
	void Start() override;
	void Shutdown() override;
	void RunFrame() override;

	void OnGameUIActivated() override;
	void OnGameUIHidden() override;

	void OLD_OnConnectToServer( const char *game, int IP, int port ) override;
	void OnDisconnectFromServer_OLD( uint8 eSteamLoginFailure, const char *username ) override;
	void OnLevelLoadingStarted( bool bShowProgressDialog ) override;
	void OnLevelLoadingFinished( bool bError, const char *failureReason, const char *extendedReason ) override;

	bool UpdateProgressBar( float progress, const char *statusText ) override;
	bool SetShowProgressText( bool show ) override;

	void ShowNewGameDialog( int chapter ) override;

	void SessionNotification( const int notification, const int param = 0 ) override;
	void SystemNotification( const int notification ) override;
	void ShowMessageDialog( const uint nType, vgui::Panel *pOwner ) override;
	void UpdatePlayerInfo( uint64 nPlayerId, const char *pName, int nTeam, byte cVoiceState, int nPlayersNeeded, bool bHost ) override;
	void SessionSearchResult( int searchIdx, void *pHostData, XSESSION_SEARCHRESULT *pResult, int ping ) override;
	void OnCreditsFinished() override;

	void SetLoadingBackgroundDialog( vgui::VPANEL panel ) override;

	void BonusMapUnlock( const char *pchFileName = nullptr, const char *pchMapName = nullptr ) override;
	void BonusMapComplete( const char *pchFileName = nullptr, const char *pchMapName = nullptr ) override;
	void BonusMapChallengeUpdate( const char *pchFileName, const char *pchMapName, const char *pchChallengeName, int iBest ) override;
	void BonusMapChallengeNames( char *pchFileName, char *pchMapName, char *pchChallengeName ) override;
	void BonusMapChallengeObjectives( int &iBronze, int &iSilver, int &iGold ) override;
	void BonusMapDatabaseSave() override;
	int BonusMapNumAdvancedCompleted() override;
	void BonusMapNumMedals( int piNumMedals[ 3 ] ) override;

	void OnConnectToServer2( const char *game, int IP, int connectionPort, int queryPort ) override;

	bool ValidateStorageDevice( int *pStorageDeviceValidated ) override;
	void SetProgressOnStart() override;
	void OnDisconnectFromServer( uint8 eSteamLoginFailure ) override;
	void OnConfirmQuit() override;
	bool IsMainMenuVisible() override;
	void SetMainMenuOverride( vgui::VPANEL panel ) override;
	void SendMainMenuCommand( const char *pszCommand ) override;

	void BonusMapChallengeNames( OUT_Z_CAP(fileSize) char *pchFileName, intp fileSize,
		OUT_Z_CAP(mapSize) char *pchMapName, intp mapSize,
		OUT_Z_CAP(challengeSize) char *pchChallengeName, intp challengeSize ) override;

	// State helpers used by the API.
	bool IsInLevel();
	bool IsInBackgroundLevel();
	bool IsInMultiplayer();
	bool HasSavedThisMenuSession() const { return m_bHasSavedThisMenuSession; }
	void SetSavedThisMenuSession( bool bState ) { m_bHasSavedThisMenuSession = bState; }
	bool IsActive() const { return m_bActivatedUI; }

	// Emits the current state (in level, paused, ...) to the page.
	void PushState();

private:
	void PlayGameStartupSound();

	CreateInterfaceFn m_GameFactory;

	bool m_bPlayGameStartupSound;
	bool m_bActivatedUI;
	bool m_bHasSavedThisMenuSession;
	bool m_bLoading;
	// After a load: keep the UI (showing the last game frame) up until the
	// client is in game and draws the world, then hide it.
	bool m_bHideAfterLoad = false;
	int m_nHideAfterLoadFrames = 0;
	double m_flHideAfterLoadDeadline = 0;
	bool m_bShowProgressText;

	float m_flLastProgress;
	std::string m_LastStatusText;
	std::string m_LastStateJson;
	double m_flNextStatePoll;
};

extern CGameUI &GameUI();

extern IGameClientExports *GameClientExports();

#endif // GAMEUI_INTERFACE_H
