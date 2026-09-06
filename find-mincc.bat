@echo off
rem Called inside the caller's SETLOCAL; returns MINCC_COMPILER.
set "MINCC_COMPILER="
if defined MINCC_HOME goto configured
for %%C in (petcc64.exe) do set "MINCC_COMPILER=%%~$PATH:C"
if defined MINCC_COMPILER exit /b 0
if exist "%~dp0..\mincc\petcc64.exe" set "MINCC_COMPILER=%~dp0..\mincc\petcc64.exe"
if defined MINCC_COMPILER exit /b 0
if exist "%~dp0..\..\mincc\petcc64.exe" set "MINCC_COMPILER=%~dp0..\..\mincc\petcc64.exe"
if defined MINCC_COMPILER exit /b 0
>&2 echo mincc not found. Set MINCC_HOME to its directory or add petcc64.exe to PATH.
exit /b 1
:configured
if exist "%MINCC_HOME%\petcc64.exe" goto found
>&2 echo MINCC_HOME does not contain petcc64.exe: "%MINCC_HOME%"
exit /b 1
:found
for %%C in ("%MINCC_HOME%\petcc64.exe") do set "MINCC_COMPILER=%%~fC"
exit /b 0
