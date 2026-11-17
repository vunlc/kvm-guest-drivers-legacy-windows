@echo off
rem ===========================================================================
rem vio9p (QEMU VirtFS / virtio-9p) installer for Windows 7 x64.
rem
rem Run as Administrator from the folder that holds the package files:
rem   vio9p.inf vio9p.sys vio9p.cat vio9psvc.exe VirtIOTestCert.cer
rem   uninstall-win7.bat LICENSE.txt and optionally winfsp-*.msi
rem
rem   install-win7.bat [/quiet]
rem
rem Used as-is by vio9p-win7-setup.exe (IExpress package).
rem ===========================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem IExpress packages run as 32-bit processes. Re-run under the 64-bit cmd so
rem pnputil, bcdedit, System32 and the registry are not redirected by WOW64.
if defined PROCESSOR_ARCHITEW6432 (
  "%SystemRoot%\Sysnative\cmd.exe" /c ""%~f0" %*"
  exit /b !errorlevel!
)

set "QUIET="
if /i "%~1"=="/quiet" set "QUIET=1"

set "SRC=%~dp0"
set "DEST=%ProgramFiles%\vio9p"
set "SVC=VirtIO-9P"
set "UNINSTKEY=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\vio9p"
set "NEEDREBOOT="
set "RC=0"

echo VirtIO 9P (VirtFS) for Windows 7
echo ================================
echo.

rem --- checks ----------------------------------------------------------------
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
if /i not "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
  echo ERROR: this package is for 64-bit Windows 7 only.
  set "RC=1" & goto :end
)
for %%F in (vio9p.inf vio9p.sys vio9p.cat vio9psvc.exe) do (
  if not exist "%SRC%%%F" (
    echo ERROR: %%F is missing from "%SRC%".
    set "RC=2" & goto :end
  )
)

rem SHA-256 signatures need KB4474419 (or a later rollup that includes it).
wmic qfe get HotFixID 2>nul | findstr /i "KB4474419" >nul
if errorlevel 1 (
  echo WARNING: could not confirm SHA-2 code signing support ^(KB4474419^).
  echo          If the driver later reports code 52, install KB4490628 and KB4474419.
  echo.
)

rem --- WinFsp ----------------------------------------------------------------
sc qc WinFsp.Launcher >nul 2>&1
if errorlevel 1 (
  set "WINFSP_MSI="
  for %%M in ("%SRC%winfsp-*.msi") do set "WINFSP_MSI=%%~fM"
  if not defined WINFSP_MSI (
    echo ERROR: WinFsp is not installed and no winfsp-*.msi is included.
    set "RC=3" & goto :end
  )
  echo [1/6] Installing WinFsp...
  msiexec /i "!WINFSP_MSI!" /qn /norestart
  if errorlevel 1 if not errorlevel 3010 (
    echo ERROR: WinFsp installation failed.
    set "RC=4" & goto :end
  )
) else (
  echo [1/6] WinFsp is already installed.
)

rem --- test certificate and test signing ------------------------------------
echo [2/6] Trusting the driver's test certificate...
if exist "%SRC%VirtIOTestCert.cer" (
  certutil -addstore -f Root "%SRC%VirtIOTestCert.cer" >nul
  certutil -addstore -f TrustedPublisher "%SRC%VirtIOTestCert.cer" >nul
)

bcdedit /enum {current} | findstr /i "testsigning" | findstr /i "Yes" >nul
if errorlevel 1 (
  echo        Enabling test-signing mode ^(takes effect after a reboot^)...
  bcdedit /set testsigning on >nul
  set "NEEDREBOOT=1"
)

rem --- driver ------------------------------------------------------------------
rem Stop the service first so the driver update does not pull the device out
rem from under a mounted share.
sc stop %SVC% >nul 2>&1
ping -n 4 127.0.0.1 >nul

echo [3/6] Installing the driver...
rem vio9psvc.exe --install-driver forces the package onto the device even when
rem a driver with the same version is installed (pnputil on Windows 7 cannot).
set "PUBLISHED="
"%SRC%vio9psvc.exe" --install-driver "%SRC%vio9p.inf" > "%TEMP%\vio9p-drv.txt"
set "DRVRC=!errorlevel!"
for /f "tokens=2 delims==" %%N in ('findstr /b "PUBLISHED=" "%TEMP%\vio9p-drv.txt"') do set "PUBLISHED=%%N"
del "%TEMP%\vio9p-drv.txt" >nul 2>&1
if "!DRVRC!"=="3010" set "NEEDREBOOT=1"
if not "!DRVRC!"=="0" if not "!DRVRC!"=="3010" set "PUBLISHED="
if not defined PUBLISHED (
  echo ERROR: driver installation failed ^(code !DRVRC!^).
  set "RC=6" & goto :end
)
echo        Driver package: !PUBLISHED!

rem --- files -------------------------------------------------------------------
echo [4/6] Copying files to "%DEST%"...
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%SRC%vio9psvc.exe" "%DEST%\" >nul || (set "RC=7" & goto :end)
copy /y "%SRC%uninstall-win7.bat" "%DEST%\" >nul
if exist "%SRC%LICENSE.txt" copy /y "%SRC%LICENSE.txt" "%DEST%\" >nul
> "%DEST%\driver-package.txt" echo !PUBLISHED!

rem --- service -----------------------------------------------------------------
rem Dependency: PlugPlay (device lookup and arrival/removal notifications).
rem Not WinFsp's driver: the WinFsp DLL loads it on demand and its service name
rem changes with every WinFsp release. Not Vio9pDrv: PnP starts it when the
rem device exists; the service waits for the device itself.
echo [5/6] Configuring the %SVC% service...
sc query %SVC% >nul 2>&1
if errorlevel 1 (
  sc create %SVC% binPath= "\"%DEST%\vio9psvc.exe\"" start= auto depend= PlugPlay DisplayName= "VirtIO 9P (VirtFS) Service" >nul
) else (
  sc config %SVC% binPath= "\"%DEST%\vio9psvc.exe\"" start= auto depend= PlugPlay DisplayName= "VirtIO 9P (VirtFS) Service" >nul
)
if errorlevel 1 (
  echo ERROR: could not create the service.
  set "RC=8" & goto :end
)
sc description %SVC% "Mounts the QEMU VirtFS (virtio-9p) share as a drive using WinFsp." >nul
sc failure %SVC% reset= 86400 actions= restart/5000/restart/10000/restart/30000 >nul

rem --- Programs and Features entry -------------------------------------------
set "VERSION="
for /f "tokens=2 delims=," %%V in ('findstr /b /i "DriverVer" "%SRC%vio9p.inf"') do (
  for /f "tokens=1" %%W in ("%%V") do set "VERSION=%%W"
)
reg add "%UNINSTKEY%" /v DisplayName /d "VirtIO 9P (VirtFS) for Windows 7" /f >nul
reg add "%UNINSTKEY%" /v DisplayVersion /d "!VERSION!" /f >nul
reg add "%UNINSTKEY%" /v Publisher /d "vio9p (virtio-win based)" /f >nul
reg add "%UNINSTKEY%" /v InstallLocation /d "%DEST%" /f >nul
reg add "%UNINSTKEY%" /v UninstallString /d "\"%SystemRoot%\System32\cmd.exe\" /c \"\"%DEST%\uninstall-win7.bat\"\"" /f >nul
reg add "%UNINSTKEY%" /v NoModify /t REG_DWORD /d 1 /f >nul
reg add "%UNINSTKEY%" /v NoRepair /t REG_DWORD /d 1 /f >nul

rem --- start ---------------------------------------------------------------------
if defined NEEDREBOOT (
  echo [6/6] The share will be mounted after the reboot.
) else (
  echo [6/6] Starting the %SVC% service...
  sc start %SVC% >nul
)

echo.
echo Installation complete.
if defined NEEDREBOOT (
  echo RESTART WINDOWS to finish: test-signing mode was just enabled.
) else (
  echo The VirtFS share appears as a new drive ^(Z: or the next free letter^).
)

:end
echo.
if not defined QUIET pause
exit /b %RC%
