$ErrorActionPreference = "Continue"

$projectRoot = Split-Path -Parent $PSScriptRoot
$fengari = Join-Path $projectRoot "node_modules\.bin\fengari.cmd"
$probe = Join-Path $PSScriptRoot "autojoin_probe_tests.lua"
$output = & $fengari $probe 2>&1
$exitCode = $LASTEXITCODE
$rendered = ($output | Out-String)
$rendered

if ($exitCode -ne 0 -or -not $rendered.Contains(
    "PASS Pal3Mien AutoJoin probe"
)) {
    throw "Pal3Mien AutoJoin probe failed."
}
