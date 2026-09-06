@echo off
setlocal DisableDelayedExpansion
call "%~dp0find-mincc.bat"
if errorlevel 1 exit /b 1
pushd "%~dp0" || exit /b 1
if not exist build mkdir build
if not exist build goto failed
"%MINCC_COMPILER%" -std -Wall -Werror -peconsole -I src -o build\usb-eject.exe src\main.c src\text.c src\cli.c src\inventory.c src\target.c src\eject.c src\diagnose.c src\action.c src\output.c src\portable.c -lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32
if errorlevel 1 goto failed
echo Built "%CD%\build\usb-eject.exe"
popd
exit /b 0
:failed
popd
exit /b 1
