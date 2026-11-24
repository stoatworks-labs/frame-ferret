@echo off
rem What Windows makes of VDD's modes: vdbench modes, then the same after
rem setting <width> <height> <freq> the old way (59 asks for 59.94).
rem
rem   vd-modes.cmd [width height freq]
setlocal
set BIN=%~dp0..\..\build\Release
"%BIN%\vdbench.exe" modes
if not "%3"=="" "%BIN%\vdbench.exe" modes %1 %2 %3
