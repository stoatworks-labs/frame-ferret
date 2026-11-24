@echo off
rem Desktop Duplication on VDD's display with the card up, nothing of Frame
rem Ferret's in between: once counting pictures only, once copying each to
rem memory and reading its counter.
rem
rem   vd-dup.cmd <seconds>
setlocal
set BIN=%~dp0..\..\build\Release
set LOG=%TEMP%\ffvd
if not exist "%LOG%" mkdir "%LOG%"
set /a CARD=%1*2+12
start "" /b "%BIN%\vdbench.exe" card vdd %CARD% 60 > "%LOG%\card.log" 2>&1
ping -n 4 127.0.0.1 > nul
"%BIN%\vdbench.exe" dup %1
"%BIN%\vdbench.exe" dup %1 copy
ping -n 8 127.0.0.1 > nul
echo ---- card
type "%LOG%\card.log"
