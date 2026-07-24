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
$serverAgent = Join-Path $buildDirectory "Release\PalVerifyServer.exe"
$coordinator = Join-Path `
    $buildDirectory `
    "palverify-coordinator-linux-amd64"

Push-Location $projectRoot
try {
    go test ./...
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

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

go build -trimpath -o $serverAgent `
    ./cmd/palverify-server
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$previousGoos = $env:GOOS
$previousGoarch = $env:GOARCH
$previousCgo = $env:CGO_ENABLED
try {
    $env:GOOS = "linux"
    $env:GOARCH = "amd64"
    $env:CGO_ENABLED = "0"
    go build -trimpath -o $coordinator `
        ./cmd/palverify-coordinator
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    $env:GOOS = $previousGoos
    $env:GOARCH = $previousGoarch
    $env:CGO_ENABLED = $previousCgo
}

npm test
exit $LASTEXITCODE
