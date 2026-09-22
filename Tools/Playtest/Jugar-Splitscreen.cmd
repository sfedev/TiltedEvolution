@echo off
rem Pantalla dividida local: servidor + cliente A (teclado/raton) + cliente B (mando).
rem Parametros opcionales: -Layout Stacked  |  -NoAutoConnect  |  -CheckOnly  |  -RestorePrefs
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Jugar-Splitscreen.ps1" %*
pause
