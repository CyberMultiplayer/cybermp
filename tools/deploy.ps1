# Deploys the plugin, waiting for the game to close first -- it locks the dll.
# Verifies the copy by hash, because a stale dll looks exactly like a missing method.
#
#   .\tools\deploy.ps1
#   .\tools\deploy.ps1 -TimeoutSeconds 60

[CmdletBinding()]
param(
    [string]$GameDir = 'D:\Cyberpunk 2077',
    [string]$Config = 'Release',
    [int]$TimeoutSeconds = 1200
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$source = Join-Path $root "build\src\client\$Config\cybermp.dll"
$targetDir = Join-Path $GameDir 'red4ext\plugins\cybermp'
$target = Join-Path $targetDir 'cybermp.dll'

if (-not (Test-Path $source)) {
    throw "Not built yet: $source"
}

$waited = 0
while (Get-Process Cyberpunk2077 -ErrorAction SilentlyContinue) {
    if ($waited -eq 0) { Write-Host 'Game is running, waiting for it to close...' }
    if ($waited -ge $TimeoutSeconds) { throw "Game still running after ${TimeoutSeconds}s" }

    Start-Sleep -Seconds 3
    $waited += 3
}

# The game keeps the handle briefly after the process is gone.
if ($waited -gt 0) { Start-Sleep -Seconds 2 }

if (-not (Test-Path $targetDir)) {
    New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
}

Copy-Item $source $target -Force

if ((Get-FileHash $source).Hash -ne (Get-FileHash $target).Hash) {
    throw 'Copy does not match the build'
}

Write-Host ("Deployed {0:N1} KB -> {1}" -f ((Get-Item $target).Length / 1KB), $target)
