$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $projectRoot "dist"
$metadata = Get-Content `
    -Raw `
    -LiteralPath (Join-Path $projectRoot "packaging\Info.json") |
    ConvertFrom-Json
$version = [string]$metadata.Version
$releaseRoot = Join-Path $distRoot "PalVerify_Windows_v$version"

& (Join-Path $PSScriptRoot "package.ps1")
$basePackageSucceeded = $?
if (-not $basePackageSucceeded) {
    throw "Base PalVerify packaging failed."
}

$nsisCandidates = @(
    (Get-Command makensis.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue),
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$nsis = $nsisCandidates | Select-Object -First 1
if (-not $nsis) {
    throw "NSIS 3 is required. Install it with: winget install NSIS.NSIS"
}

$wixCandidates = @(
    (Join-Path $projectRoot ".deps\wix5\wix.exe"),
    (Get-Command wix.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -ErrorAction SilentlyContinue)
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$wix = $wixCandidates | Select-Object -First 1
if (-not $wix) {
    throw (
        "WiX is required. Install it with: " +
        "dotnet tool install --tool-path .deps\wix5 wix --version 5.0.2"
    )
}

$nsisOutput = Join-Path $distRoot "PalVerify_${version}_x64-setup.exe"
$msiOutput = Join-Path $distRoot "PalVerify_${version}_x64_en-US.msi"
$hashManifest = Join-Path $distRoot "PalVerify_INSTALLER_SHA256SUMS.txt"
$fileVersion = "$version.0"

foreach ($output in @($nsisOutput, $msiOutput, $hashManifest)) {
    if (Test-Path -LiteralPath $output) {
        Remove-Item -LiteralPath $output -Force
    }
}

$nsisScript = Join-Path `
    $projectRoot `
    "packaging\windows\nsis\PalVerify.nsi"
$installerIcon = Join-Path `
    $projectRoot `
    "packaging\windows\PalVerify.ico"
& $nsis `
    "/DVERSION=$version" `
    "/DFILE_VERSION=$fileVersion" `
    "/DSOURCE_ROOT=$releaseRoot" `
    "/DOUTPUT_EXE=$nsisOutput" `
    "/DINSTALLER_ICON=$installerIcon" `
    $nsisScript
if ($LASTEXITCODE -ne 0) {
    throw "NSIS packaging failed."
}

$wixSource = Join-Path `
    $projectRoot `
    "packaging\windows\wix\PalVerify.wxs"
& $wix build `
    $wixSource `
    -arch x64 `
    "-d" "SourceRoot=$releaseRoot" `
    "-d" "Version=$version" `
    -o $msiOutput
if ($LASTEXITCODE -ne 0) {
    throw "WiX MSI packaging failed."
}

$certificate = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1
$signTool = Get-ChildItem `
    "C:\Program Files (x86)\Windows Kits\10\bin" `
    -Filter signtool.exe `
    -Recurse `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if ($certificate -and $signTool) {
    foreach ($installer in @($nsisOutput, $msiOutput)) {
        & $signTool.FullName sign `
            /sha1 $certificate.Thumbprint `
            /fd SHA256 `
            /td SHA256 `
            /tr "http://timestamp.digicert.com" `
            $installer
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode signing failed: $installer"
        }
    }
    "AUTHENTICODE_SIGNED subject=$($certificate.Subject)"
} else {
    "AUTHENTICODE_UNSIGNED reason=no-valid-code-signing-certificate"
}

$hashLines = foreach ($installer in @($nsisOutput, $msiOutput)) {
    $hash = (Get-FileHash `
        -LiteralPath $installer `
        -Algorithm SHA256).Hash
    "$hash  $([System.IO.Path]::GetFileName($installer))"
}
$hashLines | Set-Content -LiteralPath $hashManifest -Encoding utf8

"NSIS_INSTALLER=$nsisOutput"
"NSIS_SHA256=$((Get-FileHash -LiteralPath $nsisOutput -Algorithm SHA256).Hash)"
"MSI_INSTALLER=$msiOutput"
"MSI_SHA256=$((Get-FileHash -LiteralPath $msiOutput -Algorithm SHA256).Hash)"
"INSTALLER_HASHES=$hashManifest"
