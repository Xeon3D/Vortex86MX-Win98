@echo off
rem BACK.BAT - reinstall the driver that TRY replaced (works without the network).
rem   BACK V   display driver      BACK N   network driver
rem   The swap is queued in WININIT.INI for the next boot.
if "%1"=="V" goto vga
if "%1"=="v" goto vga
if "%1"=="N" goto net
if "%1"=="n" goto net
echo Usage: BACK V   or   BACK N
goto end
:vga
if not exist C:\M2010\B\LASTV\VESAMINI.VXD goto none
if not exist C:\M2010\B\PEND\NUL md C:\M2010\B\PEND
copy /y C:\M2010\B\LASTV\VESAMINI.DRV C:\M2010\B\PEND\VESAMINI.DRV > nul
copy /y C:\M2010\B\LASTV\VESAMINI.VXD C:\M2010\B\PEND\VESAMINI.VXD > nul
if not exist C:\WINDOWS\WININIT.INI goto newiniV
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto linesV
:newiniV
echo [rename]> C:\WINDOWS\WININIT.INI
:linesV
echo C:\WINDOWS\SYSTEM\VESAMINI.DRV=C:\M2010\B\PEND\VESAMINI.DRV>> C:\WINDOWS\WININIT.INI
echo C:\WINDOWS\SYSTEM\VESAMINI.VXD=C:\M2010\B\PEND\VESAMINI.VXD>> C:\WINDOWS\WININIT.INI
if exist C:\M2010\B\LASTV\ID.TXT copy /y C:\M2010\B\LASTV\ID.TXT C:\M2010\B\CURV.TXT > nul
echo Display driver from C:\M2010\B\LASTV queued; installed during the next boot.
if exist C:\M2010\B\LASTV\ID.TXT type C:\M2010\B\LASTV\ID.TXT
goto end
:net
if not exist C:\M2010\B\LASTN\RDCPCI5.SYS goto none
if not exist C:\M2010\B\PEND\NUL md C:\M2010\B\PEND
copy /y C:\M2010\B\LASTN\RDCPCI5.SYS C:\M2010\B\PEND\RDCPCI5.SYS > nul
if not exist C:\WINDOWS\WININIT.INI goto newiniN
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto linesN
:newiniN
echo [rename]> C:\WINDOWS\WININIT.INI
:linesN
echo C:\WINDOWS\SYSTEM\RDCPCI5.SYS=C:\M2010\B\PEND\RDCPCI5.SYS>> C:\WINDOWS\WININIT.INI
if exist C:\M2010\B\LASTN\ID.TXT copy /y C:\M2010\B\LASTN\ID.TXT C:\M2010\B\CURN.TXT > nul
echo Network driver from C:\M2010\B\LASTN queued; installed during the next boot.
if exist C:\M2010\B\LASTN\ID.TXT type C:\M2010\B\LASTN\ID.TXT
goto end
:none
echo *** nothing saved in C:\M2010\B\LAST%1 yet - nothing changed
:end
