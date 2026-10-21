@echo off
rem INSTAGT.BAT - install the unattended test agent EBOXAGT.EXE
rem   INSTAGT       download EBOXAGT.EXE, start it at every logon, start it now
rem   INSTAGT OFF   keep it from starting (creates C:\M2010\AGT\OFF.TXT)
rem   The agent only connects out to the PC (FTP): it runs jobs from
rem   exchange\out\JOB and reports to exchange\in (HB.TXT, RUN\...).
rem   Its own log is C:\M2010\AGT\AGT.LOG.
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
if not exist C:\M2010\AGT\NUL md C:\M2010\AGT
C:
cd \M2010\AGT
if "%1"=="OFF" goto off
if "%1"=="off" goto off
if exist OFF.TXT del OFF.TXT
if exist EBOXAGT.NEW del EBOXAGT.NEW
echo open %PC%> agt.scr
echo user anonymous ebox@>> agt.scr
echo binary>> agt.scr
echo cd out>> agt.scr
echo get EBOXAGT.EXE EBOXAGT.NEW>> agt.scr
echo get AGTON.REG>> agt.scr
echo bye>> agt.scr
ftp -n -s:agt.scr > nul
if not exist EBOXAGT.NEW goto nofile
if exist AGTON.REG start /w regedit /s C:\M2010\AGT\AGTON.REG
if exist EBOXAGT.EXE del EBOXAGT.EXE
if exist EBOXAGT.EXE goto running
ren EBOXAGT.NEW EBOXAGT.EXE
start C:\M2010\AGT\EBOXAGT.EXE
echo Agent installed and started; it also starts at every logon.
echo On the PC:  python testauto\harness.py status
goto end
:running
if not exist C:\WINDOWS\WININIT.INI goto newini
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto line
:newini
echo [rename]> C:\WINDOWS\WININIT.INI
:line
echo C:\M2010\AGT\EBOXAGT.EXE=C:\M2010\AGT\EBOXAGT.NEW>> C:\WINDOWS\WININIT.INI
echo The agent is running, so the new EBOXAGT.EXE is queued for the next boot.
goto end
:off
echo agent off> OFF.TXT
echo Agent switched off from the next boot on (INSTAGT turns it on again).
goto end
:nofile
echo *** download failed - is ftpserver.py running on %PC% ?
:end
