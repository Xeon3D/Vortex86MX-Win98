@echo off
rem KEEP.BAT - remember the driver installed NOW as your known-good version.
rem   KEEP V   display driver      KEEP N   network driver
rem   GOOD V / GOOD N (or RESCUE from DOS) bring it back later.
if not exist C:\M2010\B\NUL md C:\M2010\B
if "%1"=="V" goto vga
if "%1"=="v" goto vga
if "%1"=="N" goto net
if "%1"=="n" goto net
echo Usage: KEEP V   or   KEEP N
goto end
:vga
if not exist C:\M2010\B\GOODV\NUL md C:\M2010\B\GOODV
copy /y C:\WINDOWS\SYSTEM\VESAMINI.DRV C:\M2010\B\GOODV\VESAMINI.DRV > nul
copy /y C:\WINDOWS\SYSTEM\VESAMINI.VXD C:\M2010\B\GOODV\VESAMINI.VXD > nul
if exist C:\M2010\B\CURV.TXT copy /y C:\M2010\B\CURV.TXT C:\M2010\B\GOODV\ID.TXT > nul
if not exist C:\M2010\B\CURV.TXT echo display driver saved with KEEP (no build ID)> C:\M2010\B\GOODV\ID.TXT
echo Installed display driver saved as known-good (C:\M2010\B\GOODV).
dir C:\M2010\B\GOODV
goto end
:net
if not exist C:\M2010\B\GOODN\NUL md C:\M2010\B\GOODN
copy /y C:\WINDOWS\SYSTEM\RDCPCI5.SYS C:\M2010\B\GOODN\RDCPCI5.SYS > nul
if exist C:\M2010\B\CURN.TXT copy /y C:\M2010\B\CURN.TXT C:\M2010\B\GOODN\ID.TXT > nul
if not exist C:\M2010\B\CURN.TXT echo network driver saved with KEEP (no build ID)> C:\M2010\B\GOODN\ID.TXT
echo Installed network driver saved as known-good (C:\M2010\B\GOODN).
dir C:\M2010\B\GOODN
:end
