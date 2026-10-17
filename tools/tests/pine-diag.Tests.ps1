#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$collector = Join-Path $PSScriptRoot '../pine-diag.ps1'
$temp = Join-Path ([IO.Path]::GetTempPath()) ('pine adb test-' + [guid]::NewGuid())
[void][IO.Directory]::CreateDirectory($temp)
$env:PINE_FAKE_ADB_ARGS = Join-Path $temp 'args.txt'
$env:PINE_FAKE_ADB_PID = Join-Path $temp 'pid.txt'
$env:PINE_FAKE_ADB_CLOSED = Join-Path $temp 'closed.txt'
$env:PINE_FAKE_ADB_FIXTURE = Join-Path $PSScriptRoot 'fixtures/native-sink.jsonl'
$env:PINE_FAKE_ADB_SESSIONS_FIXTURE = Join-Path $PSScriptRoot 'fixtures/native-sessions.jsonl'
$fixtureLines = [IO.File]::ReadAllLines($env:PINE_FAKE_ADB_FIXTURE, [Text.Encoding]::UTF8)
$isWindowsHost = $env:OS -eq 'Windows_NT'
if ($isWindowsHost) {
    $fake = Join-Path $temp 'adb.exe'
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $compiler /nologo /target:exe /codepage:65001 "/out:$fake" (Join-Path $PSScriptRoot 'fake-adb.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Fake ADB compilation failed' }
    # On Windows, closing the managed fake's GetStdHandle leaves the pipe open.
    # Use a CRT/CLR-free executable to exercise actual native EOF while alive.
    $closedOutputAdb = Join-Path $temp 'adb-closed-output.exe'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $visualStudio) { throw 'Visual Studio C++ tools are required for the native EOF fixture' }
    $setup = Join-Path $visualStudio 'VC/Auxiliary/Build/vcvars64.bat'
    $source = Join-Path $PSScriptRoot 'fake-adb-closed-output.c'
    $object = Join-Path $temp 'fake-adb-closed-output.obj'
    $ctrlcConsole = Join-Path $temp 'ctrlc-console.exe'
    $ctrlcSource = Join-Path $PSScriptRoot 'ctrlc-console.c'
    $ctrlcObject = Join-Path $temp 'ctrlc-console.obj'
    $buildBatch = Join-Path $temp 'build-native.cmd'
    [IO.File]::WriteAllLines($buildBatch, @(
        '@echo off', '@chcp 65001 >nul', ('call "{0}" >nul' -f $setup),
        'if errorlevel 1 exit /b %errorlevel%',
        ('cl.exe /nologo /Od /W4 /WX /GS- /Zl /TC /Fo"{0}" "{1}" /link /NODEFAULTLIB /ENTRY:FakeAdbMain /SUBSYSTEM:CONSOLE /OUT:"{2}" kernel32.lib' -f $object, $source, $closedOutputAdb),
        'if errorlevel 1 exit /b %errorlevel%',
        ('cl.exe /nologo /Od /W4 /WX /GS- /Zl /TC /Fo"{0}" "{1}" /link /NODEFAULTLIB /ENTRY:CtrlCTestMain /SUBSYSTEM:WINDOWS /OUT:"{2}" kernel32.lib' -f $ctrlcObject, $ctrlcSource, $ctrlcConsole),
        'exit /b %errorlevel%'
    ), (New-Object System.Text.UTF8Encoding($false)))
    $buildProcess = New-Object System.Diagnostics.Process
    try {
        $buildProcess.StartInfo.FileName = $env:ComSpec
        $buildProcess.StartInfo.Arguments = '/d /s /c ""' + $buildBatch + '""'
        $buildProcess.StartInfo.UseShellExecute = $false
        $buildProcess.StartInfo.CreateNoWindow = $true
        $buildProcess.StartInfo.RedirectStandardOutput = $true
        $buildProcess.StartInfo.RedirectStandardError = $true
        [void]$buildProcess.Start()
        $buildOutput = $buildProcess.StandardOutput.ReadToEndAsync()
        $buildError = $buildProcess.StandardError.ReadToEndAsync()
        if (-not $buildProcess.WaitForExit(60000)) { throw 'Native ADB test helper compilation timed out' }
        if ($buildProcess.ExitCode -ne 0) {
            $outputText = if ($buildOutput.Wait(2000)) { $buildOutput.Result } else { '[compiler stdout did not close]' }
            $errorText = if ($buildError.Wait(2000)) { $buildError.Result } else { '[compiler stderr did not close]' }
            throw ('Native ADB test helper compilation failed: ' + $outputText + $errorText)
        }
    } finally {
        try { if (-not $buildProcess.HasExited) { $buildProcess.Kill(); [void]$buildProcess.WaitForExit(2000) } } catch { }
        $buildProcess.Dispose()
    }
} else {
    $fake = Join-Path $temp 'adb'
    Copy-Item (Join-Path $PSScriptRoot 'fake-adb.py') $fake
    & chmod +x $fake
    $closedOutputAdb = $fake
}
function Assert([bool]$condition, [string]$message) { if (-not $condition) { throw $message } }
function Read-SharedText([string]$Path, [IO.FileShare]$Share = [IO.FileShare]::ReadWrite) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $Share)
    $reader = $null
    try {
        # Strict UTF8, without BOM detection silently changing the encoding.
        $reader = New-Object System.IO.StreamReader($stream, (New-Object System.Text.UTF8Encoding($false, $true)), $false)
        return $reader.ReadToEnd()
    } finally {
        if ($reader) { $reader.Dispose() } else { $stream.Dispose() }
    }
}
function Test-WindowsConsoleCtrlC {
    $consoleOutput = Join-Path $temp 'console-ctrlc'
    $consolePid = Join-Path $temp 'console-adb-pid.txt'
    $ready = Join-Path $temp 'console-ready.txt'
    $report = Join-Path $temp 'console-report.txt'
    $powershellExecutable = (Get-Process -Id $PID).Path
    $helper = New-Object System.Diagnostics.Process
    $elapsed = [Diagnostics.Stopwatch]::StartNew()
    try {
        $helper.StartInfo.FileName = $ctrlcConsole
        $helper.StartInfo.UseShellExecute = $false
        # The GUI subsystem helper has no inherited console. It creates its own
        # PowerShell console; the native membership check guards the broadcast.
        $helper.StartInfo.EnvironmentVariables['PINE_CTRLC_EXE'] = $powershellExecutable
        $helper.StartInfo.EnvironmentVariables['PINE_CTRLC_COMMAND'] = '"{0}" -NoLogo -NoProfile -File "{1}" -AdbPath "{2}" -NonInteractive -OutputDirectory "{3}"' -f $powershellExecutable, [IO.Path]::GetFullPath($collector), $fake, $consoleOutput
        $helper.StartInfo.EnvironmentVariables['PINE_CTRLC_READY'] = $ready
        $helper.StartInfo.EnvironmentVariables['PINE_CTRLC_REPORT'] = $report
        $helper.StartInfo.EnvironmentVariables['PINE_FAKE_ADB_MODE'] = 'normal'
        $helper.StartInfo.EnvironmentVariables['PINE_FAKE_ADB_PID'] = $consolePid
        $helper.StartInfo.EnvironmentVariables['PINE_FAKE_ADB_ARGS'] = Join-Path $temp 'console-adb-args.txt'
        Write-Host ('WINDOWS CTRL_C HOST executable={0}; powershell={1}; clr={2}' -f $powershellExecutable, $PSVersionTable.PSVersion, [Environment]::Version)
        [void]$helper.Start()
        do {
            $files = @(Get-ChildItem $consoleOutput -Filter '*.jsonl' -ErrorAction SilentlyContinue)
            $records = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
            foreach ($file in $files) {
                try {
                    foreach ($line in ((Read-SharedText $file.FullName) -split '\r?\n')) {
                        if ($fixtureLines -ccontains $line) { [void]$records.Add($line) }
                    }
                } catch [IO.IOException], [Text.DecoderFallbackException] { }
            }
            if ((Test-Path $consolePid) -and $files.Count -eq 3 -and $records.Count -eq $fixtureLines.Length) { break }
            if ($helper.HasExited) { break }
            Start-Sleep -Milliseconds 50
        } while ($elapsed.Elapsed.TotalSeconds -lt 15)
        Assert ((Test-Path $consolePid) -and $files.Count -eq 3 -and $records.Count -eq $fixtureLines.Length -and -not $helper.HasExited) ('Console Ctrl+C fixture not ready: files={0}; completeRecords={1}/{2}; helperExited={3}' -f $files.Count, $records.Count, $fixtureLines.Length, $helper.HasExited)
        $fakePid = [int][IO.File]::ReadAllText($consolePid)
        Write-Host ('WINDOWS CTRL_C READY files={0}; completeRecords={1}/{2}; fakeAdbPid={3}' -f $files.Count, $records.Count, $fixtureLines.Length, $fakePid)
        # Atomic marker: only this suite authorizes the single signal, after
        # reading every original sink record from all still-open session files.
        [IO.File]::WriteAllText(($ready + '.tmp'), [string]$fakePid, (New-Object System.Text.UTF8Encoding($false)))
        [IO.File]::Move(($ready + '.tmp'), $ready)
        $remaining = [Math]::Max(1, 35000 - [int]$elapsed.Elapsed.TotalMilliseconds)
        Assert ($helper.WaitForExit($remaining)) 'Console Ctrl+C helper exceeded the 35-second overall bound'
        $nativeReport = Read-SharedText $report
        Assert ($helper.ExitCode -eq 0 -and $nativeReport -match '(?m)^result=PASS\r?$') 'Native console Ctrl+C delivery, isolation or process cleanup failed'
        $files = @(Get-ChildItem $consoleOutput -Filter '*.jsonl')
        Assert ($files.Count -eq 3) 'Console Ctrl+C changed the native session file count'
        $lines = @($files | ForEach-Object {
            # Exclusive access also checks that no surviving writer retains a file.
            (Read-SharedText $_.FullName ([IO.FileShare]::None)) -split '\r?\n' | Where-Object { $_.Length -gt 0 }
        })
        Assert ($lines.Count -eq $fixtureLines.Length) 'Console Ctrl+C lost or duplicated a native sink record'
        $distinct = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
        foreach ($line in $lines) {
            Assert ($fixtureLines -ccontains $line) 'Console Ctrl+C altered native UTF8 JSONL or added a BOM'
            [void]$distinct.Add($line)
        }
        Assert ($distinct.Count -eq $fixtureLines.Length) 'Console Ctrl+C did not retain every native sink record'
        $events = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
        Assert (@($events | Where-Object { $_.fields.message -eq $expectedUnicode }).Count -eq 1) 'Console Ctrl+C did not preserve the complete Unicode event'
        Write-Host 'PASS: Windows CTRL_C_EVENT=0 in an isolated console, PowerShell exit within 10s, original fake ADB handle reaped before Job closure, complete UTF8 JSONL and exclusive output access'
    } finally {
        # Killing the helper closes only its private Job. This is failure cleanup
        # and can never turn the failed assertion into a passing test.
        try { if (-not $helper.HasExited) { $helper.Kill(); [void]$helper.WaitForExit(2000) } } catch { }
        if (Test-Path $report) { Write-Host ("WINDOWS CTRL_C CONSOLE PROBE`n" + (Read-SharedText $report)) }
        $helper.Dispose()
    }
}
function Expect-Failure([string]$mode, [string]$expected, [string]$selectedSerial = '', [string]$failureAdbPath = $fake) {
    $env:PINE_FAKE_ADB_MODE = $mode
    $caught = $false
    # Give Windows .NET startup/EOF time to finish; this is not a deadline test.
    try { & $collector -AdbPath $failureAdbPath -Serial $selectedSerial -NonInteractive -OutputDirectory $temp -DurationSeconds 15 | Out-Null }
    catch {
        $caught = $_.ToString().Contains($expected)
        if (-not $caught) { throw }
    }
    Assert $caught "Expected failure for $mode containing $expected"
}
function Show-WindowsClosedOutputProbe([string]$Executable, [string]$Label) {
    # Distinguish a live pipe writer from a reader task that missed native EOF.
    # This supplements, and does not replace, the collector's EOF assertions.
    if (-not ('PineAdbPipeProbe' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PineAdbPipeProbe {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool PeekNamedPipe(IntPtr pipe, IntPtr buffer, uint size,
        IntPtr bytesRead, out uint available, IntPtr bytesLeft);
}
'@ }
    $savedPid = $env:PINE_FAKE_ADB_PID
    $savedClosed = $env:PINE_FAKE_ADB_CLOSED
    $env:PINE_FAKE_ADB_PID = Join-Path $temp ('probe-pid-' + $Label + '.txt')
    $env:PINE_FAKE_ADB_CLOSED = Join-Path $temp ('probe-closed-' + $Label + '.txt')
    $env:PINE_FAKE_ADB_MODE = 'closed-output'
    $probe = New-Object System.Diagnostics.Process
    try {
        $probe.StartInfo.FileName = $Executable
        $probe.StartInfo.Arguments = '-s test-device logcat -v raw PineDiag:V *:S'
        $probe.StartInfo.UseShellExecute = $false
        $probe.StartInfo.CreateNoWindow = $true
        $probe.StartInfo.RedirectStandardOutput = $true
        $probe.StartInfo.RedirectStandardError = $true
        $probe.StartInfo.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
        $probe.StartInfo.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
        [void]$probe.Start()
        # Expose the read handle before starting async reads, avoiding any
        # FileStream flush/position synchronization while a read is pending.
        $readHandle = $probe.StandardOutput.BaseStream.SafeFileHandle.DangerousGetHandle()
        $errors = $probe.StandardError.ReadToEndAsync()
        $limit = [DateTime]::UtcNow.AddSeconds(8)
        $lastFixtureLineSeen = $false
        $received = 0
        $read = $null
        while ([DateTime]::UtcNow -lt $limit) {
            $read = $probe.StandardOutput.ReadLineAsync()
            while (-not $read.IsCompleted -and [DateTime]::UtcNow -lt $limit) { Start-Sleep -Milliseconds 50 }
            if (-not $read.IsCompleted -or $null -eq $read.Result) { break }
            $received++
            if ($read.Result -ceq $fixtureLines[$fixtureLines.Length - 1]) { $lastFixtureLineSeen = $true; break }
        }
        while (-not (Test-Path $env:PINE_FAKE_ADB_CLOSED) -and -not $probe.HasExited -and [DateTime]::UtcNow -lt $limit) { Start-Sleep -Milliseconds 50 }
        [uint32]$available = 0
        $peek = $false
        $peekError = -1
        if ($lastFixtureLineSeen) {
            # Peek can block behind a pending synchronous ReadFile. All previous
            # line tasks are complete here; inspect the pipe before asking for EOF.
            $peek = [PineAdbPipeProbe]::PeekNamedPipe($readHandle, [IntPtr]::Zero, 0, [IntPtr]::Zero, [ref]$available, [IntPtr]::Zero)
            $peekError = if ($peek) { 0 } else { [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
            $read = $probe.StandardOutput.ReadLineAsync()
            if ($Label -eq 'native') { [void]$read.Wait(2000) }
            else { Start-Sleep -Milliseconds 200 }
        }
        $marker = if (Test-Path $env:PINE_FAKE_ADB_CLOSED) { [IO.File]::ReadAllText($env:PINE_FAKE_ADB_CLOSED) } else { '' }
        $hasResult = $read -and $read.Status -eq [System.Threading.Tasks.TaskStatus]::RanToCompletion
        $diagnostic = [ordered]@{
            fixture = $Label
            powershell = $PSVersionTable.PSVersion.ToString(); clr = [Environment]::Version.ToString()
            pid = $probe.Id; marker = $marker; exited = $probe.HasExited
            receivedLines = $received; lastFixtureLineSeen = $lastFixtureLineSeen
            stdoutTask = [string]$read.Status; stderrTask = [string]$errors.Status
            stdoutResultIsNull = if ($hasResult) { $null -eq $read.Result } else { $null }
            stdoutResultType = if ($hasResult -and $null -ne $read.Result) { $read.Result.GetType().FullName } else { '' }
            peekSucceeded = $peek; peekError = $peekError; available = $available
        }
        Write-Host ('WINDOWS CLOSED-OUTPUT PROBE ' + ($diagnostic | ConvertTo-Json -Compress))
        if ($Label -eq 'native') {
            Assert ($lastFixtureLineSeen -and $marker -eq [string]$probe.Id -and -not $probe.HasExited) 'Native EOF fixture must publish its marker and remain alive after all fixture records'
            Assert (-not $peek -and $peekError -eq 109) 'Native EOF fixture must produce kernel ERROR_BROKEN_PIPE, not an empty connected pipe'
            Assert ($hasResult -and $null -eq $read.Result) 'Native EOF fixture must complete ReadLineAsync with null while the process is alive'
        }
    } finally {
        try { if (-not $probe.HasExited) { $probe.Kill(); [void]$probe.WaitForExit(2000) } } catch { }
        $probe.Dispose()
        $env:PINE_FAKE_ADB_PID = $savedPid
        $env:PINE_FAKE_ADB_CLOSED = $savedClosed
    }
}
try {
    if ($isWindowsHost) {
        Show-WindowsClosedOutputProbe $fake 'managed'
        Show-WindowsClosedOutputProbe $closedOutputAdb 'native'
    }
    $env:PINE_FAKE_ADB_MODE = 'multiple'
    $listOutput = Join-Path $temp 'list-only'
    & $collector -AdbPath $fake -ListDevices -NonInteractive -OutputDirectory $listOutput | Out-Null
    Assert (-not (Test-Path $env:PINE_FAKE_ADB_PID) -and -not (Test-Path $listOutput)) 'Device listing should not start logcat or create a capture directory'
    Expect-Failure 'none' 'No ADB devices'
    Expect-Failure 'multiple' 'Multiple ADB devices'
    Expect-Failure 'multiple' 'not present' 'missing-device'
    Expect-Failure 'unauthorized' 'unauthorized'
    Expect-Failure 'offline' 'not ready'
    Expect-Failure 'permissions' 'no ADB permissions'
    Expect-Failure 'discovery-error' 'ADB discovery failed'
    Expect-Failure 'disconnect' 'stream ended'
    $closedOutput = [Diagnostics.Stopwatch]::StartNew()
    try { Expect-Failure 'closed-output' 'stream ended' '' $closedOutputAdb }
    finally { Assert (Test-Path $env:PINE_FAKE_ADB_CLOSED) 'Closed-output fixture did not confirm native stdout closure' }
    Assert ($closedOutput.Elapsed.TotalSeconds -lt 12) 'Closed stdout must not wait forever for stderr/process exit'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    Assert ([IO.File]::ReadAllText($env:PINE_FAKE_ADB_CLOSED) -eq [string]$childId) 'Closed-output fixture marker came from a different process'
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Closed stream leaked ADB'
    $env:PINE_FAKE_ADB_MODE = 'multiple'
    $out = Join-Path $temp 'filtered'
    & $collector -AdbPath $fake -Serial second-device -SessionId native-adb-a -TitleId 0100751007ada000 -OutputDirectory $out -DurationSeconds 3 | Out-Null
    $files = @(Get-ChildItem $out -Filter '*.jsonl')
    Assert ($files.Count -eq 1) 'Session/title filtering should produce one file'
    $lines = @([IO.File]::ReadAllLines($files[0].FullName, [Text.Encoding]::UTF8))
    Assert ($lines.Count -eq 6) 'Base title filter must retain DLC/guest/stage/summary context and known content before launch'
    # Compare the collected bytes with actual sink output, not a second event
    # serializer. The first BEFORE_TITLE record has neither known title field.
    for ($i = 0; $i -lt $lines.Count; $i++) { Assert ($lines[$i] -ceq $fixtureLines[$i + 1]) 'Collector altered or selected the wrong native sink record' }
    $events = @($lines | ForEach-Object { $_ | ConvertFrom-Json })
    Assert ($events[0].fields.detail -eq 'phase=ui_association execution_confirmed=false') 'Structured pre-native title must survive the title filter without parsing detail text'
    $event = @($events | Where-Object { $_.event -eq 'BASE_SELECTED' })[0]
    $expectedUnicode = -join ([char[]]@(0x4e2d, 0x6587, 0x20, 0x65e5, 0x5fd7))
    Assert ($event.fields.message -eq $expectedUnicode) 'UTF8 did not round trip'
    $bytes = [IO.File]::ReadAllBytes($files[0].FullName)
    Assert (-not ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf)) 'JSONL should be UTF8 without a BOM'
    Assert ($files[0].Name -match 'native-adb-a') 'Output filename lacks session'
    $arguments = [IO.File]::ReadAllText($env:PINE_FAKE_ADB_ARGS)
    Assert ($arguments.Contains('-s second-device logcat -v raw PineDiag:V *:S')) 'Wrong device/tag'
    Assert (-not ($arguments -match 'logcat.* -c')) 'Collector must not clear device logs'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Timed collection leaked ADB'

    $env:PINE_FAKE_ADB_MODE = 'crash-context'
    $crashOutput = Join-Path $temp 'crash-context'
    & $collector -AdbPath $fake -NonInteractive -IncludeCrashLog -OutputDirectory $crashOutput -DurationSeconds 3 | Out-Null
    $crashFiles = @(Get-ChildItem $crashOutput -Filter '*.log')
    Assert ($crashFiles.Count -eq 1) 'Crash context should be saved separately from JSONL'
    $crashText = Read-SharedText $crashFiles[0].FullName
    Assert ($crashText.Contains('Fatal signal 6') -and $crashText.Contains('FATAL EXCEPTION: synthetic')) 'Non-JSON native/Java crash lines must survive'
    Assert ($crashText.Contains('BEFORE_TITLE')) 'Crash context must include startup events with unknown title'
    Assert (@(Get-ChildItem $crashOutput -Filter '*.jsonl').Count -eq 3) 'Crash capture must keep all sessions without implicit title/session filtering'
    $arguments = [IO.File]::ReadAllText($env:PINE_FAKE_ADB_ARGS)
    Assert ($arguments.Contains('logcat -b main -b crash -v raw PineDiag:V AndroidRuntime:V DEBUG:V libc:V *:S')) 'Crash mode must read actual main/crash buffers and fatal tags'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Crash collection leaked ADB'
    $env:PINE_FAKE_ADB_MODE = 'multiple'
    $ownTitleOutput = Join-Path $temp 'own-title'
    & $collector -AdbPath $fake -Serial second-device -SessionId native-adb-a -TitleId 0100751007ADB001 -OutputDirectory $ownTitleOutput -DurationSeconds 3 | Out-Null
    Assert (@(Get-ChildItem $ownTitleOutput -Filter '*.jsonl').Count -eq 0) 'A distinct DLC Data ID must not override known base-title context'
    $unknownOutput = Join-Path $temp 'unknown-base'
    & $collector -AdbPath $fake -Serial second-device -SessionId native-adb-unknown -TitleId 0100751007ADB001 -OutputDirectory $unknownOutput -DurationSeconds 3 | Out-Null
    $unknownFiles = @(Get-ChildItem $unknownOutput -Filter '*.jsonl')
    Assert ($unknownFiles.Count -eq 1) 'Unknown base must allow event-specific fields.title_id fallback'
    $unknownLines = @([IO.File]::ReadAllLines($unknownFiles[0].FullName, [Text.Encoding]::UTF8))
    Assert ($unknownLines.Count -eq 1 -and $unknownLines[0] -ceq $fixtureLines[10]) 'Fallback must preserve the native unknown-base content record only'

    # Controlled schema inputs only: canonical application identity must beat a
    # Patch CNMT legacy root, while the old root fallback remains supported.
    $identityFixture = Join-Path $temp 'identity.jsonl'
    [IO.File]::WriteAllLines($identityFixture, @(
        '{"session_id":"identity","launch_id":"one","title_id":"0100751007ADA800","application_title_id":"0100751007ADA000","event":"DLC_DATA_READ","fields":{"title_id":"0100751007ADB001"}}',
        '{"session_id":"identity","launch_id":"two","title_id":"0100751007ADA000","application_title_id":"0100751007ADA000","event":"OTHER_LAUNCH","fields":{}}',
        '{"session_id":"identity","launch_id":"one","title_id":"0100751007ADA000","event":"LEGACY_ROOT","fields":{}}',
        '{"session_id":"importer","application_title_id":"0100000000009999","launch_id":"other","event":"PREVIOUS_NATIVE_TRACE","fields":{"association_status":"verified","previous_launch_id":"one","previous_session_id":"identity","previous_application_title_id":"0100751007ADA000"}}',
        '{"session_id":"importer","application_title_id":"0100751007ADA000","launch_id":"one","event":"PREVIOUS_PROCESS_CRASH","fields":{"association_status":"unknown","previous_launch_id":"one","previous_application_title_id":"0100751007ADA000"}}',
        '{"session_id":"import-ui","application_title_id":"","title_id":"","launch_id":"","event":"CONTENT_IMPORT_RESULT","fields":{"application_title_id":"0100751007ADA000","title_id":"0100751007ADA800"}}'
    ), (New-Object System.Text.UTF8Encoding($false)))
    $originalFixture = $env:PINE_FAKE_ADB_FIXTURE
    try {
        $env:PINE_FAKE_ADB_FIXTURE = $identityFixture
        $identityOutput = Join-Path $temp 'identity'
        & $collector -AdbPath $fake -Serial second-device -TitleId 0100751007ADA000 -LaunchId one -OutputDirectory $identityOutput -DurationSeconds 3 | Out-Null
        $identityRows = @(Get-ChildItem $identityOutput -Filter '*.jsonl' | ForEach-Object { [IO.File]::ReadAllLines($_.FullName) | ForEach-Object { $_ | ConvertFrom-Json } })
        Assert ($identityRows.Count -eq 3 -and @($identityRows | Where-Object { $_.event -eq 'DLC_DATA_READ' }).Count -eq 1 -and @($identityRows | Where-Object { $_.event -eq 'LEGACY_ROOT' }).Count -eq 1 -and @($identityRows | Where-Object { $_.event -eq 'PREVIOUS_NATIVE_TRACE' }).Count -eq 1) 'Application/CNMT or launch filtering lost or misattributed records'
        $previousSessionOutput = Join-Path $temp 'previous-session'
        & $collector -AdbPath $fake -Serial second-device -TitleId 0100751007ADA000 -LaunchId one -SessionId identity -OutputDirectory $previousSessionOutput -DurationSeconds 3 | Out-Null
        $previousRows = @(Get-ChildItem $previousSessionOutput -Filter '*.jsonl' | ForEach-Object { [IO.File]::ReadAllLines($_.FullName) | ForEach-Object { $_ | ConvertFrom-Json } })
        Assert ($previousRows.Count -eq 3) 'Verified prior-session filtering lost the historical trace or admitted an unknown association'
        $ownerOutput = Join-Path $temp 'typed-owner'
        & $collector -AdbPath $fake -Serial second-device -TitleId 0100751007ADA000 -OutputDirectory $ownerOutput -DurationSeconds 3 | Out-Null
        $ownerRows = @(Get-ChildItem $ownerOutput -Filter '*.jsonl' | ForEach-Object { [IO.File]::ReadAllLines($_.FullName) | ForEach-Object { $_ | ConvertFrom-Json } })
        $imports = @($ownerRows | Where-Object { $_.event -eq 'CONTENT_IMPORT_RESULT' })
        Assert ($ownerRows.Count -eq 5 -and $imports.Count -eq 1 -and $imports[0].fields.title_id -eq '0100751007ADA800') 'Typed import owner filtering lost the Patch content or admitted an unknown history association'

    } finally { $env:PINE_FAKE_ADB_FIXTURE = $originalFixture }

    $env:PINE_FAKE_ADB_MODE = 'sessions'
    $grouped = Join-Path $temp 'grouped'
    & $collector -AdbPath $fake -NonInteractive -OutputDirectory $grouped -DurationSeconds 3 | Out-Null
    $files = @(Get-ChildItem $grouped -Filter '*.jsonl')
    Assert ($files.Count -eq 33) 'Expected 32 session files and one bounded overflow file'
    $events = 0
    $overflowEvents = 0
    $collisionFiles = 0
    foreach ($file in $files) {
        $rows = @([IO.File]::ReadAllLines($file.FullName, [Text.Encoding]::UTF8) | ForEach-Object { $_ | ConvertFrom-Json })
        $events += $rows.Count
        if ($file.Name -match '-overflow\.jsonl$') {
            $overflowEvents += $rows.Count
        } else {
            $first = $rows[0].session_id
            foreach ($row in $rows) { Assert ($row.session_id -ceq $first) 'Distinct session IDs were mixed in a session file' }
            if ($first -ceq 'collision/a') {
                $collisionFiles++
                Assert ($rows.Count -eq 3) 'Repeated session events and the native summary must reuse their file'
            }
        }
    }
    Assert ($events -eq 81 -and $overflowEvents -eq 16 -and $collisionFiles -eq 1) 'Sanitized/truncated/case-sensitive session grouping or overflow lost native records'
    $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Grouped collection leaked ADB'

    # PowerShell pipeline cancellation follows the Ctrl+C stop/finally path.
    # This checks engine cancellation; console keyboard delivery is a separate manual check.
    Remove-Item $env:PINE_FAKE_ADB_PID
    $env:PINE_FAKE_ADB_MODE = 'normal'
    $cancelOutput = Join-Path $temp 'cancel'
    $runspace = [PowerShell]::Create()
    try {
        [void]$runspace.AddCommand($collector).AddParameter('AdbPath', $fake).AddParameter('NonInteractive').AddParameter('OutputDirectory', $cancelOutput)
        $pending = $runspace.BeginInvoke()
        $limit = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $saved = @(Get-ChildItem $cancelOutput -Filter '*.jsonl' -ErrorAction SilentlyContinue)
            $savedRecords = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
            foreach ($file in $saved) {
                # Windows directory enumeration can report a stale Length while
                # a writer is open. Read the actual bytes, sharing its write access.
                $reader = $null
                try {
                    $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
                    try {
                        $reader = New-Object System.IO.StreamReader($stream, [Text.Encoding]::UTF8)
                        $text = $reader.ReadToEnd()
                    } finally {
                        if ($reader) { $reader.Dispose() } else { $stream.Dispose() }
                    }
                    foreach ($line in ($text -split '\r?\n')) {
                        if ($fixtureLines -ccontains $line) { [void]$savedRecords.Add($line) }
                    }
                } catch [IO.IOException] { }
            }
            # Wait for the full real-sink corpus, including all three summaries,
            # to exercise cancellation with populated, still-open writers.
            if ((Test-Path $env:PINE_FAKE_ADB_PID) -and $saved.Count -eq 3 -and $savedRecords.Count -eq $fixtureLines.Length) { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $limit)
        if (-not ((Test-Path $env:PINE_FAKE_ADB_PID) -and $saved.Count -eq 3 -and $savedRecords.Count -eq $fixtureLines.Length)) {
            $fileDetails = @($saved | ForEach-Object { $_.Name + ':' + $_.Length }) -join ', '
            $errors = @($runspace.Streams.Error | ForEach-Object { $_.ToString() }) -join ' | '
            throw ('Cancellation fixture never saved all native sessions; state={0}; files=[{1}]; completeRecords={2}/{3}; errors=[{4}]' -f $runspace.InvocationStateInfo.State, $fileDetails, $savedRecords.Count, $fixtureLines.Length, $errors)
        }
        $directorySizes = @($saved | ForEach-Object { $_.Length }) -join ','
        Write-Host ('CANCELLATION FIXTURE READY state={0}; completeRecords={1}/{2}; directorySizes=[{3}]' -f $runspace.InvocationStateInfo.State, $savedRecords.Count, $fixtureLines.Length, $directorySizes)
        Assert ($runspace.InvocationStateInfo.State -eq [System.Management.Automation.PSInvocationState]::Running) 'Cancellation must stop a still-running collector'
        $childId = [int][IO.File]::ReadAllText($env:PINE_FAKE_ADB_PID)
    } finally {
        $runspace.Stop()
        $runspace.Dispose()
    }
    Assert (-not (Get-Process -Id $childId -ErrorAction SilentlyContinue)) 'Cancellation leaked ADB'
    $cancelEvents = @(Get-ChildItem $cancelOutput -Filter '*.jsonl' | ForEach-Object { [IO.File]::ReadAllLines($_.FullName, [Text.Encoding]::UTF8) } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert (@($cancelEvents | Where-Object { $_.fields.message -eq $expectedUnicode }).Count -eq 1) 'Cancellation did not preserve a complete UTF8 event'
    if ($isWindowsHost) { Test-WindowsConsoleCtrlC }
    else { Write-Host 'SKIP: Windows CTRL_C_EVENT console delivery requires Windows console APIs; pipeline cancellation was tested separately' }
    Write-Host 'PASS: actual native sink JSONL replay, discovery/devices/errors, base-title and unknown-context fallback filters, UTF8/BOM, session grouping/overflow, deadline and cancellation cleanup'
} finally {
    Remove-Item $temp -Recurse -Force
    Remove-Item Env:PINE_FAKE_ADB_ARGS, Env:PINE_FAKE_ADB_PID, Env:PINE_FAKE_ADB_CLOSED, Env:PINE_FAKE_ADB_MODE, Env:PINE_FAKE_ADB_FIXTURE, Env:PINE_FAKE_ADB_SESSIONS_FIXTURE -ErrorAction SilentlyContinue
}
