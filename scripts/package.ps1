$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build\Release"
$distRoot = Join-Path $projectRoot "dist"
$releaseName = "PalVerify_Windows_v0.2.0"
$releaseRoot = Join-Path $distRoot $releaseName
$payloadRoot = Join-Path $releaseRoot "payload"
$serverStage = Join-Path $distRoot "PalVerify_Server_Observation_v0.2.0"

$launcher = Join-Path $buildRoot "PalVerifyLauncher.exe"
$clientAgent = Join-Path $buildRoot "PalVerifyClient.exe"
foreach ($required in @($launcher, $clientAgent)) {
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

New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null
Copy-Item -LiteralPath $launcher -Destination $releaseRoot
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "release\README.txt") `
    -Destination $releaseRoot
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "packaging\Info.json") `
    -Destination $payloadRoot
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "packaging\thumbnail.png") `
    -Destination $payloadRoot
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "packaging\client") `
    -Destination $payloadRoot `
    -Recurse
Copy-Item `
    -LiteralPath $clientAgent `
    -Destination (Join-Path $payloadRoot "client\Scripts")

New-Item -ItemType Directory -Path $serverStage -Force | Out-Null
Copy-Item `
    -LiteralPath (Join-Path $projectRoot "packaging\server") `
    -Destination (Join-Path $serverStage "PalVerify") `
    -Recurse

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
    foreach ($executable in @(
        (Join-Path $releaseRoot "PalVerifyLauncher.exe"),
        (Join-Path $payloadRoot "client\Scripts\PalVerifyClient.exe")
    )) {
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
    "AUTHENTICODE_SIGNED subject=$($certificate.Subject)"
} else {
    "AUTHENTICODE_UNSIGNED reason=no-valid-code-signing-certificate"
}

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
