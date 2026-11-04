@echo off
rem Close the running FastBar (it restores the taskbar), rebuild, start the new build.
cd /d "%~dp0"
echo Closing FastBar...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Add-Type -Name W -Namespace FB -MemberDefinition '[DllImport(\"user32.dll\", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string c, string n); [DllImport(\"user32.dll\")] public static extern bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);';" ^
  "$h = [FB.W]::FindWindowW('FastBarMain', 'FastBar');" ^
  "if ($h -ne [IntPtr]::Zero) { [FB.W]::PostMessageW($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }" ^
  "for ($i = 0; $i -lt 50 -and (Get-Process FastBar, FastBarGuard -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 100 }" ^
  "if (Get-Process FastBar -ErrorAction SilentlyContinue) { Write-Host 'FastBar did not exit'; exit 1 }"
if errorlevel 1 (echo Could not close FastBar & exit /b 1)
call "%~dp0build.bat"
if errorlevel 1 exit /b 1
start "" "%~dp0FastBar.exe"
echo FastBar restarted with the new build.
