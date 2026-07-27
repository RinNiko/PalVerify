Unicode True

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "Sections.nsh"

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
!define MUI_FINISHPAGE_RUN "$INSTDIR\Pal3Mien.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Palworld 3 Mien Launcher"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Var UpdateMode

Section "PalVerify" MainSection
    SectionIn RO
    SetShellVarContext current
    Delete "$INSTDIR\PalVerifyLauncher.exe"
    Delete "$SMPROGRAMS\PalVerify\PalVerify Launcher.lnk"
    RMDir /r "$INSTDIR\payload"
    SetOutPath "$INSTDIR"
    File "${SOURCE_ROOT}\Pal3Mien.exe"
    File "${SOURCE_ROOT}\README.txt"
    File "${SOURCE_ROOT}\SHA256SUMS.txt"

    WriteUninstaller "$INSTDIR\Uninstall.exe"

    CreateDirectory "$SMPROGRAMS\PalVerify"
    CreateShortcut \
        "$SMPROGRAMS\PalVerify\Palworld 3 Mien.lnk" \
        "$INSTDIR\Pal3Mien.exe" \
        "" \
        "$INSTDIR\Pal3Mien.exe"
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
        "DisplayIcon" "$INSTDIR\Pal3Mien.exe"
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

Section "Desktop shortcut" DesktopShortcutSection
SectionEnd

Section "-Desktop shortcut state"
    SetShellVarContext current
    SectionGetFlags ${DesktopShortcutSection} $R0
    IntOp $R0 $R0 & ${SF_SELECTED}
    StrCmp $R0 0 desktop_shortcut_disabled
    CreateShortcut \
        "$DESKTOP\Palworld 3 Mien.lnk" \
        "$INSTDIR\Pal3Mien.exe" \
        "" \
        "$INSTDIR\Pal3Mien.exe"
    WriteRegDWORD HKCU "Software\Palworld3Mien\PalVerify" \
        "DesktopShortcut" 1
    Goto desktop_shortcut_done

desktop_shortcut_disabled:
    Delete "$DESKTOP\Palworld 3 Mien.lnk"
    WriteRegDWORD HKCU "Software\Palworld3Mien\PalVerify" \
        "DesktopShortcut" 0

desktop_shortcut_done:
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    Delete "$DESKTOP\Palworld 3 Mien.lnk"
    Delete "$SMPROGRAMS\PalVerify\Palworld 3 Mien.lnk"
    Delete "$SMPROGRAMS\PalVerify\Uninstall PalVerify.lnk"
    RMDir "$SMPROGRAMS\PalVerify"

    Delete "$INSTDIR\Pal3Mien.exe"
    Delete "$INSTDIR\PalVerifyLauncher.exe"
    RMDir /r "$INSTDIR\payload"
    Delete "$INSTDIR\README.txt"
    Delete "$INSTDIR\SHA256SUMS.txt"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU \
        "Software\Microsoft\Windows\CurrentVersion\Uninstall\PalVerify"
    DeleteRegKey HKCU "Software\Palworld3Mien\PalVerify"
SectionEnd

Function .onInit
    StrCpy $UpdateMode "0"
    ${GetParameters} $R0
    ${GetOptions} $R0 "/UPDATE=" $R1
    StrCmp $R1 "1" 0 restore_desktop_preference
    StrCpy $UpdateMode "1"
    nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /IM Pal3Mien.exe'
    Sleep 500

restore_desktop_preference:
    ClearErrors
    ReadRegDWORD $R2 HKCU "Software\Palworld3Mien\PalVerify" \
        "DesktopShortcut"
    IfErrors init_done
    StrCmp $R2 0 0 init_done
    SectionSetFlags ${DesktopShortcutSection} 0

init_done:
FunctionEnd

Function .onInstSuccess
    StrCmp $UpdateMode "1" 0 update_done
    Exec '"$INSTDIR\Pal3Mien.exe"'

update_done:
FunctionEnd
