@echo off
rem Build FastBar with MinGW gcc (https://www.mingw-w64.org/ or MinGW.org)
cd /d "%~dp0"
gcc -Os -s -Wall -mwindows fastbar.c -o FastBar.exe -lshell32 -lgdi32
if errorlevel 1 (echo Build failed & exit /b 1)
gcc -Os -s -Wall -mwindows guard.c -o FastBarGuard.exe -lshell32
if errorlevel 1 (echo Build failed & exit /b 1)
echo OK: FastBar.exe, FastBarGuard.exe
