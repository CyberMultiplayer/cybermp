# Smoke test for the transport: handshake, ping/pong, version refusal, and the
# server going away. Exercises the protocol without launching the game.
#
#   .\tools\nettest.ps1

[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [uint16]$Port = 11780,
    [int]$Rounds = 3
)

$ErrorActionPreference = 'Stop'

# We deliberately expect non-zero exit codes from the client. On PowerShell 7.4+
# native failures are turned into terminating errors by default, which would abort
# the script on a result we asked for.
$PSNativeCommandUseErrorActionPreference = $false

$root = Split-Path $PSScriptRoot -Parent
$server = Join-Path $root "build\src\server\$Config\cybermp_server.exe"
$client = Join-Path $root "build\src\nettest\$Config\cybermp_nettest.exe"

foreach ($exe in @($server, $client)) {
    if (-not (Test-Path $exe)) { throw "Not built: $exe" }
}

Write-Host "pwsh $($PSVersionTable.PSVersion), port $Port"

$log = Join-Path $env:TEMP 'cybermp_nettest_server.log'
$process = Start-Process -FilePath $server -ArgumentList $Port -PassThru -NoNewWindow -RedirectStandardOutput $log
$failed = 0

function Invoke-Client {
    param([string[]]$Arguments)

    # Out-Host keeps the client's stdout off the pipeline. A PowerShell function
    # returns everything written to the output stream, not just what `return` names,
    # so without this the caller gets the whole transcript plus the exit code.
    & $client @Arguments | Out-Host
    return $LASTEXITCODE
}

try {
    # Poll instead of sleeping a fixed amount: a cold CI runner is much slower than
    # a local machine, and a fixed wait is exactly how this kind of test goes flaky.
    $ready = $false
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        Start-Sleep -Milliseconds 300
        if ((Invoke-Client @($Port, 1)) -eq 0) { $ready = $true; break }
        Write-Host "  waiting for server, attempt $attempt"
    }

    if (-not $ready) {
        Write-Host 'FAIL: server never answered'
        $failed++
    }

    Write-Host ''
    Write-Host '--- server up: expecting replies ---'
    $code = Invoke-Client @($Port, $Rounds)
    if ($code -ne 0) { Write-Host "FAIL: expected exit 0, got $code"; $failed++ }

    Write-Host ''
    Write-Host '--- wrong protocol version: expecting refusal ---'
    $code = Invoke-Client @($Port, 1, 999)
    if ($code -ne 2) { Write-Host "FAIL: expected exit 2 (refused), got $code"; $failed++ }

    Stop-Process -Id $process.Id -Force
    Start-Sleep -Milliseconds 500

    Write-Host ''
    Write-Host '--- server down: expecting timeout ---'
    $code = Invoke-Client @($Port, 1)
    if ($code -eq 0) { Write-Host 'FAIL: got a reply with no server running'; $failed++ }
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
