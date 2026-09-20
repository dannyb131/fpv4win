[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd("\")
$buildRoot = $repoRoot
$temporaryJunction = $null
$createdJunction = $false
$batchFile = $null

if ($repoRoot -match "\s") {
    $driveRoot = [IO.Path]::GetPathRoot($repoRoot)
    $temporaryJunction = Join-Path $driveRoot "fpv4win-build"

    if (Test-Path -LiteralPath $temporaryJunction) {
        $junction = Get-Item -LiteralPath $temporaryJunction -Force
        $target = [IO.Path]::GetFullPath([string]$junction.Target).TrimEnd("\")
        if ($junction.LinkType -ne "Junction" -or $target -ne $repoRoot) {
            throw "Refusing to use an unexpected path: $temporaryJunction"
        }
    } else {
        New-Item -ItemType Junction -Path $temporaryJunction -Target $repoRoot | Out-Null
        $createdJunction = $true
    }
    $buildRoot = $temporaryJunction
}

$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$vsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vsWhere)) {
    throw "Visual Studio Build Tools could not be located."
}
$vsInstall = (& $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$vsDevCmd = Join-Path $vsInstall "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path -LiteralPath $vsDevCmd)) {
    throw "The Visual C++ developer environment could not be located."
}

try {
    $batchFile = Join-Path ([IO.Path]::GetTempPath()) ("fpv4win-build-{0}.cmd" -f [guid]::NewGuid())
    [IO.File]::WriteAllLines($batchFile, @(
        "@echo off",
        "setlocal",
        "set `"PATH=`"",
        "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul",
        "if errorlevel 1 exit /b %errorlevel%",
        "`"$cmake`" --preset windows-release --fresh",
        "if errorlevel 1 exit /b %errorlevel%",
        "`"$cmake`" --build --preset windows-release",
        "exit /b %errorlevel%"
    ))

    Push-Location $buildRoot
    try {
        & $env:ComSpec /d /c $batchFile
        $buildExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
} finally {
    if ($batchFile -and (Test-Path -LiteralPath $batchFile)) {
        Remove-Item -LiteralPath $batchFile -Force
    }
    if ($createdJunction -and (Test-Path -LiteralPath $temporaryJunction)) {
        [IO.Directory]::Delete($temporaryJunction)
    }
}

if ($buildExitCode -ne 0) {
    throw "The fpv4win build failed."
}

Write-Host "Build complete: $(Join-Path $repoRoot 'build\Release\fpv4win.exe')"
