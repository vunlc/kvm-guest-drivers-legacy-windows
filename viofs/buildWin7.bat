@echo off
rem Builds viofs (driver, service and package) for Windows 7 / Server 2008 R2.
rem Must be run from a shell that is NOT already inside a newer EWDK environment;
rem see WIN7.md for the required toolchain.
call ..\build\build.bat viofs.sln "Win7" %*
