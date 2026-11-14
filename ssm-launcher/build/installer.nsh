; SourceSpeedrunMod launcher installer customizations (electron-builder NSIS).
;
; Installs to C:\Program Files\SSM-Launcher. The mod itself is downloaded by
; the launcher into ...\SSM-Launcher\Games, which is made writable for normal
; users (the launcher runs without admin rights, and the game writes its saves
; and configs into its own folder). Uninstalling or updating the launcher never
; deletes Games\.

!macro preInit
  ; Default install folder (only when there is no previous per-machine install).
  SetRegView 64
  ReadRegStr $0 HKLM "${INSTALL_REGISTRY_KEY}" InstallLocation
  ${if} $0 == ""
    WriteRegExpandStr HKLM "${INSTALL_REGISTRY_KEY}" InstallLocation "$PROGRAMFILES64\SSM-Launcher"
  ${endif}
  SetRegView 32
  ReadRegStr $0 HKLM "${INSTALL_REGISTRY_KEY}" InstallLocation
  ${if} $0 == ""
    WriteRegExpandStr HKLM "${INSTALL_REGISTRY_KEY}" InstallLocation "$PROGRAMFILES64\SSM-Launcher"
  ${endif}
!macroend

!macro customInstall
  ; Games\ for the downloaded mod: Users get Modify (inherited by everything inside).
  CreateDirectory "$INSTDIR\Games"
  nsExec::ExecToLog 'icacls "$INSTDIR\Games" /grant *S-1-5-32-545:(OI)(CI)M /T /C /Q'

  !ifndef SSM_TEST
  ; Launcher 1.1 installed per user; remove that copy so there is only one.
  ; (Its downloaded mod in %LOCALAPPDATA%\SourceSpeedrunMod stays and keeps working.)
  IfFileExists "$LOCALAPPDATA\Programs\ssm-launcher\Uninstall SourceSpeedrunMod Launcher.exe" 0 +2
    ExecWait '"$LOCALAPPDATA\Programs\ssm-launcher\Uninstall SourceSpeedrunMod Launcher.exe" /S /currentuser'
  !endif
!macroend

!macro customRemoveFiles
  ; Remove the launcher's own files, keep Games\ (the mod, saves, configs).
  SetOutPath $TEMP
  FindFirst $0 $1 "$INSTDIR\*.*"
  ssm_remove_loop:
    StrCmp $1 "" ssm_remove_done
    StrCmp $1 "." ssm_remove_next
    StrCmp $1 ".." ssm_remove_next
    StrCmp $1 "Games" ssm_remove_next
    IfFileExists "$INSTDIR\$1\*.*" 0 ssm_remove_file
      RMDir /r "$INSTDIR\$1"
      Goto ssm_remove_next
    ssm_remove_file:
      Delete "$INSTDIR\$1"
  ssm_remove_next:
    FindNext $0 $1
    Goto ssm_remove_loop
  ssm_remove_done:
  FindClose $0
  ; Only removed when empty (no Games\ left).
  RMDir "$INSTDIR"
!macroend
