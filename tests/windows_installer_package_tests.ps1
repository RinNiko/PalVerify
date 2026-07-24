$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$failures = [System.Collections.Generic.List[string]]::new()

function Require-File {
    param([string]$RelativePath)

    $fullPath = Join-Path $projectRoot $RelativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        $failures.Add("missing file: $RelativePath")
        return $null
    }
    return Get-Content -Raw -LiteralPath $fullPath
}

function Require-Text {
    param(
        [AllowNull()][string]$Content,
        [string]$Expected,
        [string]$Message
    )

    if ($null -eq $Content -or -not $Content.Contains($Expected)) {
        $failures.Add($Message)
    }
}

function Require-NoText {
    param(
        [AllowNull()][string]$Content,
        [string]$Unexpected,
        [string]$Message
    )

    if ($null -ne $Content -and $Content.Contains($Unexpected)) {
        $failures.Add($Message)
    }
}

$nsis = Require-File "packaging\windows\nsis\PalVerify.nsi"
$wix = Require-File "packaging\windows\wix\PalVerify.wxs"
$packager = Require-File "scripts\package-installers.ps1"
$installerIcon = Join-Path `
    $projectRoot `
    "packaging\windows\PalVerify.ico"
if (-not (Test-Path -LiteralPath $installerIcon -PathType Leaf)) {
    $failures.Add("missing file: packaging\windows\PalVerify.ico")
}

$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $projectRoot "scripts\package-installers.ps1"),
    [ref]$tokens,
    [ref]$parseErrors
) | Out-Null
if ($parseErrors.Count -gt 0) {
    $failures.Add(
        "package-installers.ps1 must parse: $($parseErrors[0].Message)"
    )
}

Require-Text $nsis 'RequestExecutionLevel user' `
    "NSIS installer must not require administrator rights"
Require-Text $nsis 'InstallDir "$LOCALAPPDATA\Programs\PalVerify"' `
    "NSIS installer must use a per-user install directory"
Require-Text $nsis 'File "${SOURCE_ROOT}\PalVerifyLauncher.exe"' `
    "NSIS installer must include PalVerifyLauncher.exe"
Require-Text $nsis 'File /r "${SOURCE_ROOT}\payload\*.*"' `
    "NSIS installer must include the complete payload directory"
Require-Text $nsis 'WriteUninstaller "$INSTDIR\Uninstall.exe"' `
    "NSIS installer must provide an uninstaller"
Require-Text $nsis 'MUI_ICON "${INSTALLER_ICON}"' `
    "NSIS installer must use a Windows ICO asset"
Require-Text $nsis '"LegalCopyright"' `
    "NSIS version metadata must include copyright information"

Require-Text $wix 'Scope="perUser"' `
    "MSI installer must use per-user scope"
Require-Text $wix 'StandardDirectory Id="LocalAppDataFolder"' `
    "MSI installer must use LocalAppData"
Require-Text $wix '<MajorUpgrade' `
    "MSI installer must support major upgrades"
Require-Text $wix 'PalVerifyLauncher.exe' `
    "MSI installer must include PalVerifyLauncher.exe"
Require-Text $wix 'PalVerifyClient.exe' `
    "MSI installer must include PalVerifyClient.exe"
Require-NoText $wix 'Guid="{' `
    "WiX component GUIDs must not use braces"
Require-NoText $wix 'UpgradeCode="{' `
    "WiX upgrade code must not use braces"
Require-Text $wix 'Guid="*"' `
    "WiX file components must use deterministic generated GUIDs"

Require-Text $packager 'PalVerify_${version}_x64-setup.exe' `
    "packager must produce an NSIS setup executable"
Require-Text $packager 'PalVerify_${version}_x64_en-US.msi' `
    "packager must produce an MSI"
Require-Text $packager 'PalVerify_INSTALLER_SHA256SUMS.txt' `
    "packager must produce an installer hash manifest"
Require-Text $packager '$basePackageSucceeded = $?' `
    "packager must use PowerShell success state for nested scripts"
Require-Text $packager '".deps\wix5\wix.exe"' `
    "packager must use the WiX 5 tool path without the WiX 6/7 OSMF gate"
Require-Text $packager 'wix --version 5.0.2' `
    "packager must document the reproducible WiX 5.0.2 bootstrap command"

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        Write-Output "FAIL $failure"
    }
    exit 1
}

Write-Output "PASS Windows installer package contract"
