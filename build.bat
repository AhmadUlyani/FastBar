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
rem FastBarTap.dll (see-through taskbar) is loaded into 64-bit Explorer, so it
rem needs a 64-bit gcc (mingw-w64). Set GCC64, or it is looked for in D:\mingw64.
if not defined GCC64 set "GCC64=D:\mingw64\bin\gcc.exe"
if exist "%GCC64%" (
  "%GCC64%" -O2 -s -Wall -shared -static tap.c -o FastBarTap.tmp -lole32 -loleaut32 -luser32
  if errorlevel 1 (echo Build failed & exit /b 1)
  rem Explorer keeps the DLL loaded (and locked) once it was used
  move /y FastBarTap.tmp FastBarTap.dll >nul 2>&1 || (del FastBarTap.tmp & echo Note: FastBarTap.dll is in use by Explorer, not replaced. Restart Explorer to update it.)
) else (echo Note: no 64-bit gcc at "%GCC64%", FastBarTap.dll not built)
echo OK: FastBar.exe, FastBarGuard.exe, FastBarTap.dll
