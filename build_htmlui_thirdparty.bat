@ECHO OFF
Setlocal EnableDelayedExpansion

REM Builds third-party dependencies of the HTML GameUI (gameui.dll):
REM   1. Downloads the CEF (Chromium Embedded Framework) binary distribution into
REM      thirdparty\cef if it is not there yet, verifying its SHA1.
REM   2. Builds gameui\cefbridge (libcef_dll_wrapper, htmlhost, gameui_cefsub).
REM
REM Usage: build_htmlui_thirdparty.bat <x64>
REM Must be run from a Visual Studio x64 developer command prompt.

IF NOT ["%~1"]==["x64"] (
  ECHO HTML GameUI supports x64 only.
  EXIT /B 1
)

SET CEF_VERSION=156.0.30+g6028375+chromium-156.0.8078.96
SET CEF_PLATFORM=windows64
SET CEF_SHA1=1fedfc8dd0274caec9183a709ad1ed7856867b91
SET CEF_NAME=cef_binary_%CEF_VERSION%_%CEF_PLATFORM%_minimal
SET CEF_URL=https://cef-builds.spotifycdn.com/cef_binary_156.0.30%%2Bg6028375%%2Bchromium-156.0.8078.96_windows64_minimal.tar.bz2
SET CEF_DIR=%~dp0thirdparty\cef

IF NOT EXIST "%CEF_DIR%\include\cef_version.h" (
  ECHO Downloading CEF %CEF_VERSION%...
  IF NOT EXIST "%~dp0thirdparty\cef_download" MKDIR "%~dp0thirdparty\cef_download"
  PUSHD "%~dp0thirdparty\cef_download"

  curl -L --fail -o cef.tar.bz2 "%CEF_URL%"
  IF ERRORLEVEL 1 (
    ECHO Downloading CEF failed.
    POPD
    EXIT /B 1
  )

  certutil -hashfile cef.tar.bz2 SHA1 | findstr /I /C:"%CEF_SHA1%" >NUL
  IF ERRORLEVEL 1 (
    ECHO CEF archive SHA1 mismatch: expected %CEF_SHA1%.
    POPD
    EXIT /B 1
  )

  tar -xjf cef.tar.bz2
  IF ERRORLEVEL 1 (
    ECHO Extracting CEF failed.
    POPD
    EXIT /B 1
  )
  POPD

  FOR /D %%D IN ("%~dp0thirdparty\cef_download\cef_binary_*") DO MOVE "%%D" "%CEF_DIR%" >NUL
  RMDIR /S /Q "%~dp0thirdparty\cef_download"
)

IF NOT EXIST "%CEF_DIR%\include\cef_version.h" (
  ECHO CEF distribution is missing in %CEF_DIR%.
  EXIT /B 1
)

REM Build the CEF bridge.
cmake -S "%~dp0gameui\cefbridge" -B "%~dp0gameui\cefbridge\out" -G "Visual Studio 18 2026" -A x64
IF ERRORLEVEL 1 (
  ECHO cmake generation for gameui\cefbridge failed.
  EXIT /B 1
)

cmake --build "%~dp0gameui\cefbridge\out" --config Release
IF ERRORLEVEL 1 (
  ECHO cmake --build Release for gameui\cefbridge failed.
  EXIT /B 1
)

cmake --build "%~dp0gameui\cefbridge\out" --config Debug
IF ERRORLEVEL 1 (
  ECHO cmake --build Debug for gameui\cefbridge failed.
  EXIT /B 1
)

EXIT /B 0
