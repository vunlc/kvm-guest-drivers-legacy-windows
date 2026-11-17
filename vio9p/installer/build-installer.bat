@echo off
rem ===========================================================================
rem Builds vio9p-win7-setup.exe (self-extracting IExpress package).
rem   build-installer.bat <winfsp-msi>
rem Run after buildWin7.bat amd64 and build\signAll.bat. Output:
rem   ..\Install\vio9p-win7-setup.exe
rem ===========================================================================
setlocal EnableExtensions

set "HERE=%~dp0"
set "PKG=%HERE%..\Install\Win7\amd64"
set "STAGE=%HERE%stage"
set "SED=%HERE%vio9p-win7-setup.sed"
rem IExpress needs a fully resolved output path (no "..").
for %%I in ("%HERE%..\Install") do set "OUT=%%~fI\vio9p-win7-setup.exe"
set "MSI=%~f1"

if "%~1"=="" (
  echo usage: build-installer.bat ^<winfsp-msi^>
  exit /b 1
)
if not exist "%MSI%" (
  echo ERROR: "%MSI%" not found.
  exit /b 1
)
for %%F in (vio9p.inf vio9p.sys vio9p.cat vio9psvc.exe) do (
  if not exist "%PKG%\%%F" (
    echo ERROR: "%PKG%\%%F" not found. Build first.
    exit /b 1
  )
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
for %%F in (vio9p.inf vio9p.sys vio9p.cat vio9psvc.exe) do copy /y "%PKG%\%%F" "%STAGE%\" >nul
copy /y "%HERE%..\..\build\VirtIOTestCert.cer" "%STAGE%\" >nul
copy /y "%HERE%install-win7.bat" "%STAGE%\" >nul
copy /y "%HERE%uninstall-win7.bat" "%STAGE%\" >nul
copy /y "%MSI%" "%STAGE%\" >nul
rem The license dialog needs CRLF line breaks.
type "%HERE%..\LICENSE" | more > "%STAGE%\LICENSE.txt"
for %%M in ("%MSI%") do set "MSINAME=%%~nxM"

if exist "%OUT%" del /q "%OUT%"

> "%SED%" (
  echo [Version]
  echo Class=IEXPRESS
  echo SEDVersion=3
  echo [Options]
  echo PackagePurpose=InstallApp
  echo ShowInstallProgramWindow=0
  echo HideExtractAnimation=1
  echo UseLongFileName=1
  echo InsideCompressed=0
  echo CAB_FixedSize=0
  echo CAB_ResvCodeSigning=0
  echo RebootMode=N
  echo InstallPrompt=%%InstallPrompt%%
  echo DisplayLicense=%%DisplayLicense%%
  echo FinishMessage=%%FinishMessage%%
  echo TargetName=%%TargetName%%
  echo FriendlyName=%%FriendlyName%%
  echo AppLaunched=%%AppLaunched%%
  echo PostInstallCmd=%%PostInstallCmd%%
  echo AdminQuietInstCmd=%%AdminQuietInstCmd%%
  echo UserQuietInstCmd=%%UserQuietInstCmd%%
  echo SourceFiles=SourceFiles
  echo [Strings]
  echo InstallPrompt=Install VirtIO 9P ^(VirtFS^) for Windows 7 x64? This installs WinFsp if needed, trusts the test certificate of the driver and enables test-signing mode ^(a restart may be required^).
  echo DisplayLicense=%STAGE%\LICENSE.txt
  echo FinishMessage=
  echo TargetName=%OUT%
  echo FriendlyName=VirtIO 9P ^(VirtFS^) for Windows 7
  echo AppLaunched=cmd /c install-win7.bat
  echo PostInstallCmd=^<None^>
  echo AdminQuietInstCmd=cmd /c install-win7.bat /quiet
  echo UserQuietInstCmd=cmd /c install-win7.bat /quiet
  echo FILE0="vio9p.inf"
  echo FILE1="vio9p.sys"
  echo FILE2="vio9p.cat"
  echo FILE3="vio9psvc.exe"
  echo FILE4="VirtIOTestCert.cer"
  echo FILE5="install-win7.bat"
  echo FILE6="uninstall-win7.bat"
  echo FILE7="LICENSE.txt"
  echo FILE8="%MSINAME%"
  echo [SourceFiles]
  echo SourceFiles0=%STAGE%\
  echo [SourceFiles0]
  echo %%FILE0%%=
  echo %%FILE1%%=
  echo %%FILE2%%=
  echo %%FILE3%%=
  echo %%FILE4%%=
  echo %%FILE5%%=
  echo %%FILE6%%=
  echo %%FILE7%%=
  echo %%FILE8%%=
)

rem iexpress.exe is a GUI program: wait for it explicitly. It does not accept a
rem quoted SED path, so pass the short (8.3) form, which has no spaces.
for %%S in ("%SED%") do set "SEDSHORT=%%~sS"
start "" /wait "%SystemRoot%\System32\iexpress.exe" /N /Q %SEDSHORT%
if not exist "%OUT%" (
  echo ERROR: IExpress did not produce "%OUT%".
  exit /b 1
)
echo Built "%OUT%"
exit /b 0
