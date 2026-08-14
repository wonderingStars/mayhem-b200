@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM
REM Double-click launcher for mayhem-b200. TWO processes make the full
REM experience, and this starts both:
REM
REM   mayhem-b200.exe     the radio app itself; serves the JSON/screen API on
REM                       127.0.0.1:8090 (that endpoint is an API, not a UI --
REM                       opening it in a browser shows nothing useful)
REM   mayhem-portal.exe   the web portal: the app grid, the ADS-B map, the
REM                       live screen. Serves http://127.0.0.1:8081 and talks
REM                       to the API above.
REM
REM It waits until the PORTAL answers before opening the browser, because the
REM B200 takes ~15-20 s to initialise and an early browser hit would show
REM "connection refused" with no retry. Once the portal page is up it handles
REM backend restarts on its own.
setlocal
cd /d "%~dp0build"

if not exist mayhem-b200.exe (
    echo mayhem-b200.exe not found in "%~dp0build" - build the project first.
    pause
    exit /b 1
)
if not exist mayhem-portal.exe (
    echo mayhem-portal.exe not found - build it with:
    echo   cd webgui ^&^& go build -o ..\build\mayhem-portal.exe .\cmd\mayhem-portal
    pause
    exit /b 1
)

tasklist /fi "imagename eq mayhem-b200.exe" 2>nul | find /i "mayhem-b200.exe" >nul
if not errorlevel 1 (
    echo mayhem-b200 is already running - opening the portal.
    start "" http://127.0.0.1:8081
    exit /b 0
)

start "mayhem-b200" mayhem-b200.exe --driver=uhd --portal=8090

tasklist /fi "imagename eq mayhem-portal.exe" 2>nul | find /i "mayhem-portal.exe" >nul
if errorlevel 1 (
    start "mayhem-portal" mayhem-portal.exe -http 127.0.0.1:8081 -backend http://127.0.0.1:8090
)

echo Starting mayhem-b200, waiting for the portal (the B200 takes ~20s)...
for /l %%i in (1,1,60) do (
    curl -s -o nul --max-time 1 http://127.0.0.1:8081/api/status && goto portal_up
    timeout /t 1 /nobreak >nul
)
echo The portal did not come up within 60s - check the mayhem-b200 window.
pause
exit /b 1

:portal_up
start "" http://127.0.0.1:8081
exit /b 0
