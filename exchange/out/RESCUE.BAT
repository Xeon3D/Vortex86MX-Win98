@echo off
rem RESCUE.BAT - for "Command prompt only" when Windows does not start:
rem   copies the known-good drivers (saved with KEEP V / KEEP N) straight
rem   into C:\WINDOWS\SYSTEM. Without Windows running nothing is in use,
rem   so no WININIT.INI is needed. Then restart.
rem   RESCUE      both           RESCUE V   display only    RESCUE N   network only
if "%1"=="N" goto net
if "%1"=="n" goto net
if not exist C:\M2010\B\GOODV\VESAMINI.VXD goto nov
copy /y C:\M2010\B\GOODV\VESAMINI.DRV C:\WINDOWS\SYSTEM\VESAMINI.DRV
copy /y C:\M2010\B\GOODV\VESAMINI.VXD C:\WINDOWS\SYSTEM\VESAMINI.VXD
if exist C:\M2010\B\GOODV\ID.TXT copy /y C:\M2010\B\GOODV\ID.TXT C:\M2010\B\CURV.TXT > nul
echo Display driver restored from C:\M2010\B\GOODV
goto vdone
:nov
echo No known-good display driver saved (run KEEP V while it works).
:vdone
if "%1"=="V" goto end
if "%1"=="v" goto end
:net
if not exist C:\M2010\B\GOODN\RDCPCI5.SYS goto non
copy /y C:\M2010\B\GOODN\RDCPCI5.SYS C:\WINDOWS\SYSTEM\RDCPCI5.SYS
if exist C:\M2010\B\GOODN\ID.TXT copy /y C:\M2010\B\GOODN\ID.TXT C:\M2010\B\CURN.TXT > nul
echo Network driver restored from C:\M2010\B\GOODN
goto end
:non
echo No known-good network driver saved (run KEEP N while it works).
:end
