<#
.SYNOPSIS
    Install the CEF renderer for Wick on Windows (x64).

.DESCRIPTION
    Windows counterpart of Wick's scripts/install-cef-mac.sh and
    scripts/install-cef-linux.sh. Fetches
    the CEF (Chromium Embedded Framework) runtime, builds wick-renderer.exe and
    wick-helper.exe from the renderer sources (see -SourceDir) with MSVC, and
    installs them to
    %USERPROFILE%\.wick\cef, where wick looks for the renderer. Required once
    per machine to enable JavaScript rendering.

    Needs Visual Studio 2022 (or Build Tools) with the C++ x64 tools and a
    Windows 10/11 SDK. No administrator rights are required.

.PARAMETER InstallDir
    Target directory. Defaults to <home>\.wick\cef (HOME if set, otherwise
    USERPROFILE), matching the lookup in Wick's rust\src\cef.rs (with the
    Windows patch applied).

.PARAMETER SourceDir
    The directory holding renderer_win.c and build-windows.ps1: the rust\cef
    directory of a wick checkout, or the directory of a standalone copy of
    the renderer. Defaults to WICK_SOURCE_DIR (a wick checkout root), then
    this script's own directory, then the checkout this script lives in.

.PARAMETER SdkRoot
    Where the CEF distribution is kept between runs. Defaults to SourceDir.

.PARAMETER Force
    Rebuild and reinstall even when an up-to-date renderer is installed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\install-cef-windows.ps1
#>
[CmdletBinding()]
param(
    [string]$InstallDir = '',
    [string]$SourceDir = '',
    [string]$SdkRoot = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# Bumped together with WICK_RENDERER_PROTOCOL in the renderer sources: an
# installed renderer that reports another protocol is rebuilt.
$RendererProtocol = 'v3-cf-aware'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $InstallDir) {
    $homeDir = $env:HOME
    if (-not $homeDir) { $homeDir = $env:USERPROFILE }
    if (-not $homeDir) { throw 'Cannot determine the home directory (HOME and USERPROFILE are unset).' }
    $InstallDir = Join-Path $homeDir '.wick\cef'
}

if (-not $SourceDir) {
    $candidates = @()
    if ($env:WICK_SOURCE_DIR) { $candidates += (Join-Path $env:WICK_SOURCE_DIR 'rust\cef') }
    $candidates += $ScriptDir
    $candidates += (Join-Path $ScriptDir '..\rust\cef')
    $SourceDir = $candidates | Where-Object { Test-Path (Join-Path $_ 'renderer_win.c') } | Select-Object -First 1
    if (-not $SourceDir) {
        throw 'The renderer sources (renderer_win.c) were not found next to this script or in a wick checkout. Pass -SourceDir.'
    }
}
$SourceDir = (Resolve-Path $SourceDir).Path
$buildScript = Join-Path $SourceDir 'build-windows.ps1'
if (-not (Test-Path $buildScript)) { throw "build-windows.ps1 not found in $SourceDir" }

$renderer = Join-Path $InstallDir 'wick-renderer.exe'

if ((Test-Path $renderer) -and -not $Force) {
    $installed = ''
    try { $installed = (& $renderer --version) -join ' ' } catch { $installed = '' }
    if ($installed -match [regex]::Escape("WICK_RENDERER_PROTOCOL=$RendererProtocol")) {
        Write-Host "CEF renderer already installed: $installed"
        Write-Host "  $InstallDir"
        Write-Host 'Use -Force to rebuild.'
        exit 0
    }
    Write-Host "Installed wick-renderer is from a pre-$RendererProtocol build. Rebuilding..."
}

# Files of a running renderer cannot be replaced.
$running = Get-Process wick-renderer, wick-helper -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    throw "wick-renderer is running from $InstallDir. Stop wick (or the application hosting it) and run this script again."
}

Write-Host "Installing the Wick CEF renderer to $InstallDir..."
Write-Host ''

$buildArguments = @{ InstallDir = $InstallDir }
if ($SdkRoot) { $buildArguments.SdkRoot = $SdkRoot }
& $buildScript @buildArguments

$installed = (& $renderer --version) -join ' '
if ($LASTEXITCODE -ne 0 -or $installed -notmatch [regex]::Escape("WICK_RENDERER_PROTOCOL=$RendererProtocol")) {
    throw "The installed renderer did not report protocol ${RendererProtocol}: $installed"
}

Write-Host ''
Write-Host '=== Wick CEF renderer installed ==='
Write-Host "  Renderer: $renderer"
Write-Host "  $installed"
Write-Host ''
Write-Host 'Test it:  wick fetch https://example.com --render cef'
Write-Host ''
Write-Host 'If a per-application firewall is active, the first render asks for'
Write-Host "network access for $(Join-Path $InstallDir 'wick-helper.exe') (Chromium's network process)."
