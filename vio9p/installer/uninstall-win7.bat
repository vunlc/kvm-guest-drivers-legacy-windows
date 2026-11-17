@echo off
rem ===========================================================================
rem Removes vio9p (QEMU VirtFS / virtio-9p) from Windows 7 x64.
rem   uninstall-win7.bat [/quiet]
rem WinFsp, the test certificate and test-signing mode are left in place
rem because other software may rely on them.
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion

if defined PROCESSOR_ARCHITEW6432 (
  "%SystemRoot%\Sysnative\cmd.exe" /c ""%~f0" %*"
  exit /b !errorlevel!
)

set "QUIET="
if /i "%~1"=="/quiet" set "QUIET=1"

set "DEST=%ProgramFiles%\vio9p"
set "SVC=VirtIO-9P"
set "UNINSTKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\vio9p"

net session >nul 2>&1
if errorlevel 1 (
  if defined QUIET (
    echo ERROR: run this as Administrator.
    exit /b 5
  )
  rem Ask for elevation ^(UAC^) and run again as Administrator.
  > "%TEMP%\vio9p-elevate.vbs" echo CreateObject^("Shell.Application"^).ShellExecute "%ComSpec%", "/c """"%~f0"""" %*", "", "runas", 1
  cscript //nologo "%TEMP%\vio9p-elevate.vbs"
  del "%TEMP%\vio9p-elevate.vbs" >nul 2>&1
  exit /b 0
)

echo Removing VirtIO 9P (VirtFS)...

rem Stopping the service unmounts the share.
sc stop %SVC% >nul 2>&1
ping -n 4 127.0.0.1 >nul
sc delete %SVC% >nul 2>&1

rem Detach the device first: this unloads vio9p.sys so its driver packages are
rem no longer in use and can be deleted cleanly.
if exist "%DEST%\vio9psvc.exe" "%DEST%\vio9psvc.exe" --remove-devices

rem Remove every vio9p driver package (earlier versions too) so Windows cannot
rem fall back to an older one. Published INFs may be UTF-16, so read them with
rem "type" (findstr cannot search UTF-16). Windows may rebind the device while
rem packages are removed, so make a few passes.
for /l %%P in (1,1,3) do (
  for %%I in ("%SystemRoot%\INF\oem*.inf") do (
    type "%%~fI" 2>nul | find /i "vio9p.sys" >nul && (
      echo Removing driver package %%~nxI...
      rem pnputil removes the package from the driver store but can leave the
      rem published INF behind; delete that so later passes do not see it again.
      pnputil -f -d %%~nxI >nul 2>&1 && del /f /q "%%~fI" "%%~dpnI.PNF" >nul 2>&1
    )
  )
  ping -n 2 127.0.0.1 >nul
)
rem An INF whose driver-store package is already gone is not removed by
rem pnputil; delete such leftovers directly.
for %%I in ("%SystemRoot%\INF\oem*.inf") do (
  type "%%~fI" 2>nul | find /i "vio9p.sys" >nul && (
    del /f /q "%%~fI" "%%~dpnI.PNF" >nul 2>&1
  )
)

rem Rescan: the device returns without a driver ("PCI Device").
del /f /q "%SystemRoot%\System32\drivers\vio9p.sys" >nul 2>&1
if exist "%DEST%\vio9psvc.exe" "%DEST%\vio9psvc.exe" --rescan

reg delete "%UNINSTKEY%" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\VirtIO-9P" /f >nul 2>&1

echo Done. WinFsp, the test certificate and test-signing mode were left in place.
if not defined QUIET pause

rem Delete the install folder, including this script, after cmd has read it.
cd /d "%TEMP%"
(goto) 2>nul & rmdir /s /q "%DEST%"
