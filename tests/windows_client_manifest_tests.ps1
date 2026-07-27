param(
    [Parameter(Mandatory = $true)]
    [string]$ClientPath
)

$ErrorActionPreference = "Stop"

$manifestTool = Get-ChildItem `
    "C:\Program Files (x86)\Windows Kits\10\bin" `
    -Filter mt.exe `
    -Recurse `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\mt\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $manifestTool) {
    throw "Windows SDK manifest tool was not found."
}

$resolvedClient = [System.IO.Path]::GetFullPath($ClientPath)
if (-not (Test-Path -LiteralPath $resolvedClient)) {
    throw "PalVerifyClient artifact was not found: $resolvedClient"
}

$temporaryManifest = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    "palverify-client-manifest-$([guid]::NewGuid().ToString('N')).xml"
try {
    & $manifestTool.FullName `
        "-inputresource:$resolvedClient;#1" `
        "-out:$temporaryManifest"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not extract the PalVerifyClient manifest."
    }

    $manifest = Get-Content `
        -Raw `
        -Encoding utf8 `
        -LiteralPath $temporaryManifest
    if ($manifest -notmatch 'name="Microsoft\.Windows\.Common-Controls"') {
        throw "PalVerifyClient does not activate Common Controls v6."
    }
    if ($manifest -notmatch 'version="6\.0\.0\.0"') {
        throw "PalVerifyClient Common Controls dependency has the wrong version."
    }
} finally {
    if (Test-Path -LiteralPath $temporaryManifest) {
        [System.IO.File]::Delete($temporaryManifest)
    }
}

"PASS PalVerifyClient activates Common Controls v6"
