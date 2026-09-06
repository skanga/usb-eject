@echo off
setlocal DisableDelayedExpansion
call "%~dp0..\find-mincc.bat"
if errorlevel 1 exit /b 1
pushd "%~dp0.." || exit /b 1
set "MODULES=src\text.c src\cli.c src\inventory.c src\target.c src\eject.c src\diagnose.c src\action.c src\output.c src\portable.c"
set "LIBS=-lkernel32 -lshell32 -lsetupapi -lcfgmgr32 -ladvapi32 -luser32"
call :compile usb-eject-tests tests\test_main.c %MODULES%
if errorlevel 1 goto failed
call :compile usb-eject-smoke src\main.c %MODULES%
if errorlevel 1 goto failed
call :compile flow-tests tests\flow_tests.c %MODULES%
if errorlevel 1 goto failed
call :compile card-tests tests\card_tests.c src\target.c src\text.c
if errorlevel 1 goto failed
call :compile portable-tests tests\portable_tests.c src\text.c src\cli.c src\inventory.c src\target.c src\eject.c src\diagnose.c src\action.c src\output.c
if errorlevel 1 goto failed
call :compile deadline-tests tests\diagnostic_deadline_tests.c src\target.c src\text.c
if errorlevel 1 goto failed
call :compile inventory-tests tests\inventory_tests.c src\text.c
if errorlevel 1 goto failed
call :compile diagnose-smoke tests\diagnose_smoke.c src\target.c src\text.c src\diagnose.c
if errorlevel 1 goto failed
call :compile runner tests\runner.c
if errorlevel 1 goto failed
tests\runner.exe
if errorlevel 1 goto failed
popd
exit /b 0
:compile
set "TEST_NAME=%1"
rem SHIFT does not change %%*, so collect the remaining source arguments.
set "TEST_SOURCES="
:source
shift
if "%~1"=="" goto build
set "TEST_SOURCES=%TEST_SOURCES% %1"
goto source
:build
"%MINCC_COMPILER%" -std -Wall -Werror -peconsole -I src -o "tests\%TEST_NAME%.exe" %TEST_SOURCES% %LIBS%
exit /b %errorlevel%
:failed
popd
exit /b 1
