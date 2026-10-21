@echo off
rem INSTALL.BAT - offline install of the open R6040.SYS (no PC / FTP needed).
rem   Run it from this folder (floppy, CD or a copy on C:) in a DOS box,
rem   with RDC's driver already installed for the adapter (RDCPCI5.SYS).
rem   Copies R6040.SYS, RDCPCI5.ORG (RDC's original 8.01.08), R6040DBG.EXE
rem   and NETUNDO.BAT to C:\M2010, then queues R6040.SYS under the name
rem   RDCPCI5.SYS in WININIT.INI.  Adapter entry, bindings and TCP/IP
rem   settings stay as they are.
rem Usage: INSTALL         install + ask to reboot
rem        INSTALL DIAG    also log the CMOS shutdown record at every boot
rem                        (AUTOEXEC.BAT line, C:\M2010\R6040DBG.TXT)
rem        INSTALL CLEAN   machine without RDC's driver: copies R6040.SYS and
rem                        R6040.INF into Windows for a Device Manager install
rem Undo:  C:\M2010\NETUNDO    (works offline; not for the CLEAN route)
rem From DOS: copy C:\M2010\NET\RDCPCI5.ORG C:\WINDOWS\SYSTEM\RDCPCI5.SYS
if not exist R6040.SYS goto wrongdir
if not exist RDCPCI5.ORG goto wrongdir
if "%1"=="CLEAN" goto clean
if "%1"=="clean" goto clean
if not exist C:\WINDOWS\SYSTEM\RDCPCI5.SYS goto clean
if not exist C:\M2010\NUL md C:\M2010
if not exist C:\M2010\NET\NUL md C:\M2010\NET
copy /y RDCPCI5.ORG C:\M2010\NET\RDCPCI5.ORG > nul
copy /y R6040.SYS C:\M2010\NET\OWNNEW.SYS > nul
copy /y R6040DBG.EXE C:\M2010\R6040DBG.EXE > nul
copy /y NETUNDO.BAT C:\M2010\NETUNDO.BAT > nul
if not exist C:\M2010\NET\INSTALLD.SYS copy C:\WINDOWS\SYSTEM\RDCPCI5.SYS C:\M2010\NET\INSTALLD.SYS > nul
if not "%1"=="DIAG" if not "%1"=="diag" goto nodiag
find "R6040DBG" C:\AUTOEXEC.BAT > nul
if not errorlevel 1 goto nodiag
if exist C:\AUTOEXEC.BAT if not exist C:\M2010\NET\AUTOEXEC.ORG copy C:\AUTOEXEC.BAT C:\M2010\NET\AUTOEXEC.ORG > nul
echo.>> C:\AUTOEXEC.BAT
echo if exist C:\M2010\R6040DBG.EXE C:\M2010\R6040DBG.EXE /LOG C:\M2010\R6040DBG.TXT>> C:\AUTOEXEC.BAT
:nodiag
if not exist C:\WINDOWS\WININIT.INI goto newini
rem only one [rename] section: WININIT ignores lines under a second header
find "[rename]" C:\WINDOWS\WININIT.INI > nul
if errorlevel 1 echo [rename]>> C:\WINDOWS\WININIT.INI
goto append
:newini
echo [rename]> C:\WINDOWS\WININIT.INI
:append
echo C:\WINDOWS\SYSTEM\RDCPCI5.SYS=C:\M2010\NET\OWNNEW.SYS>> C:\WINDOWS\WININIT.INI
echo.
echo R6040.SYS queued in WININIT.INI; installed during the next boot.
echo Back to RDC's driver at any time: C:\M2010\NETUNDO
echo Press Ctrl+C to stay, any other key to reboot...
pause > nul
rundll32.exe shell32.dll,SHExitWindowsEx 2
goto end
:wrongdir
echo *** run INSTALL from the folder that contains R6040.SYS and RDCPCI5.ORG
goto end
:clean
rem no RDC driver here: prepare a Device Manager install from R6040.INF
if not exist R6040.INF goto wrongdir
copy /y R6040.SYS C:\WINDOWS\SYSTEM\R6040.SYS > nul
copy /y R6040.INF C:\WINDOWS\INF\R6040.INF > nul
echo R6040.SYS and R6040.INF copied into Windows (no RDC driver needed).
echo Now: Control Panel - System - Device Manager - the network adapter
echo ("PCI Ethernet Controller" under Other devices, or the RDC R6040) -
echo Properties - Driver - Update Driver - "Display a list of all the
echo drivers" - Have Disk - C:\WINDOWS\INF - "RDC R6040 Fast Ethernet
echo (open R6040.SYS)".  Windows then asks to restart.
echo To go back: Device Manager - Remove the adapter, or disable it.
:end
