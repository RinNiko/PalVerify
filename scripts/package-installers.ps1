$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$distRoot = Join-Path $projectRoot "dist"
$metadata = Get-Content `
    -Raw `
    -LiteralPath (Join-Path $projectRoot "package.json") |
    ConvertFrom-Json
$version = [string]$metadata.version
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

$nsisOutput = Join-Path $distRoot "Pal3Mien-Setup.exe"
$hashManifest = Join-Path $distRoot "Pal3Mien-Setup.sha256.txt"
$fileVersion = "$version.0"

foreach ($output in @($nsisOutput, $hashManifest)) {
    if (Test-Path -LiteralPath $output) {
        Remove-Item -LiteralPath $output -Force
    }
}

$nsisScript = Join-Path `
    $projectRoot `
    "packaging\windows\nsis\PalVerify.nsi"
$installerIcon = Join-Path `
    $projectRoot `
    "resources\launcher\pal3mien.ico"
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
    & $signTool.FullName sign `
        /sha1 $certificate.Thumbprint `
        /fd SHA256 `
        /td SHA256 `
        /tr "http://timestamp.digicert.com" `
        $nsisOutput
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode signing failed: $nsisOutput"
    }
    "AUTHENTICODE_SIGNED subject=$($certificate.Subject)"
} else {
    "AUTHENTICODE_UNSIGNED reason=no-valid-code-signing-certificate"
}

$installerHash = (
    Get-FileHash -LiteralPath $nsisOutput -Algorithm SHA256
).Hash
"$installerHash  $([System.IO.Path]::GetFileName($nsisOutput))" |
    Set-Content -LiteralPath $hashManifest -Encoding utf8

"NSIS_INSTALLER=$nsisOutput"
"NSIS_SHA256=$installerHash"
"INSTALLER_HASHES=$hashManifest"
