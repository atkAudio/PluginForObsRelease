<#
.SYNOPSIS
    Sets up git hooks for the project

.DESCRIPTION
    Copies the pre-commit hook to .git/hooks/ directory.
    The pre-commit hook runs clang-format on C/C++ files and gersemi on CMake files.

.EXAMPLE
    .\setup-hooks.ps1
#>

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$GitHooksDir = Join-Path $RepoRoot ".git\hooks"
$SourceHook = Join-Path $ScriptDir "pre-commit"
$DestHook = Join-Path $GitHooksDir "pre-commit"

if (-not (Test-Path $GitHooksDir)) {
    Write-Error "Git hooks directory not found. Are you in a git repository?"
    exit 1
}

if (-not (Test-Path $SourceHook)) {
    Write-Error "Source pre-commit hook not found at: $SourceHook"
    exit 1
}

Copy-Item -Path $SourceHook -Destination $DestHook -Force
Write-Host "Pre-commit hook installed successfully!" -ForegroundColor Green

# Check for required tools
Write-Host "`nChecking for required tools..." -ForegroundColor Cyan

$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
if ($clangFormat) {
    $version = & clang-format --version
    Write-Host "  clang-format: $version" -ForegroundColor Green
}
else {
    Write-Host "  clang-format: NOT FOUND" -ForegroundColor Yellow
    Write-Host "    Install LLVM or add clang-format to PATH" -ForegroundColor Yellow
}

$gersemi = Get-Command gersemi -ErrorAction SilentlyContinue
if ($gersemi) {
    $version = & gersemi --version
    Write-Host "  gersemi: $version" -ForegroundColor Green
}
else {
    Write-Host "  gersemi: NOT FOUND" -ForegroundColor Yellow
    Write-Host "    Install with: pip install gersemi" -ForegroundColor Yellow
}
