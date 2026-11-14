// Copyright Source Authors. All rights reserved.
//
// IGameConsole implementation backed by the HTML GameUI. Console output is
// captured through IConsoleDisplayFunc and streamed to the page.

#ifndef GAMECONSOLE_H
#define GAMECONSOLE_H
#ifdef _WIN32
#pragma once
#endif

#include <mutex>
#include <string>

#include "GameUI/IGameConsole.h"
#include "icvar.h"

class CGameConsole : public IGameConsole, public IConsoleDisplayFunc
{
public:
	CGameConsole();
	~CGameConsole() = default;

	// IGameConsole
	void Initialize() override;
	void Activate() override;
	void Hide() override;
	void Clear() override;
	bool IsConsoleVisible() override;
	void SetParent( vgui::VPANEL parent ) override;
	void Initialize( vgui::VPANEL parent, const char *panelModule ) override;
	void Shutdown() override;

	// IConsoleDisplayFunc
	void ColorPrint( const Color &clr, const char *pMessage ) override;
	void Print( const char *pMessage ) override;
	void DPrint( const char *pMessage ) override;

	// Writes the captured console text to condumpNNN.txt.
	void Dump();

private:
	void Append( const char *text, int r, int g, int b, int a, bool developer );

	bool m_bInitialized;
	bool m_bVisible;

	std::mutex m_HistoryMutex;
	std::string m_History;  // plain-text scrollback for condump
};

extern CGameConsole &GameConsole();

#endif // GAMECONSOLE_H
