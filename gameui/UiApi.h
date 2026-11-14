// Copyright Source Authors. All rights reserved.
//
// Engine API exposed to the HTML GameUI. Every method is invoked on the main
// thread with JSON arguments and writes a JSON result.

#ifndef SE_GAMEUI_UIAPI_H_
#define SE_GAMEUI_UIAPI_H_

#include <string>

#include "Json.h"

// Calls API |method|. Returns false and sets |error| on failure.
bool UiApi_Dispatch( const char *method, const json::Value &args, json::Writer &result, std::string &error );

// Serializes the current game state (in level, paused, ...) as JSON.
std::string UiApi_StateJson();

#endif  // SE_GAMEUI_UIAPI_H_
