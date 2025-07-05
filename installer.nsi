; Sound Tracker NSIS Installer Script
!define PRODUCT_NAME "Windows Sound Tracker"
!define PRODUCT_VERSION "1.0"
!define PRODUCT_PUBLISHER "Your Company"
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\SoundTracker.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

; MUI Settings
!include "MUI2.nsh"
!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

; Welcome page
!insertmacro MUI_PAGE_WELCOME
; License page (optional)
; !insertmacro MUI_PAGE_LICENSE "License.txt"
; Directory page
!insertmacro MUI_PAGE_DIRECTORY
; Instfiles page
!insertmacro MUI_PAGE_INSTFILES
; Finish page
!define MUI_FINISHPAGE_RUN "$INSTDIR\SoundTracker.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Sound Tracker"
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_INSTFILES

; Language files
!insertmacro MUI_LANGUAGE "English"

; Installer attributes
Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "SoundTracker_Setup.exe"
InstallDir "$PROGRAMFILES64\Sound Tracker"
InstallDirRegKey HKLM "${PRODUCT_DIR_REGKEY}" ""
ShowInstDetails show
ShowUnInstDetails show
RequestExecutionLevel admin

Section "MainSection" SEC01
  SetOutPath "$INSTDIR"
  SetOverwrite try
  
  ; Copy main executable
  File "standalone\bin\Release\SoundTracker.exe"
  
  ; Create logs directory
  CreateDirectory "$INSTDIR\logs"
  
  ; Create shortcuts
  CreateDirectory "$SMPROGRAMS\Sound Tracker"
  CreateShortCut "$SMPROGRAMS\Sound Tracker\Sound Tracker.lnk" "$INSTDIR\SoundTracker.exe"
  CreateShortCut "$DESKTOP\Sound Tracker.lnk" "$INSTDIR\SoundTracker.exe"
  
  ; Create uninstaller
  WriteUninstaller "$INSTDIR\uninst.exe"
  CreateShortCut "$SMPROGRAMS\Sound Tracker\Uninstall.lnk" "$INSTDIR\uninst.exe"
  
  ; Write registry keys
  WriteRegStr HKLM "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\SoundTracker.exe"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\SoundTracker.exe"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
SectionEnd

Section Uninstall
  ; Remove files
  Delete "$INSTDIR\SoundTracker.exe"
  Delete "$INSTDIR\uninst.exe"
  
  ; Remove shortcuts
  Delete "$SMPROGRAMS\Sound Tracker\Sound Tracker.lnk"
  Delete "$SMPROGRAMS\Sound Tracker\Uninstall.lnk"
  Delete "$DESKTOP\Sound Tracker.lnk"
  
  ; Remove directories
  RMDir "$SMPROGRAMS\Sound Tracker"
  RMDir /r "$INSTDIR"
  
  ; Remove registry keys
  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_DIR_REGKEY}"
  
  SetAutoClose true
SectionEnd