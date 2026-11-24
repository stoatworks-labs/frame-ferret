@echo off
rem The virtual-display node on its own, no transport: vdbench builds the node
rem in-process and judges the counter card it captures.
rem
rem   vd-direct.cmd <width> <height> <hz> <seconds>
rem
rem Run it in the signed-in session (s1run.ps1 -Exe ...\vd-direct.cmd). The
rem node sets VDD's display to the mode first; the card starts after it and
rem reads the display's size then.
setlocal
set BIN=%~dp0..\..\build\Release
set LOG=%TEMP%\ffvd
if not exist "%LOG%" mkdir "%LOG%"
set /a CARD=%4+25

start "" /b "%BIN%\vdbench.exe" direct %4 %1 %2 %3 > "%LOG%\direct.log" 2>&1
ping -n 8 127.0.0.1 > nul
"%BIN%\vdbench.exe" card vdd %CARD% %3 > "%LOG%\card.log" 2>&1
ping -n 3 127.0.0.1 > nul
echo ---- card
type "%LOG%\card.log"
echo ---- direct
type "%LOG%\direct.log"
