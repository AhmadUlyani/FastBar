@echo off
rem Build FastBar with MinGW gcc (https://www.mingw-w64.org/ or MinGW.org)
cd /d "%~dp0"
windres fastbar.rc -O coff -o fastbar_res.o
if errorlevel 1 (echo Build failed & exit /b 1)
gcc -Os -s -Wall -mwindows fastbar.c fastbar_res.o -o FastBar.exe -lshell32 -lgdi32 -lole32 -luuid
if errorlevel 1 (echo Build failed & exit /b 1)
windres guard.rc -O coff -o guard_res.o
if errorlevel 1 (echo Build failed & exit /b 1)
gcc -Os -s -Wall -mwindows guard.c guard_res.o -o FastBarGuard.exe -lshell32
if errorlevel 1 (echo Build failed & exit /b 1)
echo OK: FastBar.exe, FastBarGuard.exe
