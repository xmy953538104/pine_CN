@echo off
setlocal
:new_directory
set "PINE_BOUNDARY_BOOT=%~dp0results\bootstrap-%RANDOM%-%RANDOM%"
if exist "%PINE_BOUNDARY_BOOT%" goto new_directory
mkdir "%PINE_BOUNDARY_BOOT%" 2>nul
if errorlevel 1 (
  echo Cannot create a private result directory. No Android execution was requested.
  pause
  exit /b 73
)
"%~dp0host\node.exe" "%~dp0host\run-once.js" --bundle "%~dp0." --output "%PINE_BOUNDARY_BOOT%" 2>"%PINE_BOUNDARY_BOOT%\launcher-stderr.log"
set "PINE_BOUNDARY_EXIT=%ERRORLEVEL%"
>"%PINE_BOUNDARY_BOOT%\bootstrap.json" echo {"schema":1,"status":"HOST_COLLECTOR_EXIT_RECORDED","collector_exit_code":%PINE_BOUNDARY_EXIT%,"android_result":"consult nested result.json; bootstrap is not device proof"}
echo.
echo Native collector exit code: %PINE_BOUNDARY_EXIT%
echo Result parent: %PINE_BOUNDARY_BOOT%
echo Keep the result directory. Do not rerun after a native failure.
pause
exit /b %PINE_BOUNDARY_EXIT%
