#requires -Version 5.1
[CmdletBinding()]
param([string]$AdbPath, [string]$Serial, [string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'host/node.exe'
$script = Join-Path $PSScriptRoot 'host/run-once.js'
$arguments = @($script, '--bundle', $PSScriptRoot)
if ($AdbPath) { $arguments += @('--adb', $AdbPath) }
if ($Serial) { $arguments += @('--serial', $Serial) }
if ($OutputDirectory) { $arguments += @('--output', $OutputDirectory) }
& $runner @arguments
exit $LASTEXITCODE
