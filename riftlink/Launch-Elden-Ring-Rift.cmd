@echo off
setlocal
rem Starts Elden Ring OFFLINE (no Easy Anti-Cheat) with the bridge enabled for this launch only.
set "ER_GAME=%~dp0.."
if not exist "%ER_GAME%\eldenring.exe" (
  echo Can't find eldenring.exe next to this folder.
  pause
  exit /b 1
)
if not exist "%ER_GAME%\mods\ERMinecraftPreview.dll" (
  if exist "%ER_GAME%\mods\ERMinecraftPreview.dll.ratchetmode-off" (
    echo Enabling the bridge loader mods\ERMinecraftPreview.dll ^(it stays inactive on normal launches^)
    copy /y "%ER_GAME%\mods\ERMinecraftPreview.dll.ratchetmode-off" "%ER_GAME%\mods\ERMinecraftPreview.dll" >nul
  ) else (
    echo The bridge loader mods\ERMinecraftPreview.dll is missing.
    pause
    exit /b 1
  )
)
if not exist "%ER_GAME%\mods\ermc-preview\erbridge_core.bin" (
  echo The bridge core mods\ermc-preview\erbridge_core.bin is missing.
  pause
  exit /b 1
)
set "ERBRIDGE=1"
set "ERMC_RUNTIME_DIR=%~dp0runtime"
if not exist "%ERMC_RUNTIME_DIR%" mkdir "%ERMC_RUNTIME_DIR%"
echo Steam must be running. Elden Ring starts offline; load your save and stand in the world.
cd /d "%ER_GAME%"
start "" eldenring.exe
rem The link program (riftlink.exe) starts too, unless it is already running. It waits for both games.
tasklist /fi "imagename eq riftlink.exe" 2>nul | find /i "riftlink.exe" >nul || start "" "%~dp0riftlink.exe"
