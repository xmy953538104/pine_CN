@echo off
setlocal
"%~dp0host\node.exe" "%~dp0host\run-once.js" --bundle "%~dp0"
set "PINE_BOUNDARY_EXIT=%ERRORLEVEL%"
echo.
echo Native collector exit code: %PINE_BOUNDARY_EXIT%
echo Keep the result directory. Do not rerun after a native failure.
pause
exit /b %PINE_BOUNDARY_EXIT%
