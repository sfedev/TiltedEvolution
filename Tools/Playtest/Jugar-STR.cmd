@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-STR.ps1" %*
if errorlevel 1 (
  echo.
  echo No se ha podido iniciar Skyrim Together. Revisa el mensaje anterior.
  pause
  exit /b 1
)
