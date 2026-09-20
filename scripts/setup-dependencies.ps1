[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $repoRoot ".deps"
$vcpkgRoot = Join-Path $depsRoot "vcpkg"
$vcpkgCommit = "04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4"
$vcpkgRepository = "https://github.com/microsoft/vcpkg.git"

New-Item -ItemType Directory -Path $depsRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $vcpkgRoot ".git"))) {
    if (Test-Path -LiteralPath $vcpkgRoot) {
        throw "The dependency folder exists but is not a vcpkg checkout: $vcpkgRoot"
    }

    New-Item -ItemType Directory -Path $vcpkgRoot | Out-Null
    & git -C $vcpkgRoot init
    if ($LASTEXITCODE -ne 0) { throw "Unable to initialize vcpkg." }
    & git -C $vcpkgRoot remote add origin $vcpkgRepository
    if ($LASTEXITCODE -ne 0) { throw "Unable to configure the vcpkg repository." }
}

$origin = (& git -C $vcpkgRoot remote get-url origin).Trim()
if ($LASTEXITCODE -ne 0 -or $origin -ne $vcpkgRepository) {
    throw "Refusing to modify an unexpected repository in $vcpkgRoot"
}

& git -C $vcpkgRoot fetch --depth 1 origin $vcpkgCommit
if ($LASTEXITCODE -ne 0) { throw "Unable to download the pinned vcpkg revision." }
& git -C $vcpkgRoot checkout --force --detach $vcpkgCommit
if ($LASTEXITCODE -ne 0) { throw "Unable to select the pinned vcpkg revision." }

& (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
if ($LASTEXITCODE -ne 0) { throw "Unable to bootstrap vcpkg." }
$packages = @(
    "libusb",
    "libpcap",
    "libsodium",
    "ffmpeg[openssl,srt]",
    "qt5-base",
    "qt5-declarative",
    "qt5-multimedia",
    "qt5-quickcontrols2",
    "sdl2",
    "vcpkg-tool-ninja"
)

$buildVcpkgRoot = $vcpkgRoot
$temporaryJunction = $null

if ($repoRoot -match "\s") {
    $driveRoot = [IO.Path]::GetPathRoot($repoRoot)
    $temporaryJunction = Join-Path $driveRoot "fpv4win-deps-build"
    if (Test-Path -LiteralPath $temporaryJunction) {
        throw "The temporary dependency build path already exists: $temporaryJunction"
    }
    New-Item -ItemType Junction -Path $temporaryJunction -Target $repoRoot | Out-Null
    $buildVcpkgRoot = Join-Path $temporaryJunction ".deps\vcpkg"
}

$installExitCode = 1
try {
    # Remove only work trees left incomplete by previous path-related failures.
    foreach ($portName in @("ffmpeg", "harfbuzz")) {
        $failedBuildTree = Join-Path $buildVcpkgRoot "buildtrees\$portName"
        if (Test-Path -LiteralPath $failedBuildTree) {
            Remove-Item -LiteralPath $failedBuildTree -Recurse -Force
        }
    }
    & (Join-Path $buildVcpkgRoot "vcpkg.exe") install @packages --triplet x64-windows --clean-after-build
    $installExitCode = $LASTEXITCODE
} finally {
    if ($temporaryJunction -and (Test-Path -LiteralPath $temporaryJunction)) {
        # Directory.Delete removes the junction itself without following its target.
        [IO.Directory]::Delete($temporaryJunction)
    }
}
if ($installExitCode -ne 0) { throw "One or more dependencies failed to install." }

Write-Host "Dependencies are ready in $depsRoot"
Write-Host "Build with: ./scripts/build-windows.ps1"
