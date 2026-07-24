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
$packager = Require-File "scripts\package-installers.ps1"
$basePackager = Require-File "scripts\package.ps1"
$builder = Require-File "scripts\build.ps1"
$launcherSource = Require-File "apps\launcher\main.cpp"
$launcherStateSource = Require-File "src\launcher_state.cpp"
$launcherResources = Require-File "resources\launcher\launcher.rc"
$launcherResourceHeader = Require-File "resources\launcher\resource.h"
$payloadPacker = Require-File "apps\payload_packer\main.cpp"
$payloadArchive = Require-File "src\payload_archive.cpp"
$cmake = Require-File "CMakeLists.txt"
$clientAgentSource = Require-File "apps\client_agent\main.cpp"
$serverAgentSource = Require-File "cmd\palverify-server\main.go"
$coordinatorSource = Require-File "cmd\palverify-coordinator\main.go"
$manifestSyncSource = Require-File `
    "internal\coordinator\manifest_sync.go"
$clientConfig = Require-File "packaging\client\Scripts\config.json"
$packageInfo = Require-File "packaging\Info.json"
$serverConfig = Require-File `
    "packaging\server\Scripts\server-config.example.json"
$releaseManifest = Require-File "release\palverify-launcher-manifest.json"
$installerIcon = Join-Path `
    $projectRoot `
    "resources\launcher\pal3mien.ico"
if (-not (Test-Path -LiteralPath $installerIcon -PathType Leaf)) {
    $failures.Add("missing file: resources\launcher\pal3mien.ico")
}
$launcherBackground = Join-Path `
    $projectRoot `
    "resources\launcher\background-reference-v2.png"
if (-not (Test-Path -LiteralPath $launcherBackground -PathType Leaf)) {
    $failures.Add(
        "launcher must include the cleaned reference artwork from design image 6"
    )
}
$readyStartButton = Join-Path `
    $projectRoot `
    "resources\launcher\button-start-ready-reference.png"
if (-not (Test-Path -LiteralPath $readyStartButton -PathType Leaf)) {
    $failures.Add(
        "launcher must include the exact ready-state Start reference asset"
    )
}
$newsButton = Join-Path `
    $projectRoot `
    "resources\launcher\button-news-reference.png"
if (-not (Test-Path -LiteralPath $newsButton -PathType Leaf)) {
    $failures.Add(
        "launcher must include the exact News reference asset"
    )
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
Require-Text $nsis 'File "${SOURCE_ROOT}\Pal3Mien.exe"' `
    "NSIS installer must include Pal3Mien.exe"
Require-NoText $nsis '${SOURCE_ROOT}\payload' `
    "NSIS installer must not ship an external payload directory"
Require-NoText $nsis 'CreateDirectory "$INSTDIR\payload"' `
    "NSIS install root must not create an external payload directory"
Require-Text $nsis 'RMDir /r "$INSTDIR\payload"' `
    "NSIS upgrade must remove legacy external payload directories"
Require-Text $nsis 'WriteUninstaller "$INSTDIR\Uninstall.exe"' `
    "NSIS installer must provide an uninstaller"
Require-Text $nsis 'MUI_ICON "${INSTALLER_ICON}"' `
    "NSIS installer must use a Windows ICO asset"
Require-Text $nsis '"LegalCopyright"' `
    "NSIS version metadata must include copyright information"

Require-Text $packager 'Pal3Mien-Setup.exe' `
    "packager must produce the permanent NSIS setup filename"
Require-Text $packager '"package.json"' `
    "installer version must track the launcher release independently"
Require-NoText $packager '.msi' `
    "official packaging must produce only the player-facing EXE"
Require-Text $packager 'Pal3Mien-Setup.sha256.txt' `
    "packager must produce a local installer hash record"
Require-Text $packager '$basePackageSucceeded = $?' `
    "packager must use PowerShell success state for nested scripts"
Require-Text $builder 'go test ./...' `
    "build must run coordinator and server-agent tests"
Require-Text $builder 'go build -trimpath -o $serverAgent' `
    "build must compile PalVerifyServer.exe"
Require-Text $builder 'palverify-coordinator-linux-amd64' `
    "build must compile the production Linux coordinator"
Require-Text $basePackager 'PalVerifyServer.exe' `
    "server package must include PalVerifyServer.exe"
Require-NoText $basePackager '$payloadRoot' `
    "portable Windows package must not stage a payload directory"
Require-NoText $basePackager '*.pdb' `
    "release packaging must not copy PDB files"
Require-Text $basePackager 'RELEASE_LAYOUT_OK no-external-payload no-pdb' `
    "release packaging must enforce the payload/PDB layout at build time"
Require-Text $basePackager 'AUTHENTICODE_UNSIGNED' `
    "release packaging must explicitly report an unsigned build"
Require-Text $basePackager 'PACKAGE_DIGEST=' `
    "release packaging must report the exact installed package digest"
Require-Text $basePackager 'palVerifyPackageDigest' `
    "release packaging must verify the GitHub manifest package digest"
Require-Text $basePackager '[StringComparer]::Ordinal' `
    "package digest paths must use the same ordinal order as the client"
Require-Text $cmake 'palverify_payload_packer' `
    "CMake must build a dedicated payload resource packer"
Require-Text $cmake 'embedded-palverify-payload.bin' `
    "CMake must generate the compressed payload resource"
Require-Text $cmake 'target_link_options(PalVerifyClient PRIVATE /Brepro)' `
    "PalVerify client builds must be reproducible across identical rebuilds"
Require-Text $cmake 'target_link_options(Pal3Mien PRIVATE /Brepro)' `
    "launcher builds must be reproducible across identical rebuilds"
Require-Text $launcherResourceHeader '#define IDR_PALVERIFY_PAYLOAD 216' `
    "launcher resources must reserve a stable embedded payload resource ID"
Require-Text $launcherSource 'unpack_payload_archive' `
    "launcher must unpack the embedded resource before installation"
Require-Text $payloadPacker 'pack_payload_archive' `
    "payload packer must use the versioned compression format"
Require-Text $payloadPacker 'IDR_PALVERIFY_PAYLOAD RCDATA' `
    "payload packer must generate the PE RCDATA resource script"
Require-Text $payloadArchive 'payload-hash-mismatch' `
    "payload archive must reject SHA-256 mismatches"
Require-Text $launcherSource 'PalVerifyClient.exe' `
    "launcher must start the installed PalVerify client agent"
Require-Text $launcherSource 'CreateProcessW' `
    "launcher must create the client-agent process directly"
Require-Text $launcherSource `
    'https://palworld-3-mien-website.vercel.app/' `
    "launcher website fallback must open the Palworld 3 Mien website"
Require-NoText $launcherSource 'icon_settings' `
    "launcher must not render the removed theme/settings button"
Require-Text $launcherSource 'draw_glass_panel' `
    "launcher must draw opaque glass panels instead of transparent templates"
Require-Text $launcherSource 'draw_action_button' `
    "launcher must render Start and News with a dedicated solid action style"
Require-Text $launcherSource 'Gdiplus::Color{255, 7, 58, 87}' `
    "launcher action buttons must use a fully opaque high-contrast fill"
Require-Text $launcherSource 'TextRenderingHintAntiAliasGridFit' `
    "launcher text must avoid ClearType color fringing on the back buffer"
Require-Text $launcherResources 'background-reference-v2.png' `
    "launcher resources must use the cleaned design-image-6 background"
Require-Text $launcherResources 'button-start-ready-reference.png' `
    "launcher resources must use the exact ready Start artwork"
Require-Text $launcherResources 'button-news-reference.png' `
    "launcher resources must use the exact News artwork"
Require-Text $launcherSource 'button_start_ready' `
    "launcher must select the exact Start reference in ready state"
Require-NoText $launcherSource 'graphics.FillRectangle(&highlight_fill, highlight);' `
    "checking-state button must not draw a stray cyan highlight streak"
Require-NoText $launcherSource 'inner_rectangle.Inflate(-4.0F, -4.0F);' `
    "checking-state button must not draw a second cyan top border"
Require-Text $launcherSource 'button_news_reference' `
    "launcher must render the exact News reference"
Require-Text $launcherSource 'TrackMouseEvent' `
    "launcher buttons must track pointer leave for hover feedback"
Require-Text $launcherSource 'WM_MOUSELEAVE' `
    "launcher buttons must clear hover feedback when the pointer leaves"
Require-Text $launcherSource 'WM_LBUTTONUP' `
    "launcher buttons must activate on release after pressed feedback"
Require-Text $launcherSource 'draw_button_interaction' `
    "launcher buttons must render hover and pressed feedback"
Require-Text $launcherSource 'show_error_details' `
    "launcher failure button must open detailed diagnostics"
Require-Text $launcherSource 'copy_text_to_clipboard' `
    "launcher diagnostics must be copied for the player to send to an admin"
Require-Text $launcherSource 'failure_support_hint' `
    "launcher failure button must explain how to share diagnostics"
Require-Text $launcherSource 'constexpr float heading_font_size = 24.0F' `
    "launcher headings must use the larger readable type scale"
Require-Text $launcherSource 'constexpr float body_font_size = 18.0F' `
    "launcher body copy must use a larger readable type scale"
Require-Text $launcherSource 'constexpr float supporting_font_size = 17.0F' `
    "launcher supporting copy must remain readable over the artwork"
Require-Text $launcherSource `
    'Gdiplus::FontStyle style = Gdiplus::FontStyleBold' `
    "launcher text must default to a stronger font weight"
Require-Text $launcherResources 'FILEVERSION 1,0,0,0' `
    "launcher executable metadata must expose official version 1.0"
Require-Text $launcherSource 'launcher_version = "1.0"' `
    "launcher diagnostics and UI must expose official version 1.0"
Require-Text $launcherSource `
    'https://raw.githubusercontent.com/RinNiko/PalVerify/main/' `
    "launcher manifest must use the stable raw GitHub endpoint"
Require-Text $launcherSource `
    'WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000)' `
    "launcher manifest request must tolerate slow GitHub connections"
Require-Text $launcherSource 'palverify_version = "1.0"' `
    "launcher must bundle PalVerify verifier version 1.0"
Require-Text $releaseManifest `
    '"launcherDownloadUrl": "https://github.com/RinNiko/PalVerify/releases/download/stable/Pal3Mien-Setup.exe"' `
    "stable manifest must use the permanent player-facing installer URL"
Require-Text $launcherSource 'L"/S /UPDATE=1"' `
    "mandatory updates must launch the verified installer in silent update mode"
Require-Text $launcherSource `
    'Launcher và Palworld sẽ tự đóng để cài bản mới.' `
    "launcher must explain that the current game and launcher will close"
Require-Text $nsis '!insertmacro MUI_PAGE_COMPONENTS' `
    "installer must expose installation options"
Require-Text $nsis 'Section "Desktop shortcut" DesktopShortcutSection' `
    "installer must offer a Desktop shortcut option"
Require-NoText $nsis 'Section /o "Desktop shortcut"' `
    "Desktop shortcut option must be selected by default"
Require-Text $nsis '$DESKTOP\Palworld 3 Mien.lnk' `
    "installer must create and remove the Desktop shortcut"
foreach ($processName in @(
    "Pal3Mien.exe",
    "Palworld.exe",
    "Palworld-Win64-Shipping.exe",
    "PalVerifyClient.exe"
)) {
    Require-Text $nsis $processName `
        "update mode must close $processName before replacing files"
}
Require-Text $launcherStateSource 'steam://rungameid/1623730' `
    "launcher must open Palworld normally through Steam"
Require-NoText $launcherSource 'steam://run/1623730' `
    "launcher must not auto-join a server through Steam"
Require-NoText $launcherSource 'launch_steam_with_auto_join' `
    "launcher must not retain the auto-join launch path"
Require-Text $packageInfo '"Version": "1.0"' `
    "PalVerify package metadata must expose verifier version 1.0"
Require-Text $clientAgentSource 'CLIENT_STARTED protocol=3 version=1.0' `
    "client runtime log must expose verifier version 1.0"
Require-Text $serverAgentSource 'server agent started version=1.0' `
    "server runtime log must expose verifier version 1.0"
Require-Text $coordinatorSource 'RunManifestSync' `
    "coordinator must continuously synchronize the trusted GitHub manifest"
Require-Text $manifestSyncSource `
    'https://raw.githubusercontent.com/RinNiko/PalVerify/main/' `
    "coordinator sync must pin the PalVerify GitHub manifest"
Require-NoText $clientConfig '"token"' `
    "client package must not contain a shared bearer token"
Require-Text $clientConfig '"coordinator"' `
    "client package must use the public coordinator base URL"
Require-Text $clientAgentSource 'CLIENT_WAITING_FOR_GAME' `
    "client agent must wait for Palworld to start"
Require-Text $clientAgentSource 'write_event(log, "INTEGRITY_VIOLATION")' `
    "client log must use a generic integrity event"
Require-Text $clientAgentSource 'scan_palworld_modules' `
    "client agent must scan loaded Palworld modules for injection"
Require-NoText $clientAgentSource 'write_event(log, violations.back())' `
    "client log must not expose the matched process rule"
Require-Text $clientConfig `
    'https://palworld-3-mien-website.vercel.app/api/palverify' `
    "client config must use the production HTTPS coordinator"
Require-NoText $clientConfig 'ingest.minerua.net' `
    "client config must not use the retired MineRua coordinator"
Require-NoText $serverConfig 'ingest.minerua.net' `
    "server config must not use the retired MineRua coordinator"
Require-Text $serverConfig '"restUrl": "http://127.0.0.1:17993"' `
    "server config must keep PalDefender REST on loopback"
if ($null -ne $packageInfo) {
    $metadata = $packageInfo | ConvertFrom-Json
    if ($metadata.DebugMode -ne $false) {
        $failures.Add(
            "release package must disable game-mod debug mode"
        )
    }
    if (@($metadata.Dependencies).Count -ne 0) {
        $failures.Add(
            "standalone client package must not declare an in-game dependency"
        )
    }
    $clientRules = @(
        $metadata.InstallRule |
            Where-Object { $_.IsServer -ne $true }
    )
    if ($clientRules.Count -ne 0) {
        $failures.Add(
            "standalone verifier must not install a client-side game mod"
        )
    }
}
if ($null -ne $releaseManifest) {
    $releaseMetadata = $releaseManifest | ConvertFrom-Json
    if (
        $releaseMetadata.palVerifyPackageDigest -notmatch
            '^[0-9a-f]{64}$'
    ) {
        $failures.Add(
            "stable manifest must contain the exact PalVerify package digest"
        )
    }
}

if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        Write-Output "FAIL $failure"
    }
    exit 1
}

Write-Output "PASS Windows installer package contract"
