// Copyright Source Authors. All rights reserved.
//
// gameui_cefsub.exe: CEF helper process (renderer, GPU, utility, ...).
// Lives next to libcef.dll so that all CEF runtime files resolve locally.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "include/cef_app.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  CefMainArgs main_args(instance);
  return CefExecuteProcess(main_args, nullptr, nullptr);
}
