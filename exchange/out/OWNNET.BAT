@echo off
rem OWNNET.BAT - install our own open-source R6040 Ethernet driver
rem   (R6040.SYS, written to shut down cleanly) in place of RDC's RDCPCI5.SYS.
rem   Fetches R6040.SYS, R6040DBG.EXE, R6040.INF, NETSEND.BAT, NETUNDO.BAT
rem   (and RDCPCI5.ORG, RDC's original, if it is missing) into C:\M2010\NET,
rem   saves the installed driver once as C:\M2010\NET\INSTALLD.SYS and queues
rem   R6040.SYS under the file name RDCPCI5.SYS in WININIT.INI, so the adapter
rem   entry, its bindings and the TCP/IP settings stay exactly as they are.
rem   Also adds one line to AUTOEXEC.BAT: R6040DBG.EXE logs the driver's CMOS
rem   progress bytes from the previous shutdown to C:\M2010\R6040DBG.TXT.
rem Usage: OWNNET          fetch + queue + ask to reboot
rem        OWNNET QUEUE    fetch + queue only (WININIT.INI already has [rename])
rem   Run it after GET.BAT, not before: GET rewrites WININIT.INI.
rem Undo:  NETUNDO         puts RDC's original 8.01.08 back (works offline)
rem From DOS (Windows will not start):
rem   copy C:\M2010\NET\RDCPCI5.ORG C:\WINDOWS\SYSTEM\RDCPCI5.SYS
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
if not exist C:\M2010\NET\NUL md C:\M2010\NET
C:
cd \M2010\NET
if exist R6040.SYS del R6040.SYS
if exist R6040DBG.EXE del R6040DBG.EXE
echo open %PC%> ownnet.scr
echo user anonymous ebox@>> ownnet.scr
echo binary>> ownnet.scr
echo cd out>> ownnet.scr
echo get R6040.SYS>> ownnet.scr
echo get R6040DBG.EXE>> ownnet.scr
echo get R6040.INF>> ownnet.scr
echo get NETSEND.BAT C:\M2010\NETSEND.BAT>> ownnet.scr
echo get NETUNDO.BAT C:\M2010\NETUNDO.BAT>> ownnet.scr
if not exist RDCPCI5.ORG echo get RDCPCI5.ORG>> ownnet.scr
echo bye>> ownnet.scr
ftp -n -s:ownnet.scr
if not exist R6040.SYS goto nofiles
if not exist R6040DBG.EXE goto nofiles
if not exist RDCPCI5.ORG goto nofiles
if not exist C:\M2010\NET\INSTALLD.SYS copy C:\WINDOWS\SYSTEM\RDCPCI5.SYS C:\M2010\NET\INSTALLD.SYS > nul
copy /y R6040.SYS C:\M2010\NET\OWNNEW.SYS > nul
copy /y R6040DBG.EXE C:\M2010\R6040DBG.EXE > nul
rem keep GET.BAT from queueing RDC's patched driver over ours
echo off> C:\M2010\NETOFF.TXT
rem log the previous session's CMOS record at every boot (once)
find "R6040DBG" C:\AUTOEXEC.BAT > nul
if not errorlevel 1 goto haveauto
if exist C:\AUTOEXEC.BAT if not exist C:\M2010\NET\AUTOEXEC.ORG copy C:\AUTOEXEC.BAT C:\M2010\NET\AUTOEXEC.ORG > nul
echo.>> C:\AUTOEXEC.BAT
echo if exist C:\M2010\R6040DBG.EXE C:\M2010\R6040DBG.EXE /LOG C:\M2010\R6040DBG.TXT>> C:\AUTOEXEC.BAT
:haveauto
if "%1"=="QUEUE" goto append
if exist C:\WINDOWS\WININIT.INI goto addhdr
echo [rename]> C:\WINDOWS\WININIT.INI
goto append
:addhdr
rem only one [rename] section: WININIT ignores lines under a second header
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
:append
echo C:\WINDOWS\SYSTEM\RDCPCI5.SYS=C:\M2010\NET\OWNNEW.SYS>> C:\WINDOWS\WININIT.INI
echo.
echo Open R6040.SYS queued in WININIT.INI; installed during the next boot.
echo After the reboot: test the network (GET / SEND), then Start - Shut Down -
echo Restart.  Upload the result with NETSEND.  Back to RDC's driver: NETUNDO.
if "%1"=="QUEUE" goto end
echo Press Ctrl+C to stay, any other key to reboot...
pause > nul
rundll32.exe shell32.dll,SHExitWindowsEx 2
goto end
:nofiles
echo *** R6040.SYS / R6040DBG.EXE / RDCPCI5.ORG not downloaded - is ftpserver.py running on %PC% ?
:end
