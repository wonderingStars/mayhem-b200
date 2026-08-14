@echo off
REM Start the sdrlink server with the SoapySDR worker so mayhem-b200 can drive
REM non-USRP radios (RTL-SDR, HackRF, Airspy, Pluto, Lime, BladeRF, ...).
REM
REM SOAPY_SDR_PLUGIN_PATH points SoapySDR at the bundled device modules; adding
REM this folder to PATH lets those modules find their native libraries.
REM
REM SPDX-License-Identifier: GPL-2.0-or-later
setlocal
cd /d "%~dp0"
set "SOAPY_SDR_PLUGIN_PATH=%~dp0modules0.8"
set "PATH=%~dp0;%PATH%"

REM control 5960 (what the app connects to), stream 5961, server's own GUI on 8082.
REM Run INLINE, not via `start /min`: this .cmd is already launched hidden, so
REM the server inherits that hidden console (and passes it to its worker child).
REM `start` would instead create a brand-new, visible-but-minimised console.
sdrlink-server.exe -worker "%~dp0sdrlink-worker.exe" -control-port 5960 -stream-port 5961 -http-port 8082
