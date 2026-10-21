@echo off
rem SEND.BAT - dump the driver log and upload it (plus an optional note) to the PC
rem Usage: SEND            uploads m2010.txt, drvinit.txt, sysdir.txt, bootlog.txt
rem        SEND "text"     also writes your note into note.txt and uploads it
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
C:
cd \M2010
if exist m2010.txt del m2010.txt
if exist note.txt del note.txt
if exist sysdir.txt del sysdir.txt
set LOGEXE=C:\WINDOWS\m2010log.exe
if not exist C:\WINDOWS\m2010log.exe set LOGEXE=C:\M2010\m2010log.exe
%LOGEXE% C:\M2010\m2010.txt
dir C:\WINDOWS\SYSTEM\VESAMINI.* > sysdir.txt
dir C:\M2010\NEW.* >> sysdir.txt
if exist C:\WINDOWS\WININIT.INI type C:\WINDOWS\WININIT.INI >> sysdir.txt
if exist C:\WINDOWS\WININIT.BAK type C:\WINDOWS\WININIT.BAK >> sysdir.txt
if "%1"=="" goto nonote
echo %1 > note.txt
:nonote
echo open %PC%> ftpput.scr
echo user anonymous ebox@>> ftpput.scr
echo binary>> ftpput.scr
echo cd in>> ftpput.scr
echo put m2010.txt>> ftpput.scr
echo put sysdir.txt>> ftpput.scr
if exist note.txt echo put note.txt>> ftpput.scr
if exist C:\M2010\DRVINIT.TXT echo put C:\M2010\DRVINIT.TXT drvinit.txt>> ftpput.scr
if exist C:\M2010\DRVPREV.TXT echo put C:\M2010\DRVPREV.TXT drvprev.txt>> ftpput.scr
attrib -h -s -r C:\BOOTLOG.TXT > nul
if exist C:\BOOTLOG.TXT echo put C:\BOOTLOG.TXT bootlog.txt>> ftpput.scr
echo bye>> ftpput.scr
ftp -n -s:ftpput.scr
echo.
echo Uploaded m2010.txt + sysdir.txt to %PC% (in\)
set LOGEXE=
