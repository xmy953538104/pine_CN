#requires -Version 5.1
<#
.SYNOPSIS
Stream PineDiag JSONL over an already authorized USB or wireless ADB connection.
.EXAMPLE
.\tools\pine-diag.ps1 -Serial emulator-5554 -SessionId abc -TitleId 0100751007ADA000
#>
[CmdletBinding()]
param(
    [string]$AdbPath = 'adb',
    [string]$Serial,
    [string]$SessionId,
    [string]$TitleId,
    [string]$OutputDirectory = (Join-Path (Get-Location) 'pine-diagnostics'),
    [switch]$ListDevices,
    [switch]$NonInteractive,
    [ValidateRange(0, 86400)][int]$DurationSeconds = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$process = $null
$writer = $null
$writers = @{}

function Start-Adb([string[]]$Arguments) {
    # Arguments are controlled tokens. Reject characters that could alter argv quoting.
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"\x00-\x1f]') { throw 'Invalid ADB argument.' }
    }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $script:adbExecutable
    $info.Arguments = $Arguments -join ' '
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = $script:utf8
    $info.StandardErrorEncoding = $script:utf8
    $child = New-Object System.Diagnostics.Process
    $child.StartInfo = $info
    [void]$child.Start()
    return $child
}

try {
    $command = Get-Command $AdbPath -CommandType Application -ErrorAction SilentlyContinue
    if (-not $command) { throw 'ADB not found. Install official Android SDK Platform-Tools and put adb on PATH, or pass -AdbPath.' }
    $adbExecutable = $command.Source
    $process = Start-Adb @('devices', '-l')
    $deviceRead = $process.StandardOutput.ReadToEndAsync()
    $deviceError = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(10000)) { throw 'ADB device discovery timed out.' }
    if ($process.ExitCode -ne 0) { throw ('ADB discovery failed: ' + $deviceError.Result) }
    $devices = @($deviceRead.Result -split '\r?\n' | ForEach-Object {
        if ($_ -match '^(\S+)\s+(device|offline|unauthorized|recovery|sideload|bootloader)(?:\s|$)') {
            [pscustomobject]@{ Serial = $matches[1]; State = $matches[2] }
        }
    })
    $process.Dispose(); $process = $null
    foreach ($device in $devices) { Write-Host ('{0}  {1}' -f $device.Serial, $device.State) }
    if ($ListDevices) { return }
    if ($devices.Count -eq 0) { throw 'No ADB devices. Connect USB or pair/connect wireless ADB using Android settings.' }
    if (-not $Serial) {
        if ($devices.Count -eq 1) { $Serial = $devices[0].Serial }
        elseif ($NonInteractive) { throw 'Multiple ADB devices: specify -Serial.' }
        else { $Serial = Read-Host 'Enter the device serial from the list' }
    }
    $selected = @($devices | Where-Object { $_.Serial -ceq $Serial })
    if ($selected.Count -ne 1) { throw 'The selected serial is not present in adb devices.' }
    if ($selected[0].State -eq 'unauthorized') { throw 'Device unauthorized. Unlock Android and approve the ADB authorization prompt.' }
    if ($selected[0].State -ne 'device') { throw ('Device is not ready: ' + $selected[0].State) }
    [void][System.IO.Directory]::CreateDirectory($OutputDirectory)
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
    Write-Host 'Collecting PineDiag. Press Ctrl+C to stop. Existing device logs are retained.'
    $process = Start-Adb @('-s', $Serial, 'logcat', '-v', 'raw', 'PineDiag:V', '*:S')
    $stderr = $process.StandardError.ReadToEndAsync()
    $deadline = if ($DurationSeconds) { [DateTime]::UtcNow.AddSeconds($DurationSeconds) } else { [DateTime]::MaxValue }
    $lineTask = $process.StandardOutput.ReadLineAsync()
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $lineTask.IsCompleted) { Start-Sleep -Milliseconds 50; continue }
        $line = $lineTask.Result
        if ($null -eq $line) {
            [void]$process.WaitForExit(2000)
            throw ('ADB logcat stream ended (device disconnected or ADB stopped). ' + $stderr.Result)
        }
        $lineTask = $process.StandardOutput.ReadLineAsync()
        # Logcat startup banners and partial/non-JSON messages are not diagnostic events.
        try { $event = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
        if (-not $event -or -not $event.PSObject.Properties['session_id']) { continue }
        if ($SessionId -and $event.session_id -cne $SessionId) { continue }
        if ($TitleId -and (-not $event.PSObject.Properties['title_id'] -or $event.title_id -ine $TitleId)) { continue }
        $safeSession = ([string]$event.session_id -replace '[^A-Za-z0-9_.-]', '_')
        if ($safeSession.Length -gt 80) { $safeSession = $safeSession.Substring(0, 80) }
        if (-not $safeSession) { $safeSession = 'unknown' }
        # Bound open handles even if guest/device output invents sessions.
        if (-not $writers.ContainsKey($safeSession)) {
            if ($writers.Count -ge 32) { $safeSession = 'overflow' }
            if (-not $writers.ContainsKey($safeSession)) {
                $path = Join-Path $OutputDirectory ('pine-{0}-{1}.jsonl' -f $stamp, $safeSession)
                $writer = New-Object System.IO.StreamWriter($path, $true, $utf8)
                $writer.AutoFlush = $true
                $writers[$safeSession] = $writer
                Write-Host ('Saving ' + $path)
            }
        }
        $writers[$safeSession].WriteLine($line)
        Write-Output $line
    }
} finally {
    # PowerShell executes finally on Ctrl+C; no process survives an interrupted stream.
    if ($process) {
        try { if (-not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(2000) } } catch { }
        $process.Dispose()
    }
    foreach ($sink in $writers.Values) { $sink.Dispose() }
}
