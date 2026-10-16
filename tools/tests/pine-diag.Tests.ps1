#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$collector = Join-Path $PSScriptRoot '../pine-diag.ps1'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('pine-adb-test-' + [guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($temp)
$env:PINE_FAKE_ADB_ARGS = Join-Path $temp 'args.txt'
$env:PINE_FAKE_ADB_PID = Join-Path $temp 'pid.txt'
$isWindowsHost = $env:OS -eq 'Windows_NT'
if ($isWindowsHost) {
    $fake = Join-Path $temp 'adb.exe'
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $compiler /nologo /target:exe "/out:$fake" (Join-Path $PSScriptRoot 'fake-adb.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Fake ADB compilation failed' }
} else {
    $fake = Join-Path $temp 'adb'
    Copy-Item (Join-Path $PSScriptRoot 'fake-adb.py') $fake
    & chmod +x $fake
}
function Assert([bool]$condition, [string]$message) { if (-not $condition) { throw $message } }
function Expect-Failure([string]$mode, [string]$expected) {
    $env:PINE_FAKE_ADB_MODE = $mode
    $caught = $false
    try { & $collector -AdbPath $fake -NonInteractive -OutputDirectory $temp -DurationSeconds 1 | Out-Null }
    catch { $caught = $_.ToString().Contains($expected) }
    Assert $caught "Expected failure for $mode containing $expected"
}
try {
    Expect-Failure 'none' 'No ADB devices'
    Expect-Failure 'multiple' 'Multiple ADB devices'
    Expect-Failure 'unauthorized' 'unauthorized'
    Expect-Failure 'offline' 'not ready'
    Expect-Failure 'disconnect' 'stream ended'
    $env:PINE_FAKE_ADB_MODE = 'multiple'
    $out = Join-Path $temp 'filtered'
    & $collector -AdbPath $fake -Serial second-device -SessionId session-a -TitleId 0100 -OutputDirectory $out -DurationSeconds 1 | Out-Null
    $files = @(Get-ChildItem $out -Filter '*.jsonl')
    Assert ($files.Count -eq 1) 'Session/title filtering should produce one file'
    $event = [IO.File]::ReadAllText($files[0].FullName, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $expectedUnicode = -join ([char[]]@(0x4e2d, 0x6587, 0x20, 0x65e5, 0x5fd7))
    Assert ($event.message -eq $expectedUnicode) 'UTF8 did not round trip'
    Assert ($files[0].Name -match 'session-a') 'Output filename lacks session'
    $arguments = [IO.File]::ReadAllText($env:PINE_FAKE_ADB_ARGS)
    Assert ($arguments.Contains('-s second-device logcat -v raw PineDiag:V *:S')) 'Wrong device/tag'
    Assert (-not ($arguments -match 'logcat.* -c')) 'Collector must not clear device logs'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Timed collection leaked ADB'

    # PowerShell pipeline cancellation follows the Ctrl+C stop/finally path.
    # This checks engine cancellation; console keyboard delivery is a separate manual check.
    Remove-Item $env:PINE_FAKE_ADB_PID
    $env:PINE_FAKE_ADB_MODE = 'normal'
    $runspace = [PowerShell]::Create()
    [void]$runspace.AddCommand($collector).AddParameter('AdbPath', $fake).AddParameter('NonInteractive').AddParameter('OutputDirectory', (Join-Path $temp 'cancel'))
    $pending = $runspace.BeginInvoke()
    $limit = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path $env:PINE_FAKE_ADB_PID) -and [DateTime]::UtcNow -lt $limit) { Start-Sleep -Milliseconds 50 }
    Assert (Test-Path $env:PINE_FAKE_ADB_PID) 'Cancellation fixture never started'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    $runspace.Stop()
    $runspace.Dispose()
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Cancellation leaked ADB'
    Write-Host 'PASS: no-device, multi-device, unauthorized, offline, disconnect, filters, UTF8, retained logs, deadline and pipeline cancellation cleanup'
} finally {
    Remove-Item $temp -Recurse -Force
    Remove-Item Env:PINE_FAKE_ADB_ARGS, Env:PINE_FAKE_ADB_PID, Env:PINE_FAKE_ADB_MODE -ErrorAction SilentlyContinue
}
