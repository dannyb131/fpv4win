[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd("\")
$depsRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot ".deps")).TrimEnd("\")
$depsParent = [IO.Directory]::GetParent($depsRoot).FullName.TrimEnd("\")

if ($depsParent -ne $repoRoot) {
    throw "Refusing to remove an unexpected path: $depsRoot"
}
if (-not (Test-Path -LiteralPath $depsRoot)) {
    Write-Host "No project-local dependencies are installed."
    exit 0
}

if (-not $Force) {
    $answer = Read-Host "Remove all fpv4win dependencies from $depsRoot? [y/N]"
    if ($answer -notin @("y", "Y", "yes", "YES")) {
        Write-Host "Cancelled."
        exit 0
    }
}

Remove-Item -LiteralPath $depsRoot -Recurse -Force
Write-Host "Removed all project-local fpv4win dependencies."
