#requires -Version 5.1
<#
.SYNOPSIS
Stream PineDiag JSONL over an already authorized USB or wireless ADB connection.
.EXAMPLE
.\tools\pine-diag.ps1 -Serial emulator-5554 -SessionId abc -TitleId 0100751007ADA000
.EXAMPLE
.\tools\pine-diag.ps1 -IncludeCrashLog
# Startup crashes: omit SessionId/TitleId so pre-title evidence is retained.
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
    [switch]$IncludeCrashLog,
    [ValidateRange(0, 86400)][int]$DurationSeconds = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$process = $null
$writers = [System.Collections.Generic.Dictionary[string, System.IO.StreamWriter]]::new([System.StringComparer]::Ordinal)
$overflowWriter = $null
$crashWriter = $null
$crashBytes = 0L
$crashLimitReported = $false

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

function New-SessionWriter([string]$Session, [switch]$Overflow) {
    if ($Overflow) {
        $name = 'overflow'
    } else {
        $safeSession = $Session -replace '[^A-Za-z0-9_.-]', '_'
        if ($safeSession.Length -gt 80) { $safeSession = $safeSession.Substring(0, 80) }
        # Distinguish IDs that collide after sanitizing, truncating or Windows case folding.
        $hash = [System.Security.Cryptography.SHA256]::Create()
        try { $suffix = [BitConverter]::ToString($hash.ComputeHash($script:utf8.GetBytes($Session))).Replace('-', '').Substring(0, 12).ToLowerInvariant() }
        finally { $hash.Dispose() }
        $name = '{0}-{1}' -f $safeSession, $suffix
    }
    $path = Join-Path $script:OutputDirectory ('pine-{0}-{1}.jsonl' -f $script:stamp, $name)
    $sink = New-Object System.IO.StreamWriter($path, $true, $script:utf8)
    $sink.AutoFlush = $true
    Write-Host ('Saving ' + $path)
    return $sink
}

try {
    $command = Get-Command $AdbPath -CommandType Application -ErrorAction SilentlyContinue
    if (-not $command) { throw 'ADB not found. Install official Android SDK Platform-Tools and put adb on PATH, or pass -AdbPath.' }
    $adbExecutable = $command.Source
    $process = Start-Adb @('devices', '-l')
    $deviceRead = $process.StandardOutput.ReadToEndAsync()
    $deviceError = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(10000)) { throw 'ADB device discovery timed out.' }
    if ($process.ExitCode -ne 0) {
        $detail = if ($deviceError.IsCompleted) { $deviceError.Result } else { '' }
        throw ('ADB discovery failed: ' + $detail)
    }
    if (-not $deviceRead.Wait(2000)) { throw 'ADB device discovery output did not close.' }
    $devices = @($deviceRead.Result -split '\r?\n' | ForEach-Object {
        if ($_ -match '^(\S+)\s+(device|offline|unauthorized|recovery|sideload|bootloader|authorizing|connecting|no permissions)(?:\s|$)') {
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
    if ($selected[0].State -eq 'no permissions') { throw 'Device has no ADB permissions. Check the host USB permissions and Android SDK Platform-Tools setup.' }
    if ($selected[0].State -ne 'device') { throw ('Device is not ready: ' + $selected[0].State) }
    [void][System.IO.Directory]::CreateDirectory($OutputDirectory)
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
    Write-Host 'Collecting PineDiag. Press Ctrl+C to stop. Existing device logs are retained.'
    if ($IncludeCrashLog) {
        $crashPath = Join-Path $OutputDirectory ('pine-crash-{0}.log' -f $stamp)
        $crashWriter = New-Object System.IO.StreamWriter($crashPath, $false, $utf8)
        $crashWriter.AutoFlush = $true
        Write-Host ('Saving unfiltered crash context (32 MiB limit): ' + $crashPath)
        Write-Host 'Crash context can contain private paths or other apps. Review locally before sharing.'
        $process = Start-Adb @('-s', $Serial, 'logcat', '-b', 'main', '-b', 'crash', '-v', 'raw', 'PineDiag:V', 'AndroidRuntime:V', 'DEBUG:V', 'libc:V', '*:S')
    } else {
        $process = Start-Adb @('-s', $Serial, 'logcat', '-v', 'raw', 'PineDiag:V', '*:S')
    }
    $stderr = $process.StandardError.ReadToEndAsync()
    $elapsed = [System.Diagnostics.Stopwatch]::StartNew()
    $lineTask = $process.StandardOutput.ReadLineAsync()
    while (-not $DurationSeconds -or $elapsed.Elapsed.TotalSeconds -lt $DurationSeconds) {
        if (-not $lineTask.IsCompleted) { Start-Sleep -Milliseconds 50; continue }
        $line = $lineTask.Result
        if ($null -eq $line) {
            [void]$process.WaitForExit(2000)
            # A process (or inherited stderr handle) can stay alive after stdout closes.
            $detail = if ($stderr.IsCompleted) { $stderr.Result } else { '' }
            throw ('ADB logcat stream ended (device disconnected or ADB stopped). ' + $detail)
        }
        $lineTask = $process.StandardOutput.ReadLineAsync()
        if ($crashWriter) {
            $size = $utf8.GetByteCount($line) + 2
            if ($line.Length -le 65536 -and $crashBytes + $size -le 32MB) {
                $crashWriter.WriteLine($line)
                $crashBytes += $size
            } elseif (-not $crashLimitReported) {
                $crashWriter.WriteLine('[Capture omitted oversized lines or reached the 32 MiB crash-context limit; JSONL collection continues.]')
                $crashLimitReported = $true
            }
        }
        # Logcat startup banners and partial/non-JSON messages are not diagnostic events.
        if (-not $line.TrimStart().StartsWith('{')) { continue }
        try { $event = $line | ConvertFrom-Json -ErrorAction Stop } catch { continue }
        if (-not $event -or -not $event.PSObject.Properties['session_id'] -or $event.session_id -isnot [string] -or [string]::IsNullOrWhiteSpace($event.session_id)) { continue }
        if ($SessionId -and $event.session_id -cne $SessionId) { continue }
        if ($TitleId) {
            # New sink records carry the base launch context at top level. A DLC
            # event's fields.title_id is its own Data ID, not the base title.
            $title = $event.PSObject.Properties['title_id']
            if (-not $title -or $title.Value -isnot [string] -or [string]::IsNullOrWhiteSpace($title.Value) -or $title.Value -ieq 'unknown') {
                # Older records and observations before a base is known can
                # still expose an event-specific content identity.
                $fields = $event.PSObject.Properties['fields']
                $title = if ($fields -and $null -ne $fields.Value) { $fields.Value.PSObject.Properties['title_id'] } else { $null }
            }
            if (-not $title -or $title.Value -isnot [string] -or $title.Value -ine $TitleId) { continue }
        }
        # Bound open handles even if guest/device output invents sessions.
        if ($writers.ContainsKey($event.session_id)) {
            $sink = $writers[$event.session_id]
        } elseif ($writers.Count -lt 32) {
            $sink = New-SessionWriter $event.session_id
            $writers.Add($event.session_id, $sink)
        } else {
            if (-not $overflowWriter) { $overflowWriter = New-SessionWriter -Overflow }
            $sink = $overflowWriter
        }
        $sink.WriteLine($line)
        Write-Output $line
    }
} finally {
    # PowerShell executes finally on Ctrl+C; no process survives an interrupted stream.
    if ($process) {
        try { if (-not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(2000) } } catch { }
        $process.Dispose()
    }
    foreach ($sink in $writers.Values) { $sink.Dispose() }
    if ($overflowWriter) { $overflowWriter.Dispose() }
    if ($crashWriter) { $crashWriter.Dispose() }
}
