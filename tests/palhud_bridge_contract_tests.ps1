$ErrorActionPreference = "Stop"

$sourcePath = Join-Path $PSScriptRoot "..\packaging\palhud_bridge\Scripts\main.lua"
if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
    throw "PalHudBridge source is missing: $sourcePath"
}

$source = Get-Content -Raw -Encoding utf8 -LiteralPath $sourcePath
$forbiddenPatterns = @(
    "FindFirstOf",
    "StaticConstructObject",
    "/Script/UMG",
    "IsLocalPlayerController",
    "RegisterHook",
    "RegisterKeyBind",
    ":[Gg]et\s*\(",
    "\.[Gg]et\s*\("
)

foreach ($pattern in $forbiddenPatterns) {
    if ($source -match $pattern) {
        throw "PalHudBridge server-only contract contains forbidden pattern: $pattern"
    }
}

Write-Host "PASS PalHudBridge source exists"
Write-Host "PASS PalHudBridge excludes client-only APIs and UObject unwraps"
