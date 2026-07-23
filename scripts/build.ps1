$ErrorActionPreference = "Stop"

$visualStudioRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$cmake = Join-Path `
    $visualStudioRoot `
    "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if (-not (Test-Path -LiteralPath $cmake)) {
    throw "Visual Studio 2022 bundled CMake was not found: $cmake"
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $projectRoot "build"

& $cmake `
    -S $projectRoot `
    -B $buildDirectory `
    -G "Visual Studio 17 2022" `
    -A x64
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake `
    --build $buildDirectory `
    --config Release `
    --target RUN_TESTS
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

npm run test:probe
exit $LASTEXITCODE
