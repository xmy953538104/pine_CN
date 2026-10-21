#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '../android-bionic/Run-PineBionic.ps1'
$tokens = $null; $parseErrors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$temp = Join-Path ([IO.Path]::GetTempPath()) ('pine bionic wrapper ' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $temp 'host'))
Copy-Item $source (Join-Path $temp 'Run-PineBionic.ps1')
$env:PINE_BOUNDARY_WRAPPER_TEST_ARGS = Join-Path $temp 'argv.txt'
$fakeNode = Join-Path $temp 'host/node.exe'
if ($env:OS -eq 'Windows_NT') {
    $text = 'using System;using System.IO;using System.Text;class Fake{static int Main(string[] a){File.WriteAllLines(Environment.GetEnvironmentVariable("PINE_BOUNDARY_WRAPPER_TEST_ARGS"),a,new UTF8Encoding(false));return 37;}}'
    $code = Join-Path $temp 'Fake.cs'; [IO.File]::WriteAllText($code, $text)
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $compiler /nologo /target:exe "/out:$fakeNode" $code
    if ($LASTEXITCODE) { throw 'Wrapper test process compilation failed' }
} else {
    [IO.File]::WriteAllText($fakeNode, "#!/bin/sh`nprintf '%s\n' `"`$@`" > `"`$PINE_BOUNDARY_WRAPPER_TEST_ARGS`"`nexit 37`n", (New-Object Text.UTF8Encoding($false)))
    & chmod +x $fakeNode
}
$shellExecutable = (Get-Process -Id $PID).Path
$collector = Join-Path $temp 'Run-PineBionic.ps1'
$adb = Join-Path $temp 'ADB with spaces/adb.exe'
$output = Join-Path $temp 'results with spaces'
& $shellExecutable -NoLogo -NoProfile -File $collector -AdbPath $adb -Serial 'SERIAL:5555' -OutputDirectory $output
if ($LASTEXITCODE -ne 37) { throw 'Wrapper swallowed the native process exit code' }
$recorded = [IO.File]::ReadAllLines($env:PINE_BOUNDARY_WRAPPER_TEST_ARGS, [Text.Encoding]::UTF8)
foreach ($expected in @($adb, 'SERIAL:5555', $output, '--bundle', $temp)) {
    if ($recorded -cnotcontains $expected) { throw ('Argument was not preserved: ' + $expected) }
}
Write-Output 'PASS PowerShell AST, paths/argv and failure exit37; transport-only validation, NOT Android execution'
