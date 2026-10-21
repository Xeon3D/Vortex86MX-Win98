@echo off
rem TRY.BAT - install a build by its ID, keeping the current one as backup.
rem   TRY V27   display driver build (VESAMINI.DRV + VESAMINI.VXD)
rem   TRY N3    network driver build (installed as RDCPCI5.SYS)
rem   The build is downloaded to C:\M2010\B\(build ID), the files installed now are
rem   saved to C:\M2010\B\LASTV or LASTN, and the swap is queued in
rem   WININIT.INI for the next boot. Undo with BACK V / BACK N (offline).
set PC=10.0.0.239
if "%1"=="" goto usage
if not exist C:\M2010\NUL md C:\M2010
if not exist C:\M2010\B\NUL md C:\M2010\B
if not exist C:\M2010\B\%1\NUL md C:\M2010\B\%1
if not exist C:\M2010\B\PEND\NUL md C:\M2010\B\PEND
C:
cd \M2010\B\%1
if exist VESAMINI.DRV del VESAMINI.DRV
if exist VESAMINI.VXD del VESAMINI.VXD
if exist RDCPCI5.SYS del RDCPCI5.SYS
echo open %PC%> try.scr
echo user anonymous ebox@>> try.scr
echo binary>> try.scr
echo cd out>> try.scr
echo cd %1>> try.scr
echo get BUILD.TXT>> try.scr
echo get VESAMINI.DRV>> try.scr
echo get VESAMINI.VXD>> try.scr
echo get RDCPCI5.SYS>> try.scr
echo bye>> try.scr
ftp -n -s:try.scr > nul
if exist VESAMINI.VXD goto vga
if exist RDCPCI5.SYS goto net
echo *** build %1 not found on the PC (exchange\out\%1) - nothing changed
goto end
:vga
if not exist VESAMINI.DRV goto incomplete
if not exist C:\M2010\B\LASTV\NUL md C:\M2010\B\LASTV
copy /y C:\WINDOWS\SYSTEM\VESAMINI.DRV C:\M2010\B\LASTV\VESAMINI.DRV > nul
copy /y C:\WINDOWS\SYSTEM\VESAMINI.VXD C:\M2010\B\LASTV\VESAMINI.VXD > nul
if exist C:\M2010\B\CURV.TXT copy /y C:\M2010\B\CURV.TXT C:\M2010\B\LASTV\ID.TXT > nul
if not exist C:\M2010\B\CURV.TXT echo the driver that was installed before TRY (no build ID)> C:\M2010\B\LASTV\ID.TXT
copy /y VESAMINI.DRV C:\M2010\B\PEND\VESAMINI.DRV > nul
copy /y VESAMINI.VXD C:\M2010\B\PEND\VESAMINI.VXD > nul
if not exist C:\WINDOWS\WININIT.INI goto newiniV
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto linesV
:newiniV
echo [rename]> C:\WINDOWS\WININIT.INI
:linesV
echo C:\WINDOWS\SYSTEM\VESAMINI.DRV=C:\M2010\B\PEND\VESAMINI.DRV>> C:\WINDOWS\WININIT.INI
echo C:\WINDOWS\SYSTEM\VESAMINI.VXD=C:\M2010\B\PEND\VESAMINI.VXD>> C:\WINDOWS\WININIT.INI
echo %1> C:\M2010\B\CURV.TXT
goto done
:net
if not exist C:\M2010\B\LASTN\NUL md C:\M2010\B\LASTN
copy /y C:\WINDOWS\SYSTEM\RDCPCI5.SYS C:\M2010\B\LASTN\RDCPCI5.SYS > nul
if exist C:\M2010\B\CURN.TXT copy /y C:\M2010\B\CURN.TXT C:\M2010\B\LASTN\ID.TXT > nul
if not exist C:\M2010\B\CURN.TXT echo the driver that was installed before TRY (no build ID)> C:\M2010\B\LASTN\ID.TXT
copy /y RDCPCI5.SYS C:\M2010\B\PEND\RDCPCI5.SYS > nul
if not exist C:\WINDOWS\WININIT.INI goto newiniN
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto linesN
:newiniN
echo [rename]> C:\WINDOWS\WININIT.INI
:linesN
echo C:\WINDOWS\SYSTEM\RDCPCI5.SYS=C:\M2010\B\PEND\RDCPCI5.SYS>> C:\WINDOWS\WININIT.INI
echo %1> C:\M2010\B\CURN.TXT
echo off> C:\M2010\NETOFF.TXT
goto done
:incomplete
echo *** build %1 is incomplete - nothing changed
goto end
:done
echo.
if exist BUILD.TXT type BUILD.TXT
echo.
echo Build %1 queued; it is installed during the next boot.
echo If it misbehaves:  BACK V / BACK N  (previous files), GOOD V / GOOD N
echo (your known-good set), or RESCUE from "Command prompt only".
goto end
:usage
echo Usage: TRY V27   or   TRY N3     (build IDs are folders in exchange\out)
:end
