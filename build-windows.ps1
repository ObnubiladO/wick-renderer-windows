<#
.SYNOPSIS
    Build the CEF renderer and helper for Windows (x64).

.DESCRIPTION
    Windows counterpart of Wick's rust/cef/build-linux.sh. Downloads the CEF binary
    distribution when it is not already present, compiles wick-renderer.exe and
    wick-helper.exe with MSVC, and stages them together with the CEF runtime
    into one runnable directory.

    Requirements: Visual Studio 2022 (or Build Tools) with the C++ x64 tools
    and a Windows 10/11 SDK. curl.exe and tar.exe ship with Windows 10+.

.PARAMETER SdkRoot
    Directory that holds (or receives) the extracted CEF distribution.
    Defaults to this script's directory, like Wick's build-linux.sh.

.PARAMETER OutDir
    Staging directory for the runnable renderer. Defaults to out\windows64.

.PARAMETER InstallDir
    Optional. Copy the staged renderer there as well (for example
    %USERPROFILE%\.wick\cef, where wick looks for it).

.PARAMETER Locales
    Locale packs to stage. Default "all" (as on Linux); pass e.g. en-US to
    stage only the packs you need.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File build-windows.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File build-windows.ps1 -InstallDir "$env:USERPROFILE\.wick\cef"
#>
[CmdletBinding()]
param(
    [string]$SdkRoot = '',
    [string]$OutDir = '',
    [string]$InstallDir = '',
    [string[]]$Locales = @('all')
)

$ErrorActionPreference = 'Stop'

# Resolved here rather than in the param block: Windows PowerShell 5.1 leaves
# $PSScriptRoot empty while it evaluates parameter defaults.
$SourceDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $SdkRoot) { $SdkRoot = $SourceDir }
if (-not $OutDir) { $OutDir = Join-Path $SourceDir 'out\windows64' }

# Keep in sync with Wick's rust/cef/build-linux.sh and scripts/install-cef-*.sh.
$CefVersion = '144.0.18+gc5b2ec2+chromium-144.0.7559.246'
$CefApiVersion = '14400'
$CefPlatform = 'windows64'
$CefName = "cef_binary_${CefVersion}_${CefPlatform}_minimal"
$CefUrl = 'https://cef-builds.spotifycdn.com/' + [uri]::EscapeDataString("$CefName.tar.bz2")

function Write-Step([string]$Message) { Write-Host "==> $Message" }

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# -- MSVC environment ---------------------------------------------------------

function Import-MsvcEnvironment {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and $env:INCLUDE -and $env:LIB -and
        ($env:VSCMD_ARG_TGT_ARCH -eq 'x64')) {
        return
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio was not found. Install "Build Tools for Visual Studio 2022" with the "Desktop development with C++" workload.'
    }
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if (-not $install) {
        throw 'No Visual Studio installation with the C++ x64 build tools was found.'
    }
    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $install" }

    $lines = & cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat failed to configure the build environment.' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("env:" + $Matches[1]) -Value $Matches[2]
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'cl.exe is still unavailable after loading the Visual Studio environment.'
    }
}

# -- CEF distribution ---------------------------------------------------------

function Get-CefSdk {
    $sdk = Join-Path $SdkRoot $CefName
    if (Test-Path (Join-Path $sdk 'Release\libcef.dll')) { return $sdk }

    New-Item -ItemType Directory -Force $SdkRoot | Out-Null
    $archive = Join-Path $SdkRoot 'cef_windows64_minimal.tar.bz2'
    $expected = (& curl.exe -fsSL --retry 3 "$CefUrl.sha1")
    if ($LASTEXITCODE -ne 0 -or -not $expected) { throw 'Could not fetch the CEF checksum.' }
    $expected = $expected.Trim().ToLower()

    $haveArchive = (Test-Path $archive) -and
        ((Get-FileHash $archive -Algorithm SHA1).Hash.ToLower() -eq $expected)
    if (-not $haveArchive) {
        Write-Step 'Downloading CEF for Windows (~150 MB)...'
        Invoke-Native 'curl.exe' @('-fL', '--retry', '3', '--progress-bar', '-o', $archive, $CefUrl)
        $actual = (Get-FileHash $archive -Algorithm SHA1).Hash.ToLower()
        if ($actual -ne $expected) {
            Remove-Item $archive -Force
            throw "CEF download is corrupt: SHA-1 $actual, expected $expected."
        }
    }

    Write-Step 'Extracting CEF...'
    Invoke-Native 'tar.exe' @('-xjf', $archive, '-C', $SdkRoot)
    if (-not (Test-Path (Join-Path $sdk 'Release\libcef.dll'))) {
        throw "CEF archive did not contain $CefName."
    }
    Remove-Item $archive -Force
    return $sdk
}

# -- Build --------------------------------------------------------------------

function Build-Binary([string]$Sdk, [string]$Source, [string]$Output, [string]$Subsystem, [string[]]$ExtraLibs) {
    $objDir = Join-Path $OutDir 'obj'
    New-Item -ItemType Directory -Force $objDir | Out-Null
    # A full object-file path: a directory form would end in a backslash,
    # which breaks argument quoting when the path contains spaces.
    $object = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($Source) + '.obj')
    $arguments = @(
        '/nologo', '/std:c17', '/O2', '/MT', '/W3', '/utf-8',
        '/D_CRT_SECURE_NO_WARNINGS', "/DCEF_API_VERSION=$CefApiVersion",
        "/I$Sdk", "/I$SourceDir",
        "/Fo$object",
        "/Fe$Output",
        (Join-Path $SourceDir $Source),
        '/link',
        "/LIBPATH:$(Join-Path $Sdk 'Release')", 'libcef.lib'
    ) + $ExtraLibs + @(
        "/SUBSYSTEM:$Subsystem",
        '/MANIFEST:EMBED', "/MANIFESTINPUT:$(Join-Path $SourceDir 'wick-cef.manifest')",
        '/DYNAMICBASE', '/NXCOMPAT'
    )
    Invoke-Native 'cl.exe' $arguments
}

function Copy-Runtime([string]$Sdk, [string]$Target) {
    New-Item -ItemType Directory -Force $Target | Out-Null
    # Everything CEF loads at run time. Import libraries and the bootstrap
    # launchers are build-time / alternative-entry artefacts.
    Get-ChildItem (Join-Path $Sdk 'Release') -File |
        Where-Object { $_.Extension -ne '.lib' -and $_.Name -notlike 'bootstrap*.exe' } |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $Target $_.Name) -Force }
    Get-ChildItem (Join-Path $Sdk 'Resources') -File |
        ForEach-Object { Copy-Item $_.FullName (Join-Path $Target $_.Name) -Force }

    $localeTarget = Join-Path $Target 'locales'
    New-Item -ItemType Directory -Force $localeTarget | Out-Null
    $localeSource = Join-Path $Sdk 'Resources\locales'
    if ($Locales -contains 'all') {
        Copy-Item (Join-Path $localeSource '*') $localeTarget -Force
    } else {
        foreach ($locale in $Locales) {
            $packs = Get-ChildItem $localeSource -Filter "$locale*.pak"
            if (-not $packs) { throw "No locale pack matches '$locale'." }
            $packs | ForEach-Object { Copy-Item $_.FullName $localeTarget -Force }
        }
    }
    Copy-Item (Join-Path $Sdk 'LICENSE.txt') (Join-Path $Target 'CEF-LICENSE.txt') -Force
}

if (-not [Environment]::Is64BitOperatingSystem) { throw 'Only 64-bit Windows is supported.' }

Import-MsvcEnvironment
$sdk = Get-CefSdk
New-Item -ItemType Directory -Force $OutDir | Out-Null

Write-Step 'Building wick-renderer.exe...'
Build-Binary $sdk 'renderer_win.c' (Join-Path $OutDir 'wick-renderer.exe') 'CONSOLE' @('bcrypt.lib', 'advapi32.lib')

Write-Step 'Building wick-helper.exe...'
Build-Binary $sdk 'helper_win.c' (Join-Path $OutDir 'wick-helper.exe') 'WINDOWS' @()

Write-Step 'Staging the CEF runtime...'
Copy-Runtime $sdk $OutDir

$version = & (Join-Path $OutDir 'wick-renderer.exe') --version
if ($LASTEXITCODE -ne 0) { throw 'The built wick-renderer.exe does not start.' }
Write-Host ''
Write-Host "Built: $version"
Write-Host "  $OutDir"

if ($InstallDir) {
    Write-Step "Installing to $InstallDir..."
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    Get-ChildItem $OutDir -Force | Where-Object { $_.Name -ne 'obj' } |
        ForEach-Object { Copy-Item $_.FullName $InstallDir -Recurse -Force }
    Write-Host "Installed. wick looks for wick-renderer.exe next to wick.exe and in %USERPROFILE%\.wick\cef."
}
