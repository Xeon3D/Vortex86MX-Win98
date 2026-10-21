@echo off
rem BUILDS.BAT - which drivers are installed, and what BACK / GOOD would restore
echo ===== installed now
if exist C:\M2010\B\CURV.TXT type C:\M2010\B\CURV.TXT
dir C:\WINDOWS\SYSTEM\VESAMINI.* | find "VESAMINI"
if exist C:\M2010\B\CURN.TXT type C:\M2010\B\CURN.TXT
dir C:\WINDOWS\SYSTEM\RDCPCI5.SYS | find "RDCPCI5"
echo ===== BACK V (before the last TRY)
if exist C:\M2010\B\LASTV\ID.TXT type C:\M2010\B\LASTV\ID.TXT
if not exist C:\M2010\B\LASTV\VESAMINI.VXD echo    nothing yet (no TRY so far)
echo ===== BACK N
if exist C:\M2010\B\LASTN\ID.TXT type C:\M2010\B\LASTN\ID.TXT
if not exist C:\M2010\B\LASTN\RDCPCI5.SYS echo    nothing yet (no TRY so far)
echo ===== GOOD V (saved with KEEP V)
if exist C:\M2010\B\GOODV\ID.TXT type C:\M2010\B\GOODV\ID.TXT
if exist C:\M2010\B\GOODV\VESAMINI.VXD dir C:\M2010\B\GOODV\VESAMINI.* | find "VESAMINI"
if not exist C:\M2010\B\GOODV\VESAMINI.VXD echo    nothing saved - run KEEP V
echo ===== GOOD N (saved with KEEP N)
if exist C:\M2010\B\GOODN\ID.TXT type C:\M2010\B\GOODN\ID.TXT
if exist C:\M2010\B\GOODN\RDCPCI5.SYS dir C:\M2010\B\GOODN\RDCPCI5.SYS | find "RDCPCI5"
if not exist C:\M2010\B\GOODN\RDCPCI5.SYS echo    nothing saved - run KEEP N
