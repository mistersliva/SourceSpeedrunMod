// Copyright Source Authors. All rights reserved.
//
// Static content for the HTML GameUI. Safe to call from CEF threads (only
// uses thread-safe filesystem reads).
//
//   /                    -> htmlui/index.html
//   /ui/<path>           -> htmlui/<path>      (GAME search path, then PLATFORM)
//   /game/<path>         -> <path>             (GAME search path)
//   /img/vtf/<material>  -> materials/<material>.vtf decoded to PNG
//   /img/tga/<path>      -> <path> (.tga, MOD search path) decoded to PNG

#ifndef SE_GAMEUI_UIASSETS_H_
#define SE_GAMEUI_UIASSETS_H_

#include "tier1/utlbuffer.h"

// Returns an HTTP status code; fills |out| and |mime|.
int UiAssets_Serve( const char *path, const char *query, CUtlBuffer &out, const char *&mime );

// Encodes RGBA8888 pixels as PNG into |out|.
bool UiAssets_EncodePNG( const unsigned char *rgba, int width, int height, CUtlBuffer &out );

#endif  // SE_GAMEUI_UIASSETS_H_
