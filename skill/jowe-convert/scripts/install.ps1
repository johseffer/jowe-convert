# Installs jvconvert v0.3.1 on Windows x64.
# Downloads one file from the jowe-convert GitHub release, checks its SHA-256 and
# copies jvconvert.exe into a folder (default: %LOCALAPPDATA%\Programs\jvconvert).
#
# Usage: powershell -ExecutionPolicy Bypass -File install.ps1 [-Yes] [-Dest <folder>]
param(
    [switch]$Yes,
    [string]$Dest = (Join-Path $env:LOCALAPPDATA 'Programs\jvconvert')
)
$ErrorActionPreference = 'Stop'

$Version = 'v0.3.1'
$Asset = "jvconvert-$Version-windows-x64.zip"
$Url = "https://github.com/johseffer/jowe-convert/releases/download/$Version/$Asset"
$Sha = 'a1c055a15645fdfea952e32e0f4e0cf925b90d020911286342b7264f0d66cc2a'

if (-not [Environment]::Is64BitOperatingSystem) { throw 'jvconvert needs 64-bit Windows.' }

Write-Host "Download: $Url"
Write-Host "SHA-256:  $Sha"
Write-Host "Install:  $Dest\jvconvert.exe"
if (-not $Yes) {
    $answer = Read-Host 'Continue? [y/N]'
    if ($answer -notmatch '^(y|yes)$') { Write-Host 'Cancelled.'; exit 1 }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("jvconvert-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $zip = Join-Path $tmp $Asset
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing

    $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($got -ne $Sha) { throw "SHA-256 mismatch: expected $Sha, got $got. Nothing was installed." }
    Write-Host 'SHA-256 OK'

    Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp 'x') -Force
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Copy-Item (Join-Path $tmp 'x\jvconvert.exe') (Join-Path $Dest 'jvconvert.exe') -Force
    Copy-Item (Join-Path $tmp 'x\LICENSE') (Join-Path $Dest 'LICENSE') -Force
    Unblock-File (Join-Path $Dest 'jvconvert.exe')
} finally {
    Remove-Item -Recurse -Force $tmp
}

$exe = Join-Path $Dest 'jvconvert.exe'
& $exe --version
Write-Host ''
Write-Host "Installed $exe"
Write-Host ''
Write-Host 'Add it to Claude Code (pick the folders it may read and write):'
Write-Host "  claude mcp add jvconvert -- `"$exe`" mcp --allow `"$env:USERPROFILE\Videos`""
Write-Host ''
Write-Host 'Check what this machine can convert:'
Write-Host "  & `"$exe`" formats"
