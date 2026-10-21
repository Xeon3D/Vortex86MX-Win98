@echo off
rem NETUNDO.BAT - put the original R6040 driver RDCPCI5.SYS (8.01.08) back.
rem   Works offline: uses C:\M2010\NET\RDCPCI5.ORG (original from the PC),
rem   or C:\M2010\NET\INSTALLD.SYS (the copy NETFIX saved) if that is missing.
rem   Queues the restore in WININIT.INI and creates C:\M2010\NETOFF.TXT.
rem From DOS (Windows will not start):
rem   copy C:\M2010\NET\RDCPCI5.ORG C:\WINDOWS\SYSTEM\RDCPCI5.SYS
C:
if not exist C:\M2010\NET\NUL goto nofile
cd \M2010\NET
if exist NETOLD.SYS del NETOLD.SYS
if exist RDCPCI5.ORG copy /y RDCPCI5.ORG NETOLD.SYS > nul
if not exist NETOLD.SYS if exist INSTALLD.SYS copy /y INSTALLD.SYS NETOLD.SYS > nul
if not exist NETOLD.SYS goto nofile
if exist C:\WINDOWS\WININIT.INI goto addhdr
echo [rename]> C:\WINDOWS\WININIT.INI
goto append
:addhdr
rem only one [rename] section: WININIT ignores lines under a second header
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
:append
echo C:\WINDOWS\SYSTEM\RDCPCI5.SYS=C:\M2010\NET\NETOLD.SYS>> C:\WINDOWS\WININIT.INI
echo off> C:\M2010\NETOFF.TXT
echo Original RDCPCI5.SYS queued in WININIT.INI; restored during the next boot.
echo Press Ctrl+C to stay, any other key to reboot...
pause > nul
rundll32.exe shell32.dll,SHExitWindowsEx 2
goto end
:nofile
echo *** no original driver in C:\M2010\NET - nothing to restore
:end
