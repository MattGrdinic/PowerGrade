@echo off
REM PowerGrade - Windows installer. Right-click > Run as administrator.
REM Copies PowerGrade.ofx.bundle into the common OFX plugin folder.
setlocal
set "SRC=%~dp0PowerGrade.ofx.bundle"
set "DEST=%CommonProgramFiles%\OFX\Plugins"

if not exist "%SRC%" (
  echo PowerGrade.ofx.bundle not found next to this installer.
  pause & exit /b 1
)

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Please right-click this file and choose "Run as administrator".
  pause & exit /b 1
)

if not exist "%DEST%" mkdir "%DEST%"
if exist "%DEST%\PowerGrade.ofx.bundle" rmdir /s /q "%DEST%\PowerGrade.ofx.bundle"
xcopy /E /I /Y "%SRC%" "%DEST%\PowerGrade.ofx.bundle" >nul

echo PowerGrade installed to "%DEST%".
echo Restart DaVinci Resolve, then find it under Effects ^> OpenFX ^> Power Grade.
pause
