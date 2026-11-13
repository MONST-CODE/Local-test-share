; NSIS 3.11. Recette de développement : validation Windows native obligatoire.
; Build : makensis /DSTAGE=C:\path\out\stage /DOUTPUT=C:\path\out\PLPT-preview-setup.exe plpt-preview.nsi
Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!ifndef STAGE
  !error "STAGE doit designer le resultat de cmake --install (Release avec UI)."
!endif
!ifndef OUTPUT
  !error "OUTPUT doit designer le fichier installateur .exe a generer."
!endif
Name "PLPT - Moniteur local (Preview)"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\PLPT-Preview"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
!define MUI_WELCOMEPAGE_TEXT "Preview technique du moniteur local PLPT.$\r$\n$\r$\nAucun partage distant, invitation ni navigateur WebView2 dans cette livraison.$\r$\n$\r$\nInstallation pour l'utilisateur courant. Aucun service, demarrage automatique ou changement du pare-feu."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "French"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Cette preview exige Windows x64."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext current
  IfFileExists "$INSTDIR\Uninstall.exe" 0 done
    MessageBox MB_OK|MB_ICONEXCLAMATION "Une preview existe deja. Desinstallez-la avant de poursuivre."
    Abort
  done:
FunctionEnd
Section "PLPT monitor" SEC_MAIN
  SetOutPath "$INSTDIR"
  File "${STAGE}\bin\plpt-monitor.exe"
  File "${STAGE}\bin\plpt-desktop.exe"
  SetOutPath "$INSTDIR\docs"
  File "${STAGE}\share\doc\plpt\docs\QUICKSTART.md"
  File "${STAGE}\share\doc\plpt\third_party\DEPENDENCIES.md"
  SetOutPath "$INSTDIR\licenses"
  File "${STAGE}\share\licenses\plpt\SDL3.txt"
  File "${STAGE}\share\licenses\plpt\ImGui.txt"
  File "${STAGE}\share\licenses\plpt\Bundled-notices.txt"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\PLPT Preview.lnk" "$INSTDIR\plpt-desktop.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview" "DisplayName" "PLPT local monitor - Preview"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview" "DisplayVersion" "0.1.0"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview" "UninstallString" '$"$INSTDIR\Uninstall.exe$"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview" "NoRepair" 1
SectionEnd
Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  ; Jamais RMDir /r sur une racine utilisateur : supprimer seulement les fichiers installes connus.
  Delete "$SMPROGRAMS\PLPT Preview.lnk"
  Delete "$INSTDIR\plpt-monitor.exe"
  Delete "$INSTDIR\plpt-desktop.exe"
  Delete "$INSTDIR\docs\QUICKSTART.md"
  Delete "$INSTDIR\docs\DEPENDENCIES.md"
  Delete "$INSTDIR\licenses\SDL3.txt"
  Delete "$INSTDIR\licenses\ImGui.txt"
  Delete "$INSTDIR\licenses\Bundled-notices.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR\docs"
  RMDir "$INSTDIR\licenses"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\PLPT-Preview"
SectionEnd
