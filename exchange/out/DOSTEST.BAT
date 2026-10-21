@echo off
rem DOSTEST.BAT - fetch DOS graphics test programs into C:\DOSTEST and unpack
rem   them with 7-Zip. Run the programs from a FULL-SCREEN MS-DOS prompt.
rem   DOSBENCH : C:\DOSTEST\DOSBENCH\DOSBENCH.BAT (menu: Doom timedemo,
rem              3DBench, PCPBench, Chris's SVGA bench, FastVid, Quake)
rem   KEEN4    : C:\DOSTEST\KEEN4\KEEN4.EXE   (EGA, smooth scrolling)
rem   LEMMINGS : C:\DOSTEST\LEMMINGS\VGALEMMI.EXE (VGA 16 colours)
set PC=10.0.0.239
set Z=C:\PROGRA~1\7-ZIP\7Z.EXE
if not exist C:\DOSTEST\NUL md C:\DOSTEST
C:
cd \DOSTEST
echo open %PC%> dostest.scr
echo user anonymous ebox@>> dostest.scr
echo binary>> dostest.scr
echo cd out>> dostest.scr
echo get DOSBENCH.ZIP>> dostest.scr
echo get KEEN4.ZIP>> dostest.scr
echo get LEMMINGS.ZIP>> dostest.scr
echo bye>> dostest.scr
ftp -n -s:dostest.scr
if not exist %Z% goto noz
%Z% x -y -oC:\DOSTEST\DOSBENCH DOSBENCH.ZIP > nul
%Z% x -y -oC:\DOSTEST\KEEN4 KEEN4.ZIP > nul
%Z% x -y -oC:\DOSTEST\LEMMINGS LEMMINGS.ZIP > nul
echo.
echo Unpacked into C:\DOSTEST: DOSBENCH, KEEN4, LEMMINGS
goto end
:noz
echo.
echo 7Z.EXE not found at %Z% - open the .ZIP files in C:\DOSTEST with 7-Zip
echo and extract each one into a folder of the same name.
:end
set Z=
