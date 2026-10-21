#requires -Version 5.1
[CmdletBinding()]
param([string]$AdbPath, [string]$Serial, [string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'host/node.exe'
$script = Join-Path $PSScriptRoot 'host/run-once.js'
$resultRoot = if ($OutputDirectory) { $OutputDirectory } else { Join-Path $PSScriptRoot 'results' }
$bootstrap = Join-Path $resultRoot ('bootstrap-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($bootstrap)
$arguments = @($script, '--bundle', $PSScriptRoot, '--output', $bootstrap)
if ($AdbPath) { $arguments += @('--adb', $AdbPath) }
if ($Serial) { $arguments += @('--serial', $Serial) }
$exitCode = 1; $launchError = $null; $launchHResult = $null
try { & $runner @arguments; $exitCode = $LASTEXITCODE }
catch { $launchError = $_.Exception.Message; $launchHResult = $_.Exception.HResult }
finally {
    $record = @{ schema = 1; status = 'HOST_COLLECTOR_EXIT_RECORDED'; collector_exit_code = $exitCode; error = $launchError; windows_launch_hresult = $launchHResult; android_result = 'consult nested result.json; bootstrap is not device proof' }
    [IO.File]::WriteAllText((Join-Path $bootstrap 'bootstrap.json'), ($record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Write-Host ('Result parent: ' + $bootstrap)
}
exit $exitCode
