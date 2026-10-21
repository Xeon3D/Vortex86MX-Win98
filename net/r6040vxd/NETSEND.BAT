@echo off
rem NETSEND.BAT - upload the R6040 driver test results to the PC (in\)
rem   r6040dbg.txt  CMOS records logged at each boot by AUTOEXEC.BAT
rem   r6040now.txt  the record of the running session + installed driver
rem   netreg.txt    the network adapter registry keys
rem Usage: NETSEND            or   NETSEND "note text"
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
C:
cd \M2010
if exist r6040now.txt del r6040now.txt
if exist netreg.txt del netreg.txt
if exist netnote.txt del netnote.txt
if exist C:\M2010\R6040DBG.EXE C:\M2010\R6040DBG.EXE > r6040now.txt
dir C:\WINDOWS\SYSTEM\RDCPCI5.SYS >> r6040now.txt
dir C:\WINDOWS\SYSTEM\R6040.SYS >> r6040now.txt
dir C:\M2010\NET >> r6040now.txt
if exist C:\WINDOWS\WININIT.INI type C:\WINDOWS\WININIT.INI >> r6040now.txt
if exist C:\M2010\NETOFF.TXT echo NETOFF.TXT present >> r6040now.txt
regedit /e C:\M2010\netreg.txt "HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Net"
if "%1"=="" goto nonote
echo %1 > netnote.txt
:nonote
echo open %PC%> netsend.scr
echo user anonymous ebox@>> netsend.scr
echo binary>> netsend.scr
echo cd in>> netsend.scr
echo put r6040now.txt>> netsend.scr
if exist netreg.txt echo put netreg.txt>> netsend.scr
if exist netnote.txt echo put netnote.txt>> netsend.scr
if exist C:\M2010\R6040DBG.TXT echo put C:\M2010\R6040DBG.TXT r6040dbg.txt>> netsend.scr
echo bye>> netsend.scr
ftp -n -s:netsend.scr
echo.
echo Uploaded r6040now.txt (+ r6040dbg.txt, netreg.txt) to %PC% (in\)
