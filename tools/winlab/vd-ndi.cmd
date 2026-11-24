@echo off
rem The whole route on Windows: VDD's display -> frame-ferret run (the
rem virtual-display node routed to an NDI sender) -> vdbench probe, a separate
rem process receiving that NDI source and judging the counter card on it.
rem
rem   vd-ndi.cmd <seconds>
rem
rem Needs an NDI runtime: FERRET_NDI_RUNTIME, else the NDI 6 Runtime's usual
rem place, else the copy Resolume Arena ships (winlab has only that one). Run
rem it in the signed-in session (s1run.ps1 -Exe ...\vd-ndi.cmd).
setlocal
set BIN=%~dp0..\..\build\Release
set LOG=%TEMP%\ffvd
if not exist "%LOG%" mkdir "%LOG%"
if "%FERRET_NDI_RUNTIME%"=="" if exist "C:\Program Files\Resolume Arena\Processing.NDI.Lib.x64.dll" set "FERRET_NDI_RUNTIME=C:\Program Files\Resolume Arena\Processing.NDI.Lib.x64.dll"
set /a CARD=%1+45
set /a HALF=%1/2+10

start "" /b "%BIN%\frame-ferret.exe" run --config "%~dp0vd-ndi.json" > "%LOG%\ferret.log" 2>&1
ping -n 8 127.0.0.1 > nul
start "" /b "%BIN%\vdbench.exe" card vdd %CARD% 60 > "%LOG%\card.log" 2>&1
start "" /b "%BIN%\vdbench.exe" probe "Virtual Display" %1 > "%LOG%\probe.log" 2>&1
ping -n %HALF% 127.0.0.1 > nul
curl -s http://127.0.0.1:8740/api/state > "%LOG%\state.json"
curl -s -o "%LOG%\monitor.bmp" http://127.0.0.1:8740/preview/monitor.bmp
ping -n %HALF% 127.0.0.1 > nul
ping -n 15 127.0.0.1 > nul
taskkill /im frame-ferret.exe /f > nul 2>&1
taskkill /im vdbench.exe /f > nul 2>&1
echo ---- frame-ferret
type "%LOG%\ferret.log"
echo ---- probe
type "%LOG%\probe.log"
echo ---- card
type "%LOG%\card.log"
