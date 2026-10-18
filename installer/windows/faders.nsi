; SPDX-License-Identifier: GPL-2.0-or-later
; Windows installer for the Faders OBS plugin (NSIS 3, builds on Linux or Windows):
;   makensis -DVERSION=0.1.0 -DDLL=build-win/faders.dll -DOUTFILE=faders-obs-0.1.0-windows.exe installer/windows/faders.nsi
; Installs to %ProgramData%\obs-studio\plugins\faders, the per-machine plugin folder, and
; registers an uninstaller in Settings → Apps. The DLL goes in two places, sharing one data folder:
;   faders\faders.dll          OBS 33+ (https://obsproject.com/kb/legacy-plugin-locations)
;   faders\bin\64bit\faders.dll OBS 32 and older; OBS 33 skips it once the copy above has loaded
; Drop bin\64bit (and delete it on install) once OBS 33 is the minimum.
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "pass -DVERSION=x.y.z"
!endif
!ifndef DLL
  !error "pass -DDLL=path/to/faders.dll"
!endif
!ifndef OUTFILE
  !define OUTFILE "faders-obs-${VERSION}-windows.exe"
!endif
; makensis compiles from this script's folder, so paths are relative to it.
!define ROOT "../.."
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\FadersOBS"
; -DSIGN=path/to/sign-windows.sh signs the uninstaller (as it's generated) and the finished installer.
!ifdef SIGN
  !uninstfinalize '"${SIGN}" "%1"' = 0
  !finalize '"${SIGN}" "%1"' = 0
!endif

Name "Faders"
Caption "Faders ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
InstallDir "$COMMONPROGRAMDATA\obs-studio\plugins\faders"
BrandingText "faders.tv"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Faders"
VIAddVersionKey "CompanyName" "Faders"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later"
VIAddVersionKey "FileDescription" "Faders installer (plugin for OBS Studio)"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!include "MUI2.nsh"
!define MUI_ICON "faders.ico"
!define MUI_UNICON "faders.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "wizard.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "wizard.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TITLE "Faders"
!define MUI_WELCOMEPAGE_TEXT "Faders sends your voice, game and music as separate layers, so each viewer with the Faders browser extension can set their own levels. Your Twitch stream doesn't change.$\r$\n$\r$\nClose OBS before you continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT}/LICENSE"
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TITLE "You're set"
!define MUI_FINISHPAGE_TEXT "1. Open OBS, then Tools → Faders.$\r$\n2. Paste the key from faders.tv/portal.$\r$\n3. Put your mic, game and music on the Voice, Game and Music tracks."
!define MUI_FINISHPAGE_LINK "Open the Faders portal"
!define MUI_FINISHPAGE_LINK_LOCATION "https://faders.tv/portal/"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Faders" SecMain
  SectionIn RO
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File "/oname=faders.dll" "${DLL}"
  SetOutPath "$INSTDIR\bin\64bit"
  File "/oname=faders.dll" "${DLL}"
  SetOutPath "$INSTDIR\data"
  File /r "${ROOT}/data/*"
  SetOutPath "$INSTDIR"
  File "/oname=faders.ico" "faders.ico"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Faders"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Faders"
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "https://faders.tv/"
  WriteRegStr HKLM "${UNINST_KEY}" "HelpLink" "https://faders.tv/streamers/"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\faders.ico"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$INSTDIR\faders.dll"
  Delete "$INSTDIR\bin\64bit\faders.dll"
  RMDir "$INSTDIR\bin\64bit"
  RMDir "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  Delete "$INSTDIR\faders.ico"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
