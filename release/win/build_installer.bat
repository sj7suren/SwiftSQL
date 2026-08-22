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

REM ---- Static-CRT guard ------------------------------------------------------
REM SwiftSQL.iss bundles NO Visual C++ redistributable, because the exe links the
REM CRT statically (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" in CMakeLists.txt).
REM Those two facts live in different files with nothing tying them together. If
REM the build ever switches to the dynamic CRT (/MD), the exe starts importing
REM vcruntime140.dll and this script would happily produce an installer that dies
REM on any machine without the VC++ runtime -- a failure that CANNOT reproduce on
REM a developer box, because Visual Studio already put the runtime there.
REM So verify it here, where the assumption is actually being relied on.
where dumpbin >nul 2>&1
if errorlevel 1 (
  echo [WARN] dumpbin not found - skipping the static-CRT check.
  echo        Run this from a Visual Studio developer prompt to enable it.
) else (
  dumpbin /nologo /dependents "%~dp0dist\SwiftSQL.exe" | findstr /i "vcruntime140 msvcp140" >nul
  if not errorlevel 1 (
    echo [ERROR] dist\SwiftSQL.exe imports the DYNAMIC Visual C++ runtime.
    echo         The installer ships no redistributable, so the package would fail
    echo         to start on a clean machine.
    echo         Fix either side of the contract:
    echo           - build with the static CRT ^(/MT^), or
    echo           - restore VC_redist.x64.exe to the Files/Run sections of the .iss
    exit /b 1
  )
)

"%ISCC%" "%~dp0SwiftSQL.iss"
echo ISCC_EXIT=%errorlevel%
endlocal
