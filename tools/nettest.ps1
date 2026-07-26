# Smoke test for the transport: starts the server, pings it, then kills it and
# checks the client reports timeouts instead of hanging or crashing.
#
#   .\tools\nettest.ps1

[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [uint16]$Port = 11780,
    [int]$Rounds = 3
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
$server = Join-Path $root "build\src\server\$Config\cybermp_server.exe"
$client = Join-Path $root "build\src\nettest\$Config\cybermp_nettest.exe"

foreach ($exe in @($server, $client)) {
    if (-not (Test-Path $exe)) { throw "Not built: $exe" }
}

$log = Join-Path $env:TEMP 'cybermp_nettest_server.log'
$process = Start-Process -FilePath $server -ArgumentList $Port -PassThru -NoNewWindow -RedirectStandardOutput $log
$failed = 0

try {
    Start-Sleep -Milliseconds 700

    Write-Host '--- server up: expecting replies ---'
    & $client $Port $Rounds
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: server was up but rounds went unanswered'; $failed++ }

    Write-Host ''
    Write-Host '--- wrong protocol version: expecting refusal ---'
    & $client $Port 1 999
    if ($LASTEXITCODE -ne 2) { Write-Host "FAIL: expected exit 2 (refused), got $LASTEXITCODE"; $failed++ }

    Stop-Process -Id $process.Id -Force
    Start-Sleep -Milliseconds 500

    Write-Host ''
    Write-Host '--- server down: expecting timeouts ---'
    & $client $Port 1
    if ($LASTEXITCODE -eq 0) { Write-Host 'FAIL: got a reply with no server running'; $failed++ }
}
finally {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host '--- server output ---'
if (Test-Path $log) { Get-Content $log }

if ($failed -gt 0) { throw "$failed check(s) failed" }

Write-Host ''
Write-Host 'nettest ok'
