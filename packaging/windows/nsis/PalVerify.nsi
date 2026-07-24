Unicode True

!include "MUI2.nsh"

!ifndef VERSION
    !error "VERSION define is required"
!endif
!ifndef FILE_VERSION
    !error "FILE_VERSION define is required"
!endif
!ifndef SOURCE_ROOT
    !error "SOURCE_ROOT define is required"
!endif
!ifndef OUTPUT_EXE
    !error "OUTPUT_EXE define is required"
!endif
!ifndef INSTALLER_ICON
    !error "INSTALLER_ICON define is required"
!endif

Name "PalVerify"
OutFile "${OUTPUT_EXE}"
InstallDir "$LOCALAPPDATA\Programs\PalVerify"
InstallDirRegKey HKCU "Software\Palworld3Mien\PalVerify" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma

VIProductVersion "${FILE_VERSION}"
VIAddVersionKey /LANG=1033 "ProductName" "PalVerify"
VIAddVersionKey /LANG=1033 "CompanyName" "Palworld 3 Mien"
VIAddVersionKey /LANG=1033 "FileDescription" "PalVerify Windows Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright Palworld 3 Mien"

!define MUI_ABORTWARNING
!define MUI_ICON "${INSTALLER_ICON}"
!define MUI_UNICON "${INSTALLER_ICON}"
!define MUI_FINISHPAGE_RUN "$INSTDIR\PalVerifyLauncher.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run PalVerify Launcher"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "PalVerify" MainSection
    SetShellVarContext current
    SetOutPath "$INSTDIR"
    File "${SOURCE_ROOT}\PalVerifyLauncher.exe"
    File "${SOURCE_ROOT}\README.txt"
    File "${SOURCE_ROOT}\SHA256SUMS.txt"

    SetOutPath "$INSTDIR\payload"
    File /r "${SOURCE_ROOT}\payload\*.*"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    CreateDirectory "$SMPROGRAMS\PalVerify"
    CreateShortcut \
        "$SMPROGRAMS\PalVerify\PalVerify Launcher.lnk" \
        "$INSTDIR\PalVerifyLauncher.exe" \
        "" \
        "$INSTDIR\PalVerifyLauncher.exe"
    CreateShortcut \
        "$SMPROGRAMS\PalVerify\Uninstall PalVerify.lnk" \
        "$INSTDIR\Uninstall.exe"

    WriteRegStr HKCU "Software\Palworld3Mien\PalVerify" \
        "InstallDir" "$INSTDIR"
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "DisplayName" "PalVerify"
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "Publisher" "Palworld 3 Mien"
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "DisplayIcon" "$INSTDIR\PalVerifyLauncher.exe"
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "NoModify" 1
    WriteRegDWORD HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify" \
        "NoRepair" 1
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$SMPROGRAMS\PalVerify\PalVerify Launcher.lnk"
    Delete "$SMPROGRAMS\PalVerify\Uninstall PalVerify.lnk"
    RMDir "$SMPROGRAMS\PalVerify"

    Delete "$INSTDIR\PalVerifyLauncher.exe"
    Delete "$INSTDIR\README.txt"
    Delete "$INSTDIR\SHA256SUMS.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir /r "$INSTDIR\payload"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify"
    DeleteRegKey HKCU "Software\Palworld3Mien\PalVerify"
SectionEnd
