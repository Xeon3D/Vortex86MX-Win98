@echo off
rem GET.BAT - fetch the newest driver build from the PC and install it.
rem   Files that are in use (the loaded display driver) cannot be overwritten
rem   while Windows runs, so the replacement is queued in WININIT.INI and
rem   performed by Windows at the next boot, before the driver loads.
rem Usage: GET            (fetch + queue install + ask to reboot)
rem        GET NOREBOOT   (fetch + queue install only)
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
C:
cd \M2010
if exist vesamini.drv del vesamini.drv
if exist vesamini.vxd del vesamini.vxd
echo open %PC%> ftpget.scr
echo user anonymous ebox@>> ftpget.scr
echo binary>> ftpget.scr
echo cd out>> ftpget.scr
echo get build.txt>> ftpget.scr
echo get vesamini.drv>> ftpget.scr
echo get vesamini.vxd>> ftpget.scr
echo get m2010log.exe>> ftpget.scr
echo get RDC-M2010.INF>> ftpget.scr
echo get GET.BAT>> ftpget.scr
echo get SEND.BAT>> ftpget.scr
echo bye>> ftpget.scr
ftp -n -s:ftpget.scr
if not exist vesamini.vxd goto nofiles
if exist C:\M2010\DRVPREV.TXT del C:\M2010\DRVPREV.TXT
if exist C:\M2010\DRVINIT.TXT ren C:\M2010\DRVINIT.TXT DRVPREV.TXT
if exist m2010log.exe copy /y m2010log.exe C:\WINDOWS\m2010log.exe > nul
rem GET only refreshes the tools now; drivers are installed with TRY (build ID),
rem so the vesamini.* it downloads are NOT queued (they would undo a TRY)
if exist C:\M2010\NEW.DRV del C:\M2010\NEW.DRV
if exist C:\M2010\NEW.VXD del C:\M2010\NEW.VXD
echo open %PC%> extra.scr
echo user anonymous ebox@>> extra.scr
echo binary>> extra.scr
echo cd out>> extra.scr
echo get OWNNET.BAT>> extra.scr
echo get NETSEND.BAT>> extra.scr
echo get NETUNDO.BAT>> extra.scr
echo get DOSTEST.BAT>> extra.scr
echo get SETRATES.EXE>> extra.scr
echo get TRY.BAT>> extra.scr
echo get BACK.BAT>> extra.scr
echo get GOOD.BAT>> extra.scr
echo get KEEP.BAT>> extra.scr
echo get RESCUE.BAT>> extra.scr
echo get BUILDS.BAT>> extra.scr
echo get BIGTEST.BAT>> extra.scr
echo get UP.BAT>> extra.scr
echo get NSET.BAT>> extra.scr
echo get R6040DBG.EXE>> extra.scr
echo get INSTAGT.BAT>> extra.scr
echo cd TWEAKUI>> extra.scr
echo lcd C:\M2010\TWEAKUI>> extra.scr
echo get TWEAKUI.INF>> extra.scr
echo get TWEAKUI.CPL>> extra.scr
echo get TWEAKUI.HLP>> extra.scr
echo get TWEAKUI.CNT>> extra.scr
echo cd ..>> extra.scr
echo lcd C:\M2010>> extra.scr
echo bye>> extra.scr
if not exist C:\M2010\TWEAKUI\NUL md C:\M2010\TWEAKUI
ftp -n -s:extra.scr > nul
echo.
echo Tools updated (TRY, BACK, GOOD, KEEP, RESCUE, BUILDS, BIGTEST, SEND, INSTAGT, ...).
echo TweakUI: right-click C:\M2010\TWEAKUI\TWEAKUI.INF and choose Install.
echo Nothing was installed: install driver builds with TRY (e.g. TRY V27R), see them with BUILDS.
goto end
:nofiles
echo *** nothing downloaded - is ftpserver.py running on %PC% ?
:end
