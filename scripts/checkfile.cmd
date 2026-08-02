@echo off
REM mayhem-b200 - compile a single translation unit to check it.
REM
REM Use this instead of a full cmake build when other people may be writing
REM files in the same tree at the same time: the CMake source globs would pull
REM in their half-written code and the failure would not be yours.
REM
REM Usage (from the repo root):  scripts\checkfile.cmd src\apps\my_app.cpp
REM Exits 0 if the file compiles.

if "%~1"=="" (
  echo usage: scripts\checkfile.cmd ^<source.cpp^> [more.cpp ...]
  exit /b 2
)

REM The source list is written to a response file *before* vcvars64.bat runs.
REM vcvars re-exports its environment past an endlocal, which discards both our
REM variables and this batch file's argument context; a file on disk survives.
REM A per-process response file and object dir, so concurrent agents running
REM this script at once do not clobber each other's argument list or objects.
set "MB200_RSP=%TEMP%\mb200_check_%RANDOM%_%RANDOM%.rsp"
> "%MB200_RSP%" echo %1 %2 %3 %4 %5 %6 %7 %8 %9

if not defined VSINSTALLDIR (
  if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  ) else (
    echo cannot find vcvars64.bat - install the VS 2022 Build Tools
    exit /b 3
  )
)

set "MB200_OBJ=%TEMP%\mb200_check_%RANDOM%_%RANDOM%\"
if not exist "%MB200_OBJ%" mkdir "%MB200_OBJ%"

cl /nologo /c /std:c++20 /EHsc /permissive- /Zc:__cplusplus /utf-8 /W4 ^
   /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS ^
   /I src /I src\ui /I src\core /I src\dsp /I src\radio /I src\audio /I src\apps /I tests ^
   /external:I "C:\Program Files\UHD\include" ^
   /external:I "C:\vcpkg\installed\x64-windows\include" /external:W0 ^
   /Fo"%MB200_OBJ%" ^
   @"%MB200_RSP%"

set "MB200_RC=%ERRORLEVEL%"
del "%MB200_RSP%" >nul 2>&1
rmdir /s /q "%MB200_OBJ%" >nul 2>&1
exit /b %MB200_RC%
