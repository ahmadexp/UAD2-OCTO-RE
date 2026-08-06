@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-PluginHost.ps1" -ShutdownWhenDone
if errorlevel 1 (
  echo.
  echo Plug-in host installation failed. Review the message above.
  pause
)
