@echo off
rem Builds vio9p (driver, service and package) for Windows 7 / Server 2008 R2.
rem Must be run from a shell that is NOT already inside a newer EWDK environment;
rem see WIN7.md for the required toolchain.
call ..\build\build.bat vio9p.sln "Win7" %*
