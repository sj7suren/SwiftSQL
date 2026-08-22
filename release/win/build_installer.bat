@echo off
REM ==========================================================================
REM  Build the SwiftSQL Windows installer with Inno Setup 6.
REM  Prerequisites:
REM    * Inno Setup 6 installed (provides ISCC.exe).
REM    * ChineseSimplified.isl present in Inno's Languages folder
REM      (for the Simplified Chinese installer language).
REM    * release\win\dist\SwiftSQL.exe staged (a fresh release build of the exe).
REM  Output: release\win\Output\SwiftSQL-Setup-1.1.20.exe
REM ==========================================================================
setlocal
set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo [ERROR] Inno Setup 6 not found.
  echo         Install it from https://jrsoftware.org/isdl.php
  echo         or edit this script to point ISCC at your install.
  exit /b 1
)
if not exist "%~dp0dist\SwiftSQL.exe" (
  echo [ERROR] release\win\dist\SwiftSQL.exe is missing.
  echo         Build the app in Release and copy build\SwiftSQL.exe to dist\.
  exit /b 1
)
"%ISCC%" "%~dp0SwiftSQL.iss"
echo ISCC_EXIT=%errorlevel%
endlocal
