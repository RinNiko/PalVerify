$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build\Release"
$distRoot = Join-Path $projectRoot "dist"
$version = (Get-Content -Raw `
    -LiteralPath (Join-Path $projectRoot "package.json") |
    ConvertFrom-Json).version
$releaseName = "PalVerify_Windows_v$version"
$releaseRoot = Join-Path $distRoot $releaseName
$serverStage = Join-Path $distRoot "PalVerify_Server_v$version"

$launcher = Join-Path $buildRoot "Pal3Mien.exe"
$clientAgent = Join-Path $buildRoot "PalVerifyClient.exe"
$serverAgent = Join-Path $buildRoot "PalVerifyServer.exe"
foreach ($required in @($launcher, $clientAgent, $serverAgent)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing build artifact: $required"
    }
}

$resolvedProject = [System.IO.Path]::GetFullPath($projectRoot)
foreach ($target in @($releaseRoot, $serverStage)) {
    $resolvedTarget = [System.IO.Path]::GetFullPath($target)
    if (-not $resolvedTarget.StartsWith(
        $resolvedProject + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Refusing to clean path outside project: $resolvedTarget"
    }
    if (Test-Path -LiteralPath $resolvedTarget) {
        Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
    }
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
    foreach ($executable in @($clientAgent, $serverAgent)) {
        & $signTool.FullName sign `
            /sha1 $certificate.Thumbprint `
            /fd SHA256 `
            /td SHA256 `
            /tr "http://timestamp.digicert.com" `
            $executable
        if ($LASTEXITCODE -ne 0) {
            throw "Authenticode signing failed: $executable"
        }
    }
}

$cmake = Join-Path `
    "C:\Program Files\Microsoft Visual Studio\2022\Community" `
    "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "Visual Studio 2022 bundled CMake was not found: $cmake"
}
& $cmake `
    --build (Join-Path $projectRoot "build") `
    --config Release `
    --target Pal3Mien
if ($LASTEXITCODE -ne 0) {
    throw "Pal3Mien rebuild with embedded payload failed."
}

if ($certificate -and $signTool) {
    & $signTool.FullName sign `
        /sha1 $certificate.Thumbprint `
        /fd SHA256 `
        /td SHA256 `
        /tr "http://timestamp.digicert.com" `
        $launcher
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode signing failed: $launcher"
    }
    "AUTHENTICODE_SIGNED subject=$($certificate.Subject)"
} else {
    "AUTHENTICODE_UNSIGNED reason=no-valid-code-signing-certificate"
}

$packagePaths = @{
    "Info.json" = Join-Path $projectRoot "packaging\Info.json"
    "thumbnail.png" = Join-Path $projectRoot "packaging\thumbnail.png"
    "client/README.md" = Join-Path $projectRoot "packaging\client\README.md"
    "client/enabled.txt" = Join-Path $projectRoot "packaging\client\enabled.txt"
    "client/Scripts/config.json" = Join-Path `
        $projectRoot `
        "packaging\client\Scripts\config.json"
    "client/Scripts/main.lua" = Join-Path `
        $projectRoot `
        "packaging\client\Scripts\main.lua"
    "client/Scripts/PalVerifyClient.exe" = $clientAgent
}
$packageRelativePaths = [string[]]$packagePaths.Keys
[Array]::Sort($packageRelativePaths, [StringComparer]::Ordinal)
$packageHash = [Security.Cryptography.IncrementalHash]::CreateHash(
    [Security.Cryptography.HashAlgorithmName]::SHA256
)
foreach ($relativePath in $packageRelativePaths) {
    $fileDigest = (
        Get-FileHash `
            -LiteralPath $packagePaths[$relativePath] `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $packageHash.AppendData(
        [Text.Encoding]::UTF8.GetBytes($relativePath)
    )
    $packageHash.AppendData([byte[]](0))
    $packageHash.AppendData(
        [Text.Encoding]::ASCII.GetBytes($fileDigest)
    )
    $packageHash.AppendData([byte[]](0))
}
$packageDigest = (
    [BitConverter]::ToString($packageHash.GetHashAndReset()) -replace "-", ""
).ToLowerInvariant()
$releaseManifestPath = Join-Path `
    $projectRoot `
    "release\palverify-launcher-manifest.json"
$releaseManifest = Get-Content `
    -Raw `
    -Encoding utf8 `
    -LiteralPath $releaseManifestPath |
    ConvertFrom-Json
if ($releaseManifest.palVerifyPackageDigest -cne $packageDigest) {
    throw (
        "Release manifest palVerifyPackageDigest does not match payload. " +
        "expected=$packageDigest " +
        "actual=$($releaseManifest.palVerifyPackageDigest)"
    )
}
"PACKAGE_DIGEST=$packageDigest"

New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
Copy-Item -LiteralPath $launcher -Destination $releaseRoot
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "release\README.txt") `
    -Destination $releaseRoot

New-Item -ItemType Directory -Path $serverStage -Force | Out-Null
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "packaging\server") `
    -Destination (Join-Path $serverStage "PalVerify") `
    -Recurse
Copy-Item `
    -LiteralPath $serverAgent `
    -Destination (Join-Path $serverStage "PalVerify\Scripts")

$forbiddenReleaseFiles = @(
    Get-ChildItem -LiteralPath $releaseRoot -Recurse -Force |
        Where-Object {
            $_.Extension -ieq ".pdb" -or
            ($_.PSIsContainer -and $_.Name -ieq "payload")
        }
)
if ($forbiddenReleaseFiles.Count -ne 0) {
    throw (
        "Release layout contains forbidden payload/PDB paths: " +
        ($forbiddenReleaseFiles.FullName -join ", ")
    )
}
"RELEASE_LAYOUT_OK no-external-payload no-pdb"

$hashTargets = Get-ChildItem -LiteralPath $releaseRoot -File -Recurse |
    Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
    Sort-Object FullName
$hashLines = foreach ($file in $hashTargets) {
    $relative = $file.FullName.Substring(
        $releaseRoot.Length
    ).TrimStart("\", "/").Replace("\", "/")
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    "$hash  $relative"
}
$hashLines | Set-Content `
    -LiteralPath (Join-Path $releaseRoot "SHA256SUMS.txt") `
    -Encoding utf8

$clientZip = Join-Path $distRoot "$releaseName.zip"
$serverZip = "$serverStage.zip"
foreach ($archive in @($clientZip, $serverZip)) {
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
}
Compress-Archive -Path (Join-Path $releaseRoot "*") -DestinationPath $clientZip
Compress-Archive -Path (Join-Path $serverStage "*") -DestinationPath $serverZip

"CLIENT_RELEASE=$clientZip"
"CLIENT_SHA256=$((Get-FileHash -LiteralPath $clientZip -Algorithm SHA256).Hash)"
"SERVER_RELEASE=$serverZip"
"SERVER_SHA256=$((Get-FileHash -LiteralPath $serverZip -Algorithm SHA256).Hash)"
