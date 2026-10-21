@echo off
rem UNDIAG.BAT - remove the R6040DBG line that OWNNET DIAG / INSTALL DIAG
rem   added to C:\AUTOEXEC.BAT.  Keeps a backup in C:\M2010\NET\AUTOEXEC.BAK.
rem   (The driver's own CMOS checkpoints: registry value R6040Diag = "0".)
if not exist C:\AUTOEXEC.BAT goto none
find "R6040DBG" C:\AUTOEXEC.BAT > nul
if errorlevel 1 goto none
if not exist C:\M2010\NUL md C:\M2010
if not exist C:\M2010\NET\NUL md C:\M2010\NET
copy /y C:\AUTOEXEC.BAT C:\M2010\NET\AUTOEXEC.BAK > nul
type C:\M2010\NET\AUTOEXEC.BAK | find /v "R6040DBG" > C:\AUTOEXEC.BAT
echo R6040DBG line removed from C:\AUTOEXEC.BAT (backup: C:\M2010\NET\AUTOEXEC.BAK)
goto end
:none
echo No R6040DBG line in C:\AUTOEXEC.BAT - nothing to do.
:end
